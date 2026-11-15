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
#include "xenoforge/run.h"
#include "xenoforge/xdk_d3d.h"
#include "xenoforge/toml.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace xfm_m {
extern const xf::ppc::GeneratedModule module;
// a project name's alias (port_gen's functions_0.cpp: xfo_82100100, ADD's body)
void add_one(xf::ppc::Context &);
} // namespace xfm_m

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
    // the names as symbols: aliases of the bodies (a hooked one's original)
    CHECK(hooked.find("XF_FN(add_one) __attribute__((alias(\"?xfo_82100100@xfm_full@@YAXAEAUContext@ppc@xf@@@Z\")));") !=
          std::string::npos);
    CHECK(hooked.find("XF_FN(caller) __attribute__((alias(\"_ZN8xfm_full11xf_82100200ERN2xf3ppc7ContextE\")));") !=
          std::string::npos);
    CHECK(plain.find("alias") == std::string::npos);
    // linked, the body's code (port_gen's module, compiled into this test;
    // compared by what it does: an incremental link gives each symbol a
    // jump of its own)
    {
        auto c = std::make_unique<Context>();
        std::memset(c.get(), 0, sizeof(Context));
        c->r[3] = 5;
        xfm_m::add_one(*c);
        CHECK(c->r[3] == 6);
    }
    {
        std::set<std::string> used;
        CHECK(recomp::alias_identifier("?Update@Camera@@QAAXXZ", 0x82010000, used) == "_Update_Camera__QAAXXZ");
        CHECK(recomp::alias_identifier("Camera::Update", 0x82010000, used) == "Camera__Update");
        CHECK(recomp::alias_identifier("Camera::Update", 0x82010100, used) == "Camera__Update_82010100");
        CHECK(recomp::alias_identifier("delete", 0x82010200, used) == "delete_82010200");
        CHECK(recomp::alias_identifier("BC", 0x82010300, used) == "BC_82010300");
    }
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

// The port's options declared once (run.h): a choice by name or index, a range clamped, the default kept,
// the builder's fields, and port::option reading them as numbers
void port_options() {
    bool wide = true;
    int aspect = 0, fps = 30;
    std::vector<PortOption> options;
    options.push_back(PortOption::choice("aspect", "Aspect", aspect, {"auto", "16:9", "21:9"}, {"Auto", "", "Ultrawide"})
                          .describe("The picture's shape.")
                          .in("Video"));
    options.push_back(PortOption::integer_range("fps", "Frame rate", fps, 30, 120).in("Video").needs_restart());
    options.push_back(PortOption::boolean("wide", "Wide", wide));
    CHECK(options[0].kind == PortOption::Kind::Choice && options[0].max == 2 && options[0].category == "Video");
    CHECK(options[0].description == "The picture's shape." && options[1].restart && !options[0].restart);
    CHECK(options[0].set_text("21:9") && aspect == 2 && options[0].set_text("1") && aspect == 1);
    CHECK(!options[0].set_text("4:3") && aspect == 1);
    CHECK(options[0].choice_label(0) == "Auto" && options[0].choice_label(1) == "16:9");
    CHECK(options[1].set_text("500") && fps == 120 && options[1].default_value == 30);
    CHECK(options[2].set_text("off") && !wide && options[2].default_value == 1);
    options[1].set_value(options[1].default_value);
    CHECK(fps == 30);
    port::set_option_reader([&](const char *key) {
        for (const PortOption &o : options)
            if (o.key == key) return o.value();
        return std::nan("");
    });
    CHECK(port::option("aspect") == 1.0 && port::option("fps") == 30.0 && port::option("wide") == 0.0);
    CHECK(std::isnan(port::option("missing")));
    port::set_option_reader({});
    CHECK(std::isnan(port::option("aspect")));
}

