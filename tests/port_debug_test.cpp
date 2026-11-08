// Debugging for porters (plan E5; docs/sdk.md, "Reloading the hooks";
// docs/runtime.md, "The port's hooks"): the hook frames a context carries
// (what the reports put between guest frames), the hook counts
// (--hook-stats), owners and their hooks dropped together, the state that
// outlives a reload, the guest paused at safe points (Kernel::pause), and a
// hook module (port_module_hooks.cpp, built as a DLL beside this test) loaded
// and reloaded twice, its counts going on. The guest program is port_test's,
// interpreted.

#include "kernel_harness.h"
#include "port_program.h"

#include "objects.h"
#include "xenoforge/hooks.h"
#include "xenoforge/port.h"
#include "xenoforge/port_module.h"

#include <chrono>
#include <thread>

using namespace xf;
using namespace port_program;
using port::Context;

namespace {

uint64_t run(KernelHarness &h, uint32_t f, uint64_t r3) {
    h.c->r[3] = r3;
    h.k->call(*h.c, f);
    return h.c->r[3];
}

bool map_program(KernelHarness &h) {
    Xex xex;
    xex.base_address = BASE;
    xex.image = image_bytes();
    std::string error;
    return h.k->map_image(xex, error);
}

uint64_t calls_of(uint32_t address, hooks::Kind kind, const std::string &hook) {
    for (const hooks::Stat &s : hooks::stats())
        if (s.address == address && s.kind == kind && s.hook == hook) return s.calls;
    return ~0ull;
}

// ---- the hook frames ----

std::vector<hooks::FrameInfo> seen;
void look(Context &c) { seen = hooks::frames(c); }
void call_add(Context &c, const port::Original &) { port::call(c, ADD); }

void frames() {
    KernelHarness h;
    CHECK(h.init() && map_program(h));
    port::before(ADD, look, "look");
    port::replace(PLAIN, call_add, "call_add");
    // ADD's hook alone: one frame, its kind, names, the r1 and LR it was entered with
    seen.clear();
    h.c->lr = 0x82100204;
    run(h, ADD, 0);
    CHECK(seen.size() == 1 && seen[0].kind == hooks::Kind::Before && seen[0].hook == "look" && !seen[0].in_guest);
    CHECK(seen.size() == 1 && seen[0].address == ADD && seen[0].sp == uint32_t(h.c->r[1]));
    CHECK(seen.size() == 1 && seen[0].fn == reinterpret_cast<const void *>(&look));
    // through PLAIN's replacement, which calls ADD: two, the outer one in guest code
    seen.clear();
    CHECK(run(h, PLAIN, 5) == 6);
    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0].kind == hooks::Kind::Before && seen[0].lr == port::CALL_RETURN && !seen[0].in_guest);
        CHECK(seen[1].kind == hooks::Kind::Replace && seen[1].hook == "call_add" && seen[1].in_guest);
        CHECK(hooks::describe(seen[1]) == "replace hook call_add on 0x82100500");
    }
    // none once it returned
    CHECK(hooks::frames(*h.c).empty());
    port::remove(ADD), port::remove(PLAIN);
}

// ---- the counts ----

void count_me(Context &) {}
uint32_t mid_on(Context &) { return port::CONTINUE; }

void stats() {
    KernelHarness h;
    CHECK(h.init() && map_program(h));
    port::before(ADD, count_me, "count_me");
    port::after(ADD, count_me, "count_me");
    port::mid(MID_AT, mid_on, "mid_on");
    // off: nothing counted
    hooks::set_stats(false);
    run(h, ADD, 0);
    CHECK(calls_of(ADD, hooks::Kind::Before, "count_me") == 0);
    hooks::set_stats(true);
    for (int i = 0; i < 3; i++) run(h, ADD, 0);
    CHECK(run(h, MID, 0) == 112);
    CHECK(calls_of(ADD, hooks::Kind::Before, "count_me") == 3 && calls_of(ADD, hooks::Kind::After, "count_me") == 3);
    CHECK(calls_of(MID_AT, hooks::Kind::Mid, "mid_on") == 1);
    // registered again (removed, then added): the count goes on
    port::remove(ADD);
    port::before(ADD, count_me, "count_me");
    run(h, ADD, 0);
    CHECK(calls_of(ADD, hooks::Kind::Before, "count_me") == 4);
    CHECK(hooks::stats_report().find("before  count_me") != std::string::npos);
    port::remove(ADD), port::remove(MID_AT);
}

// ---- owners and the state kept ----

int owned_calls = 0, exe_calls = 0;
void owned(Context &) { owned_calls++; }
void exe(Context &) { exe_calls++; }

