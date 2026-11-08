// The port's side of the recompiler (docs/recompiler.md, "The project file"
// and "Replacing and hooking"): the TOML reader, the project file (ours and
// rexglue's function list), the names from an MSVC map and a Ghidra list,
// the emission with and without a project file, and the hooks (replace,
// before, after, mid-function; at link time and at run time) on a guest
// program run as generated code (port_gen's, built with this test) and in the
// interpreter, and under the lockstep check.

#include "kernel_harness.h"
#include "port_program.h"

#include "xenoforge/hooks.h"
#include "xenoforge/lockstep.h"
#include "xenoforge/port.h"
#include "xenoforge/toml.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace xfm_m {
extern const xf::ppc::GeneratedModule module;
}

using namespace xf;
using namespace port_program;
using port::Context;

namespace {

// ---- the readers ----

void toml_reader() {
    toml::Value root;
    std::string error;
    CHECK(toml::parse(R"(# a comment
top = 0x82000000   # trailing
neg = -12
big = 1_000
flt = 2.5
yes = true
str = "a\"b\u00e9"
lit = 'C:\path'
list = [1, 2,
        3, # inside
]
inline = { a = 1, b.c = "x" }

[t."quoted.key"]
v = 1
[t.other]
w = [ { x = 1 }, { x = 2 } ]

[[arr]]
n = 1
[[arr]]
n = 2
[arr.sub]
m = 3
)",
                      root, error));
    CHECK(error.empty());
    CHECK(root.find("top")->integer == 0x82000000);
    CHECK(root.find("neg")->integer == -12);
    CHECK(root.find("big")->integer == 1000);
    CHECK(root.find("flt")->number == 2.5);
    CHECK(root.find("yes")->boolean);
    CHECK(root.find("str")->string == "a\"b\xC3\xA9");
    CHECK(root.find("lit")->string == "C:\\path");
    CHECK(root.find("list")->array.size() == 3);
    CHECK(root.find("inline")->find("b")->find("c")->string == "x");
    CHECK(root.find("t")->find("quoted.key")->find("v")->integer == 1);
    CHECK(root.find("t")->find("other")->find("w")->array[1].find("x")->integer == 2);
    const toml::Value *arr = root.find("arr");
    CHECK(arr && arr->array.size() == 2 && arr->array[1].find("sub")->find("m")->integer == 3);
    // errors, with their line
    CHECK(!toml::parse("a = 1\na = 2\n", root, error) && error.starts_with("line 2"));
    CHECK(!toml::parse("a = \"open\n", root, error));
    CHECK(!toml::parse("x = 1979-05-27\n", root, error));
    CHECK(!toml::parse("i = { a = 1 }\n[i]\nb = 2\n", root, error));
}