// xdk_d3d.h on a device and shader objects made here in the 21250 layout: the registers the D3D works out at
// a draw from the shaders' headers, the bool and loop constants, and the literals the GPU keeps between draws
// (laid when the shader changes, the game's dirty groups from the shadow first)
void xdk_d3d_reader() {
    KernelHarness h;
    CHECK(h.init());
    Context &c = *h.c;
    const xf::xdk::D3DLayout *layout = xf::xdk::d3d_layout(21250);
    CHECK(layout != nullptr);
    if (!layout) return;
    const xf::xdk::D3DLayout &l = *layout;
    const xf::xdk::D3DReader reader(l);
    const uint32_t dev = h.alloc(0x4000), data = h.alloc(0x200);
    auto zero = [&](uint32_t at, uint32_t bytes) { std::memset(port::host(c, at), 0, bytes); };
    zero(dev, 0x4000), zero(data, 0x200);
    constexpr uint32_t TWO_PI = 0x40C90FDB;
    // a vertex shader with a literal for c250.xyzw (2 pi, 1, 2, 3), one with none, a pixel shader
    auto vertex_shader = [&](bool literal, uint32_t program) {
        const uint32_t o = h.alloc(0x600);
        zero(o, 0x600);
        port::store<uint32_t>(c, o + l.vs_data, data);
        port::store<uint32_t>(c, o + l.vs_descriptor, 0x80); // the descriptor at header + 0x80
        port::store<uint32_t>(c, o + l.vs_header + 0x80 + 8, program);
        port::store<uint32_t>(c, o + l.vs_header + 0x14, 0x100); // the literal block at header + 0x100
        const uint32_t block = o + l.vs_header + 0x100;
        port::store<uint32_t>(c, block + 0x10, literal ? 8 : 0);
        port::store<uint16_t>(c, block + 0x14, 250), port::store<uint16_t>(c, block + 0x16, 4);
        port::store<uint32_t>(c, block + 0x18, 0x40);
        return o;
    };
    for (uint32_t k = 0; k < 4; k++) port::store<uint32_t>(c, data + 0x40 + 4 * k, k ? k : TWO_PI);
    const uint32_t vs_a = vertex_shader(true, 0x00110001), vs_b = vertex_shader(false, 0x00010002);
    const uint32_t ps = h.alloc(0x200);
    zero(ps, 0x200);
    port::store<uint32_t>(c, ps + l.ps_descriptor, 0x40);
    port::store<uint32_t>(c, ps + l.ps_header + 0x40 + 8, 0x10000200);
    port::store<uint32_t>(c, ps + l.ps_header + 0x40 + 12, 0x4);
    port::store<uint32_t>(c, ps + l.ps_header + 0x40 + 24, 0x00030003);
    // RB_MODECONTROL as the shadow last had it: depth only (stale)
    port::store<uint32_t>(c, dev + l.blocks[4].offset + 8 * 4, 5);
    port::store<uint32_t>(c, dev + l.vertex_shader, vs_a), port::store<uint32_t>(c, dev + l.pixel_shader, ps);

    uint32_t regs[0x400];
    reader.draw_registers(c, dev, regs);
    CHECK(regs[0x180] == 0x10110201 && regs[0x181] == 4 && regs[0x182] == 0x00030003 && regs[0x208] == 4);
    port::store<uint32_t>(c, dev + l.pixel_shader, 0);
    reader.draw_registers(c, dev, regs);
    CHECK(regs[0x180] == 0x00110001 && regs[0x181] == 0 && regs[0x208] == 5);
    port::store<uint32_t>(c, dev + l.pixel_shader, ps);

    uint32_t bools[8], loops[32];
    port::store<uint32_t>(c, dev + l.bool_constants + 4, 0x80000001);
    port::store<uint32_t>(c, dev + l.bool_constants + 32 + 4 * 31, 0x00010203);
    reader.bool_loop_constants(c, dev, bools, loops);
    CHECK(bools[1] == 0x80000001 && bools[0] == 0 && loops[31] == 0x00010203);

    static uint32_t vs[1024], psc[1024];
    xf::xdk::D3DReader::LiteralCarry carry;
    auto draw = [&](uint32_t vertex, uint64_t dirty) {
        port::store<uint32_t>(c, dev + l.vertex_shader, vertex);
        port::store<uint64_t>(c, dev + l.dirty_constants, dirty);
        reader.constants(c, dev, vs, psc, &carry);
        port::store<uint64_t>(c, dev + l.dirty_constants, 0); // (the D3D's draw sends and clears them)
        return vs[250 * 4];
    };
    const uint64_t c250_group = 1ull << (63 - 250 / 4); // c248-c251
    CHECK(draw(vs_a, 0) == TWO_PI && vs[250 * 4 + 3] == 3);
    CHECK(draw(vs_b, 0) == TWO_PI);          // no literal of its own: what the last one left
    CHECK(draw(vs_b, c250_group) == 0);      // the game wrote the group: the shadow's
    CHECK(draw(vs_b, 0) == 0);
    CHECK(draw(vs_a, 0) == TWO_PI);          // set again: its literals
    CHECK(draw(vs_a, c250_group) == 0);      // the same shader: the game's write stands
    CHECK(draw(vs_b, 0) == 0 && draw(vs_a, 0) == TWO_PI);
    // without a carry: the literals laid over at every draw, the shadow elsewhere
    port::store<uint32_t>(c, dev + l.vertex_shader, vs_b);
    reader.constants(c, dev, vs, psc);
    CHECK(vs[250 * 4] == 0);
}