void owners() {
    KernelHarness h;
    CHECK(h.init() && map_program(h));
    hooks::Owner *o = hooks::make_owner("test module");
    hooks::Owner *before = hooks::set_registering(o);
    port::before(ADD, owned, "owned");
    hooks::set_registering(before);
    port::before(ADD, exe, "exe");
    run(h, ADD, 0);
    CHECK(owned_calls == 1 && exe_calls == 1);
    // the owner's hooks dropped, the executable's kept
    hooks::remove_owner(o);
    run(h, ADD, 0);
    CHECK(owned_calls == 1 && exe_calls == 2);
    CHECK(hooks::drained(o, 100));
    const hooks::Entry *e = hooks::find(ADD);
    CHECK(e && e->owners.empty() && e->before.size() == 1);
    port::remove(ADD);

    // the persistent blocks: zeroed once, the same after, anew when the size changes
    bool made = false;
    auto *a = static_cast<uint32_t *>(port::persistent("owners_test", 8, &made));
    CHECK(made && a[0] == 0 && a[1] == 0);
    a[0] = 7;
    CHECK(port::persistent("owners_test", 8, &made) == a && !made);
    auto *b = static_cast<uint32_t *>(port::persistent("owners_test", 16, &made));
    CHECK(made && b[0] == 0);
    struct Two {
        int x = 5, y = 6;
    };
    CHECK(port::persistent<Two>("owners_test_two").y == 6);
}

// ---- the guest paused ----

void pause() {
    KernelHarness h;
    CHECK(h.init());
    kernel::Kernel &k = *h.k;
    k.objects().set_tls(64, 0, 0, 0);
    // mr r5, r3; li r3, 0; loop: addi r3, r3, 1; lwz r4, 0(r5); cmpwi r4, 0; beq loop; blr
    const uint32_t code = h.memory->system_alloc(0x1000), flag = h.alloc(16);
    uint32_t at = code;
    for (uint32_t w : {0x7C651B78u, 0x38600000u, 0x38630001u, 0x80850000u, 0x2C040000u, 0x4182FFF4u, 0x4E800020u})
        k.store32(at, w), at += 4;
    k.store32(flag, 0);
    kernel::Objects &o = k.objects();
    kernel::Thread::Params p;
    p.stack_size = 64 * 1024;
    p.start_address = code;
    p.start_context = flag;
    p.name = "spinner";
    uint32_t status;
    kernel::Ref<kernel::Thread> t = o.create_thread(p, status);
    CHECK(t && o.scheduler->start(*t));
    if (!t) return;
    auto count = [&] { return uint32_t(t->context->r[3]); };
    // running: its count goes up
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (count() < 1000 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    CHECK(count() >= 1000);
    // paused at its loop's back edge (the interpreter's yield point): no more
    CHECK(k.pause(2000));
    CHECK(k.pause_unparked() == 0);
    const uint32_t stopped = count();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(count() == stopped);
    // let go: it goes on, then ends when the flag is set
    k.resume();
    while (count() == stopped && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    CHECK(count() != stopped);
    k.store32(flag, 1);
    o.scheduler->join(*t);
}

// ---- a hook module, loaded and reloaded ----

void module() {
    KernelHarness h;
    CHECK(h.init() && map_program(h));
    hooks::set_stats(true);
    kernel::PortModule m;
    std::string error;
    CHECK(m.load(XF_TEST_HOOK_MODULE, error));
    if (!error.empty()) std::printf("%s\n", error.c_str());
    // its hooks run: this load's count and the count across loads
    run(h, ADD, 0);
    run(h, ADD, 0);
    CHECK(h.c->r[30] == 2 && h.c->r[31] == 2);
    CHECK(run(h, PLAIN, 1) == 1003);
    // reloaded twice: its statics start over, the persistent state and the counts go on
    for (int load = 2; load <= 3; load++) {
        std::string line;
        CHECK(m.reload(*h.k, error, &line));
        CHECK(line.find("reloaded (load " + std::to_string(load) + ")") != std::string::npos);
        run(h, ADD, 0);
        CHECK(h.c->r[30] == 1 && h.c->r[31] == uint64_t(load + 1));
        CHECK(run(h, PLAIN, 1) == 1003);
        CHECK(calls_of(ADD, hooks::Kind::Before, "module_before") == uint64_t(load + 1));
        CHECK(calls_of(PLAIN, hooks::Kind::Replace, "module_replace") == uint64_t(load));
        // one module's hooks at a time, the old copy unloaded (no thread runs it)
        const hooks::Entry *e = hooks::find(ADD);
        CHECK(e && e->before.size() == 1 && e->owners.size() == 1);
        CHECK(m.collect() == 0);
    }
    CHECK(m.reloads() == 2);
    // a file that is not a module: refused, the hooks kept
    kernel::PortModule bad;
    CHECK(!bad.load(XF_TEST_NOT_A_MODULE, error) && !error.empty());
    run(h, ADD, 0);
    CHECK(h.c->r[30] == 2);
}

} // namespace

int main() {
    frames();
    stats();
    owners();
    pause();
    module();
    return finish("port_debug");
}