void project_file() {
    // the schema's example (docs/recompiler.md, "The project file"), names
    // files aside
    const char *text = R"(hooks = [0x82012340]
skip = [0x82013000]

[project]
name = "my_game"

[functions]
"0x82010000" = { name = "Camera_Update", end = 0x82010100, hook = true }
"0x82010200" = {}
"0x82010400" = { size = 0x40 }
"0x82010800" = { skip = true, start = false }

[names]
"0x82011000" = "Render"

[[jump_tables]]
bctr = 0x82010080
targets = [0x82010090, 0x820100A0]

[[mid_hooks]]
address = 0x82010010
jumps = [0x82010020]

[modules."Other.xex"]
hooks = [0x82500000]
[modules."Other.xex".names]
"0x82500000" = "OtherEntry"
)";
    recomp::Project p;
    std::string error;
    CHECK(recomp::parse_project(text, ".", p, error));
    CHECK(p.name == "my_game");
    const recomp::ProjectModule *m = p.find("DEFAULT.XEX");
    CHECK(m != nullptr);
    if (m) {
        CHECK(m->starts.size() == 3);
        CHECK(m->starts.at(0x82010000) == 0x82010100);
        CHECK(m->starts.at(0x82010200) == 0);
        CHECK(m->starts.at(0x82010400) == 0x82010440);
        CHECK(m->hooks.count(0x82012340) && m->hooks.count(0x82010000));
        CHECK(m->skip.count(0x82013000) && m->skip.count(0x82010800));
        CHECK(m->names.at(0x82010000) == "Camera_Update" && m->names.at(0x82011000) == "Render");
        CHECK(m->jump_tables.at(0x82010080).size() == 2);
        CHECK(m->mid_hooks.at(0x82010010) == std::vector<uint32_t>{0x82010020});
    }
    const recomp::ProjectModule *other = p.find("other.xex");
    CHECK(other && other->hooks.count(0x82500000) && other->names.at(0x82500000) == "OtherEntry");
    CHECK(p.warnings.empty());

    // rexglue's function list as a port has it, and its other tables
    CHECK(recomp::parse_project(R"(# starts the analysis missed
[functions]
"0x82012340" = {}
"0x82012400" = { size = 0x20, parent = 0x82012000 }

[[switch_tables]]
address = 0x82012480
register = 11
labels = [0x82012490]

[[midasm_hook]]
name = "MyHook"
address = 0x82012410
registers = ["r3"]
jump_address_on_true = 0x82012418
)",
                                ".", p, error));
    m = p.find("default.xex");
    CHECK(m && m->starts.size() == 2 && m->starts.at(0x82012400) == 0x82012420);
    CHECK(m && m->jump_tables.at(0x82012480) == std::vector<uint32_t>{0x82012490});
    CHECK(m && m->mid_hooks.at(0x82012410) == std::vector<uint32_t>{0x82012418} && m->names.at(0x82012410) == "MyHook");
    CHECK(p.warnings.size() == 1); // parent: not read
    CHECK(!recomp::parse_project("[functions]\n\"zz\" = {}\n", ".", p, error));
    CHECK(!recomp::parse_project("hooks = [\"x\"]\n", ".", p, error));
}

void name_files() {
    std::map<uint32_t, std::string> names;
    CHECK(recomp::read_msvc_map(R"( Address         Publics by Value              Rva+Base       Lib:Object

 0000:00000000       ___safe_se_handler_count   00000000     <absolute>
 0001:00000000       ?Update@Camera@@QAAXM@Z    82010000 f   camera.obj
 0001:00000100       _main                      82010100 f   main.obj
 0002:00000000       ??_C@_05@hello             82200000     strings.obj

 Static symbols

 0001:00000200       _helper                    82010200 f   main.obj
)",
                                names) == 3);
    CHECK(names.at(0x82010000) == "?Update@Camera@@QAAXM@Z" && names.at(0x82010200) == "_helper");
    CHECK(!names.count(0x82200000));
    names.clear();
    CHECK(recomp::read_ghidra_functions("\"Name\",\"Location\",\"Function Signature\"\n"
                                        "\"Camera::Update\",\"82010000\",\"void Camera::Update(float)\"\n"
                                        "\"FUN_82010200\",\"82010200\",\"undefined FUN_82010200(void)\"\n"
                                        "\"main\",\"ram:82010100\",\"int main(void)\"\n",
                                        names) == 2);
    CHECK(names.at(0x82010000) == "Camera::Update" && names.at(0x82010100) == "main");
    names.clear();
    CHECK(recomp::read_ghidra_functions("82010000 Update\nRender 0x82010400\n", names) == 2);
    CHECK(names.at(0x82010400) == "Render");
}

// ---- the emission ----