// invoke and GuestStack (docs/sdk.md, "Calling the game") on code placed in guest memory, interpreted:
// arguments in r3-r10 and f1-f13 counted apart, results from r3 and f1, every register back as it was,
// temporaries that a callee's own frame and saves leave alone
void typed_calls() {
    KernelHarness h;
    CHECK(h.init());
    Context &c = *h.c;
    const uint32_t code = h.alloc(256);
    auto put = [&](uint32_t at, std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) port::store<uint32_t>(c, at, w), at += 4;
    };
    const uint32_t mix = code, first = code + 0x40, framed = code + 0x80;
    // mix: r3 += r4 + r4; f1 *= f2
    put(mix, {0x7C632214, 0x7C632214, 0xFC2100B2, 0x4E800020});
    // first: r3 = the byte at r3
    put(first, {0x88630000, 0x4E800020});
    // framed: a frame of its own and a save below r1 (as compiled code does), then the byte at r3
    put(framed, {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x38000000, 0x9001FFF0, 0x88630000, 0x38210060,
                 0x8181FFF8, 0x7D8803A6, 0x4E800020});
    c.r[3] = 77, c.r[4] = 88, c.f[1] = 9.5, c.cr[2] = 1, c.ctr = 5;
    const uint64_t sp = c.r[1];
    CHECK((port::invoke<uint32_t>(c, mix, 3u, 4u, 1.5, 2.0)) == 11u);
    CHECK((port::invoke<double>(c, mix, 3u, 4u, 1.5, 2.0)) == 3.0);
    CHECK((port::invoke<float>(c, mix, 0u, 0u, 2.0f, 4.0f)) == 8.0f);
    port::invoke(c, mix, 1u, 1u); // void
    CHECK(c.r[3] == 77 && c.r[4] == 88 && c.f[1] == 9.5 && c.cr[2] == 1 && c.ctr == 5 && c.r[1] == sp);
    {
        port::GuestStack stack(c);
        const uint32_t hi = stack.string("Hi");
        const uint32_t wide = stack.wstring(u"Ok");
        struct Pair {
            port::be<uint32_t> a, b;
        };
        const uint32_t pair = stack.value(Pair{{0x01020304}, {0x05060708}});
        CHECK(hi < sp && wide < sp && pair < sp && c.r[1] < pair && (c.r[1] & 15) == 0);
        CHECK(port::load<uint32_t>(c, uint32_t(c.r[1])) == uint32_t(sp)); // the back chain
        CHECK((port::invoke<uint32_t>(c, first, hi)) == 'H');
        CHECK((port::invoke<uint32_t>(c, framed, hi + 1)) == 'i');
        CHECK((port::invoke<uint32_t>(c, framed, wide + 1)) == 'O');
        CHECK(port::load<uint16_t>(c, wide + 2) == u'k' && port::load<uint32_t>(c, pair + 4) == 0x05060708u);
        CHECK(std::string(reinterpret_cast<const char *>(port::host(c, hi))) == "Hi"); // left alone by the callee
    }
    CHECK(c.r[1] == sp);
}

} // namespace

int main() {
    toml_reader();
    project_file();
    name_files();
    emission();
    memory_helpers();
    typed_calls();
    port_options();
    xdk_d3d_reader();
    hooks_run(false);
    hooks_run(true);
    return finish("port");
}