std::string read(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void emission() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "xf_port_test_emission";
    fs::remove_all(dir);
    recomp::Image image = port_program::image();
    recomp::EmitOptions options;
    options.discover.scan_pointers = false;
    std::string error;
    auto emit = [&](const recomp::Project *project, const char *name) {
        options.project = project;
        recomp::EmittedModule m;
        CHECK(recomp::emit_image(image, "port_test.xex", image_digest(image.bytes), dir, name, options, m, error));
        return m;
    };
    emit(nullptr, "none");
    recomp::Project empty, full;
    CHECK(recomp::parse_project("[project]\nname = \"nothing\"\n", ".", empty, error));
    emit(&empty, "empty");
    CHECK(recomp::parse_project(PROJECT, ".", full, error));
    recomp::EmittedModule m = emit(&full, "full");
    CHECK(m.hooks == 2 && m.mid_hooks == 1 && m.names == 2);
    CHECK(m.warnings.empty());
    // nothing of a project, nothing different (but the namespace's name)
    auto renamed = [](std::string s, const std::string &from) {
        for (size_t at; (at = s.find(from)) != std::string::npos;) s.replace(at, from.size(), "NS");
        return s;
    };
    auto same = [&](const char *file) {
        std::string a = renamed(read(dir / "none" / file), "xfm_none");
        return !a.empty() && a == renamed(read(dir / "empty" / file), "xfm_empty");
    };
    CHECK(same("functions_0.cpp"));
    CHECK(same("table.cpp"));
    CHECK(!fs::exists(dir / "none" / "names.h"));
    const std::string plain = read(dir / "none" / "functions_0.cpp"), table = read(dir / "none" / "table.cpp");
    CHECK(plain.find("xfo_") == std::string::npos && plain.find("HOOK") == std::string::npos);
    CHECK(table.find("HookSlot") == std::string::npos && table.find("resumes, 0};") != std::string::npos);
    // with it: the slots, the wrapper, the mid hook, the names
    const std::string hooked = read(dir / "full" / "functions_0.cpp"), hooked_table = read(dir / "full" / "table.cpp");
    CHECK(hooked.find("XF_FN(xfo_82100100) { // add_one") != std::string::npos);
    CHECK(hooked.find("XF_FN(xf_82100100) { XF_HOOKED(xf_hook_82100100, xfo_82100100); }") != std::string::npos);
    CHECK(hooked.find("extern ::xf::ppc::HookSlot xf_mid_82100304;") != std::string::npos);
    CHECK(hooked.find("case 0x8210030C: goto L_8210030C;") != std::string::npos);
    CHECK(hooked.find("case 0x82100308: goto L_82100308;") != std::string::npos);
    CHECK(hooked_table.find("::xf::ppc::HookSlot xf_hook_82100100 = {0x82100100u, 0x8u, xfo_82100100, nullptr};") !=
          std::string::npos);
    CHECK(hooked_table.find("hooks, 3, names, 2};") != std::string::npos);
    CHECK(read(dir / "full" / "names.h").find("inline constexpr uint32_t add_one = 0x82100100u;") != std::string::npos);
    // an unknown address, said
    recomp::Project wrong;
    CHECK(recomp::parse_project("hooks = [0x82200000]\n[[mid_hooks]]\naddress = 0x82200080\n[project]\nmodule = "
                                "\"port_test.xex\"\n",
                                ".", wrong, error));
    m = emit(&wrong, "wrong");
    CHECK(m.warnings.size() == 2);
    fs::remove_all(dir);
}

// ---- the hooks, run ----

int static_calls = 0, before_calls = 0, after_calls = 0, plain_calls = 0, mid_calls = 0;
uint32_t mid_answer = port::CONTINUE;

// link time: a function the project file hooks
XF_BEFORE(STATIC, count_static) { static_calls++; }

void plus100(Context &c, const port::Original &original) {
    original(c);
    c.r[3] += 100;
}
void count_before(Context &) { before_calls++; }
void count_after(Context &) { after_calls++; }
void count_plain(Context &) { plain_calls++; }
uint32_t mid(Context &) {
    mid_calls++;
    return mid_answer;
}

uint64_t run(KernelHarness &h, uint32_t f, uint64_t r3) {
    h.c->r[3] = r3;
    h.k->call(*h.c, f);
    return h.c->r[3];
}

void hooks_run(bool generated) {
    KernelHarness h;
    CHECK(h.init());
    Xex xex;
    xex.base_address = BASE;
    xex.image = image_bytes();
    std::string error;
    CHECK(h.k->map_image(xex, error));
    if (generated) {
        const ppc::GeneratedModule *modules[] = {&xfm_m::module};
        h.k->set_generated_modules(modules, 1);
        CHECK(h.k->add_module_code(xex).starts_with("generated code"));
    }
    const char *mode = generated ? "generated" : "interpreted";

    // as it is, but for the link-time hook
    int s = static_calls;
    CHECK(run(h, CALLER, 10) == 11);
    CHECK(run(h, MID, 0) == 112);
    CHECK(run(h, TAIL, 5) == 6);
    CHECK(run(h, CALLER2, 1) == 3);
    CHECK(run(h, STATIC, 0) == 3 && static_calls == s + 1);

    // replace, before and after, at run time, on a function with a slot
    port::replace(ADD, plus100, "add_one");
    port::before(ADD, count_before);
    port::after(ADD, count_after);
    before_calls = after_calls = 0;
    CHECK(run(h, CALLER, 10) == 111); // a direct call
    CHECK(run(h, TAIL, 5) == 106);    // a tail branch
    CHECK(run(h, ADD, 0) == 101);     // a dispatch
    CHECK(before_calls == 3 && after_calls == 3);
    CHECK(port::name_of(ADD) == "add_one");

    // mid-function: on, a label, the next instruction, a return
    port::mid(MID_AT, mid);
    mid_calls = 0;
    mid_answer = port::CONTINUE;
    CHECK(run(h, MID, 0) == 112);
    mid_answer = MID_JUMP;
    CHECK(run(h, MID, 0) == 100);
    mid_answer = MID_AT + 4;
    CHECK(run(h, MID, 0) == 107);
    mid_answer = port::RETURN;
    CHECK(run(h, MID, 55) == 55);
    CHECK(mid_calls == 4);
    mid_answer = port::CONTINUE;

    // a function the project file does not list: dispatch reaches its hooks,
    // a direct call in the generated code does not (the interpreter's does)
    port::before(PLAIN, count_plain);
    plain_calls = 0;
    CHECK(run(h, CALLER2, 1) == 3);
    CHECK(plain_calls == (generated ? 0 : 1));
    CHECK(run(h, PLAIN, 1) == 3);
    CHECK(plain_calls == (generated ? 1 : 2));

    // the lockstep check: the hooks end segments as imports do, nothing differs
    if (generated) {
        kernel::LockstepOptions options;
        options.functions = {CALLER, MID};
        CHECK(h.k->enable_lockstep(options, error));
        before_calls = mid_calls = 0;
        CHECK(run(h, CALLER, 10) == 111);
        CHECK(run(h, MID, 0) == 112);
        CHECK(before_calls == 1 && mid_calls == 1); // run once each, for real
        kernel::LockstepCounts n = h.k->lockstep_counts();
        CHECK(n.calls == 2 && n.segments == 4 && n.matched == 4 && n.mismatched == 0 && n.given_up.empty());
        if (n.matched != 4) std::printf("%s", h.k->lockstep_report().c_str());
    }

    // all dropped: as it was
    for (uint32_t a : {ADD, MID_AT, PLAIN}) port::remove(a);
    CHECK(run(h, CALLER, 10) == 11);
    CHECK(run(h, MID, 0) == 112);
    CHECK(run(h, TAIL, 5) == 6);
    CHECK(h.k->interpreted_entries() == 0 || !generated);
    std::printf("port_test (%s): %llu interpreted entries\n", mode, (unsigned long long)h.k->interpreted_entries());
}

void memory_helpers() {
    KernelHarness h;
    CHECK(h.init());
    uint32_t a = h.alloc(64);
    Context &c = *h.c;
    port::store<uint32_t>(c, a, 0x11223344u);
    CHECK(h.k->load32(a) == 0x11223344u);
    port::store<float>(c, a + 4, 1.5f);
    CHECK(port::load<float>(c, a + 4) == 1.5f);
    port::store<uint16_t>(c, a + 8, 0xABCD);
    CHECK(h.k->load16(a + 8) == 0xABCD);
    struct Guest {
        port::be<uint32_t> flags;
        port::be<float> x;
        port::ptr<Guest> next;
    };
    Guest &g = port::ref<Guest>(c, a + 16);
    g.flags = 7;
    g.x = 2.0f;
    g.next = a + 16;
    CHECK(h.k->load32(a + 16) == 7 && port::load<float>(c, a + 20) == 2.0f && h.k->load32(a + 24) == a + 16);
    CHECK(g.next.get(c) == &g && g.next.get(c)->flags == 7u);
    c.r[4] = 9;
    CHECK(port::arg(c, 1) == 9);
}

} // namespace

int main() {
    toml_reader();
    project_file();
    name_files();
    emission();
    memory_helpers();
    hooks_run(false);
    hooks_run(true);
    return finish("port");
}
