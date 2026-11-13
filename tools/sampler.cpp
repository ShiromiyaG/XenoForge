// The sampling profiler (sampler.h).

#include "sampler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// after windows.h
#include <dbghelp.h>
#include <mmsystem.h>
// carried by the object, so whatever links boot (a port, the SDK's package) links these too
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#include <tlhelp32.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace xf::sampler {

namespace {

struct Sampled {
    HANDLE handle = nullptr;
    uint64_t cycles = 0; // the thread's cycle count at the last sample
    uint64_t stack_low = 0, stack_high = 0; // its stack's bounds (0: not known, its stack is not walked)
};

// (ntdll's NtQueryInformationThread, for a thread's environment block: its
// stack's bounds are there; found at run time, ntdll not being linked)
void stack_bounds(Sampled &s) {
    using Query = long(__stdcall *)(HANDLE, int, void *, unsigned long, unsigned long *);
    static const Query NtQueryInformationThread =
        reinterpret_cast<Query>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
    if (!NtQueryInformationThread) return;
    struct {
        long exit_status;
        void *teb;
        void *process, *thread;
        uintptr_t affinity;
        long priority, base_priority;
    } basic{};
    if (NtQueryInformationThread(s.handle, 0, &basic, sizeof(basic), nullptr) < 0 || !basic.teb) return;
    const NT_TIB *tib = static_cast<const NT_TIB *>(basic.teb);
    s.stack_low = uint64_t(reinterpret_cast<uintptr_t>(tib->StackLimit));
    s.stack_high = uint64_t(reinterpret_cast<uintptr_t>(tib->StackBase));
}

std::atomic<bool> running{false};
std::thread worker;
std::mutex mutex;
std::map<uint64_t, uint64_t> weights; // rip -> samples of a thread there
uint64_t total = 0;
uint64_t cycles_spent = 0; // by the sampled threads, in all
// with one thread sampled: each sample's stack (return addresses, the
// interrupted rip first) and its cycles
std::vector<std::pair<std::vector<uint64_t>, uint64_t>> stacks;
// name_generated's table, by host address
std::vector<std::pair<uint64_t, uint32_t>> generated;
// scope's function name's part: with stacks, only the samples under such a function count
std::string scope_name;

// a generated function's name for a host address with no symbol, and how far in
bool generated_at(uint64_t address, std::string &name, uint64_t &displacement) {
    auto it = std::upper_bound(generated.begin(), generated.end(), std::make_pair(address, UINT32_MAX));
    if (it == generated.begin()) return false;
    --it;
    // (beyond 64 KB of the nearest start: not one of them)
    if (address - it->first > 0x10000) return false;
    char b[24];
    std::snprintf(b, sizeof(b), "xf_%08X", it->second);
    name = b, displacement = address - it->first;
    return true;
}
// Past 64 KB of the nearest generated function and within 16 MB: named after it, marked far (a long
// function's code, or an op's helper the compiler kept out of line next to it), so that a game
// library's long functions are not one nameless "module+?".
bool generated_far(uint64_t address, std::string &name, uint64_t &displacement) {
    auto it = std::upper_bound(generated.begin(), generated.end(), std::make_pair(address, UINT32_MAX));
    if (it == generated.begin()) return false;
    --it;
    if (address - it->first >= 0x1000000) return false;
    char b[40];
    std::snprintf(b, sizeof(b), "xf_%08X (far)", it->second);
    name = b, displacement = address - it->first;
    return true;
}

// The suspended thread's stack by x64 unwind data (RtlVirtualUnwind), at
// most 48 frames; a frame without unwind data is a leaf (its return address
// at rsp). Into an array of the caller's, no memory taken: the thread may
// have been suspended inside the heap, holding its lock, and whoever
// allocates before it is resumed waits for it for ever. Only while the stack
// pointer lies in the thread's stack (low to high): a frame taken for a leaf
// that is none (code without unwind data that moved the stack pointer)
// leaves it anywhere, and reading there faulted the whole run.
//
// The unwind itself may still read anywhere: a function that keeps a frame
// register is unwound through it, and where the frames before were taken
// wrongly it holds anything. So the walk is guarded: a handler put before
// the process's own (which reports any fault as the run's crash) takes an
// access violation of the walking thread back to where the walk began,
// which then ends with the frames it had.
constexpr int FRAMES = 48;
thread_local volatile int walk_state = 0; // 1: walking; 2: a fault took it back
thread_local volatile int walk_depth = 0;
thread_local CONTEXT walk_resume;

LONG CALLBACK walk_guard(EXCEPTION_POINTERS *e) {
    if (walk_state != 1 || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    walk_state = 2;
    *e->ContextRecord = walk_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int walk(CONTEXT ctx, uint64_t out[FRAMES], uint64_t low, uint64_t high) {
    static const bool guarded = AddVectoredExceptionHandler(1, walk_guard) != nullptr;
    (void)guarded;
    walk_state = 0, walk_depth = 0;
    RtlCaptureContext(&walk_resume);
    if (walk_state == 2) { // (here again after a fault)
        walk_state = 0;
        return walk_depth;
    }
    walk_state = 1;
    int depth = 0;
    for (; depth < FRAMES && ctx.Rip; depth++) {
        out[depth] = ctx.Rip;
        walk_depth = depth + 1;
        if (ctx.Rsp < low || ctx.Rsp + 256 > high || (ctx.Rsp & 7)) {
            walk_state = 0;
            return depth + 1;
        }
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!fn) {
            if (!ctx.Rsp) break;
            ctx.Rip = *reinterpret_cast<const DWORD64 *>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void *handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
        }
    }
    walk_state = 0;
    return depth;
}

void sample_loop(unsigned interval_us, DWORD only) {
    timeBeginPeriod(1);
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    std::map<DWORD, Sampled> threads;
    std::vector<std::pair<std::vector<uint64_t>, uint64_t>> local_stacks;
    std::vector<uint64_t> stack;
    auto refresh = [&] {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 e{sizeof(e)};
        for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
            if (e.th32OwnerProcessID != process || e.th32ThreadID == self || threads.count(e.th32ThreadID)) continue;
            if (only && e.th32ThreadID != only) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                  e.th32ThreadID);
            if (!h) continue;
            Sampled s{h, 0};
            QueryThreadCycleTime(h, &s.cycles);
            if (only) stack_bounds(s);
            threads[e.th32ThreadID] = s;
        }
        CloseHandle(snap);
    };
    refresh();
    auto last_refresh = std::chrono::steady_clock::now();
    std::map<uint64_t, uint64_t> local;
    uint64_t local_total = 0, local_cycles = 0;
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
        for (auto it = threads.begin(); it != threads.end();) {
            Sampled &s = it->second;
            ULONG64 cycles = 0;
            if (!QueryThreadCycleTime(s.handle, &cycles)) {
                CloseHandle(s.handle);
                it = threads.erase(it);
                continue;
            }
            uint64_t spent = cycles - s.cycles;
            s.cycles = cycles;
            local_cycles += spent;
            // a thread that ran since the last sample: where it is now, one
            // sample (those found in a wait are left out of the report: a
            // thread on a processor at the sample's instant is found there
            // as often as it runs, however short its bursts)
            if (spent > 0 && SuspendThread(s.handle) != DWORD(-1)) {
                // (nothing allocated until the thread runs again: walk's note)
                CONTEXT ctx{};
                ctx.ContextFlags = only ? CONTEXT_FULL : CONTEXT_CONTROL;
                uint64_t frames[FRAMES];
                int depth = 0;
                const bool got = GetThreadContext(s.handle, &ctx);
                if (got && only) depth = walk(ctx, frames, s.stack_low, s.stack_high);
                ResumeThread(s.handle);
                if (got) {
                    local[ctx.Rip] += 1, local_total += 1;
                    if (only) {
                        stack.assign(frames, frames + depth);
                        local_stacks.emplace_back(stack, 1);
                    }
                }
            }
            ++it;
        }
        if (std::chrono::steady_clock::now() - last_refresh > std::chrono::milliseconds(200)) {
            refresh();
            last_refresh = std::chrono::steady_clock::now();
        }
    }
    for (auto &[id, s] : threads) CloseHandle(s.handle);
    timeEndPeriod(1);
    std::lock_guard lock(mutex);
    weights = std::move(local);
    total = local_total;
    cycles_spent = local_cycles;
    stacks = std::move(local_stacks);
}

bool generated_name(std::string n) {
    // a module's functions sit in its namespace (xfm_default_xex::xf_822D5D18)
    if (size_t colons = n.rfind("::"); colons != std::string::npos) n = n.substr(colons + 2);
    if (n.ends_with(" (far)")) n.resize(n.size() - 6);
    // <prefix>_<8 hex digits>: a generated function (xf_822D5D18, xfo_..., a DLL's prefix)
    if (n.size() < 10) return false;
    size_t u = n.size() - 9;
    if (n[u] != '_') return false;
    for (size_t k = u + 1; k < n.size(); k++)
        if (!std::isxdigit(uint8_t(n[k]))) return false;
    for (size_t k = 0; k < u; k++)
        if (!std::isalnum(uint8_t(n[k]))) return false;
    return true;
}

// A thread that ran since the last sample is sampled where it is, and may by
// then be waiting again (the syscall's return): one that works in short
// bursts between waits is found so nearly every time. Those samples say
// nothing of where its time went and are left out; the ones that find it
// in a burst come as often as it runs, each counting the same.
const char *const WAITING = "found waiting";
bool waiting_name(const std::string &n) {
    if (n.size() < 3 || (n.compare(0, 2, "Nt") != 0 && n.compare(0, 2, "Zw") != 0)) return false;
    return n.find("Wait") != std::string::npos || n.find("DelayExecution") != std::string::npos ||
           n.find("RemoveIoCompletion") != std::string::npos;
}

std::string category(const std::string &name, const std::string &module) {
    if (generated_name(name)) return "generated code";
    if (waiting_name(name)) return WAITING;
    if (name.find("ppc::ops::") != std::string::npos) return "instructions' cold paths";
    if (name.find("ppc::interpret") != std::string::npos || name.find("ppc_interp") != std::string::npos ||
        name.find("ppc::Interpreter") != std::string::npos)
        return "interpreter";
    if (name.find("ppc::") != std::string::npos) return "ppc runtime (dispatch, unwind)";
    if (name.find("kernel::") != std::string::npos) return "kernel";
    if (name.find("render::") != std::string::npos || name.find("gpu::") != std::string::npos ||
        name.find("vk::") != std::string::npos)
        return "GPU emulation";
    if (module.find("ntdll") != std::string::npos || module.find("KERNELBASE") != std::string::npos ||
        module.find("kernel32") != std::string::npos)
        return "host OS (" + module + ")";
    if (module.find("boot") == std::string::npos && module.find("player") == std::string::npos)
        return "other module (" + module + ")";
    return "other";
}

} // namespace

unsigned long current_thread() { return GetCurrentThreadId(); }

double process_cpu_seconds() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    auto value = [](FILETIME t) { return double(uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) * 1e-7; };
    return value(kernel) + value(user);
}

void name_generated(const void *const *hosts, const unsigned *guests, size_t count) {
    std::lock_guard lock(mutex);
    generated.clear();
    generated.reserve(count);
    for (size_t k = 0; k < count; k++) generated.push_back({uint64_t(uintptr_t(hosts[k])), uint32_t(guests[k])});
    std::sort(generated.begin(), generated.end());
}

void scope(const char *function) {
    std::lock_guard lock(mutex);
    scope_name = function ? function : "";
}

bool start(unsigned interval_us, unsigned long only_thread) {
    if (running.exchange(true)) return true;
    worker = std::thread(sample_loop, interval_us, DWORD(only_thread));
    return true;
}

void stop_and_report(size_t top) {
    if (!running.exchange(false)) return;
    worker.join();
    std::lock_guard lock(mutex);
    if (!total) return (void)std::printf("profile: no samples\n");
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    struct Fn {
        uint64_t weight = 0;
        std::string cat;
        std::map<uint64_t, uint64_t> offsets;
        std::map<uint64_t, uint64_t> addresses; // the samples' own (overloads share a name, not a base)
        uint64_t base = 0; // the function's address (for its hot offsets' source lines)
    };
    std::map<std::string, Fn> fns;
    std::map<std::string, uint64_t> cats;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    if (!scope_name.empty() && !stacks.empty()) {
        // the samples under the scope's function alone: the others dropped, the totals made again
        std::map<uint64_t, bool> scoped; // by return address
        auto in_scope = [&](uint64_t address) {
            auto it = scoped.find(address);
            if (it != scoped.end()) return it->second;
            auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
            std::memset(buffer, 0, sizeof(buffer));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 displacement = 0;
            bool is = SymFromAddr(process, address - 1, &displacement, sym) &&
                      std::strstr(sym->Name, scope_name.c_str()) != nullptr;
            return scoped[address] = is;
        };
        std::vector<std::pair<std::vector<uint64_t>, uint64_t>> kept;
        weights.clear();
        uint64_t all = total;
        total = 0;
        for (auto &[frames, w] : stacks) {
            bool under = false;
            for (size_t k = 1; k < frames.size() && !under; k++) under = in_scope(frames[k]);
            if (!under) continue;
            weights[frames[0]] += w, total += w;
            kept.emplace_back(std::move(frames), w);
        }
        stacks = std::move(kept);
        std::printf("profile: under %s alone, %.1f%% of the thread's samples\n", scope_name.c_str(),
                    100.0 * double(total) / double(all));
        if (!total) return SymCleanup(process), void();
    }
    std::set<uint64_t> waiting_at;
    for (auto &[rip, w] : weights) {
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
        std::memset(buffer, 0, sizeof(buffer));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 displacement = 0;
        std::string name = "?", module = "?";
        IMAGEHLP_MODULE64 mod{sizeof(mod)};
        if (SymGetModuleInfo64(process, rip, &mod)) module = mod.ModuleName;
        if (SymFromAddr(process, rip, &displacement, sym)) {
            name = sym->Name;
        } else if (!generated_at(rip, name, displacement)) {
            // else the offsets are from the module's base (its RVAs)
            if (!generated_far(rip, name, displacement)) {
                name = module + "+?";
                if (mod.BaseOfImage && rip >= mod.BaseOfImage) displacement = rip - mod.BaseOfImage;
            }
        }
        Fn &f = fns[name];
        if (f.cat.empty()) f.cat = category(name, module);
        if (!f.base) f.base = rip - displacement;
        f.weight += w;
        f.offsets[displacement] += w;
        f.addresses[rip] += w;
        cats[f.cat] += w;
        if (f.cat == WAITING) waiting_at.insert(rip);
    }
    // the samples of threads found waiting, left out
    const uint64_t waiting = cats.count(WAITING) ? cats[WAITING] : 0;
    cats.erase(WAITING);
    std::erase_if(fns, [](const auto &f) { return f.second.cat == WAITING; });
    std::erase_if(stacks, [&](const auto &st) { return !st.first.empty() && waiting_at.count(st.first[0]); });
    total -= waiting;
    std::printf("profile: where the CPU time went (%llu Mcycles spent; %llu samples of threads running, %llu more "
                "found waiting)\n",
                (unsigned long long)(cycles_spent / 1000000), (unsigned long long)total, (unsigned long long)waiting);
    if (!total) return SymCleanup(process), void();
    std::vector<std::pair<uint64_t, std::string>> by_cat;
    for (auto &[c, w] : cats) by_cat.push_back({w, c});
    std::sort(by_cat.rbegin(), by_cat.rend());
    for (auto &[w, c] : by_cat) std::printf("  %5.1f%%  %s\n", 100.0 * double(w) / double(total), c.c_str());
    std::vector<std::pair<uint64_t, std::string>> by_fn;
    for (auto &[n, f] : fns) by_fn.push_back({f.weight, n});
    std::sort(by_fn.rbegin(), by_fn.rend());
    std::printf("profile: top functions\n");
    const char *lines_wanted = std::getenv("XF_PROFILE_LINES");
    const size_t lines_shown = lines_wanted ? size_t(std::strtoul(lines_wanted, nullptr, 10)) : 3;
    for (size_t k = 0; k < by_fn.size() && k < top; k++) {
        const Fn &f = fns[by_fn[k].second];
        std::vector<std::pair<uint64_t, uint64_t>> offs;
        for (auto &[o, w] : f.offsets) offs.push_back({w, o});
        std::sort(offs.rbegin(), offs.rend());
        std::string hot;
        if (lines_shown > 3) {
            // by source line instead (XF_PROFILE_LINES=N): the function's N most sampled lines with
            // their shares of the whole, the code inlined into it under its own files' lines
            std::map<std::string, uint64_t> by_line;
            for (auto &[address, w] : f.addresses) {
                IMAGEHLP_LINE64 line{sizeof(line)};
                DWORD column = 0;
                std::string where = "?";
                if (SymGetLineFromAddr64(process, address, &column, &line) && line.FileName) {
                    const char *file = std::max(std::strrchr(line.FileName, '\\'), std::strrchr(line.FileName, '/'));
                    where = std::string(file ? file + 1 : line.FileName) + ":" + std::to_string(line.LineNumber);
                }
                by_line[where] += w;
            }
            std::vector<std::pair<uint64_t, std::string>> lines;
            for (auto &[where, w] : by_line) lines.push_back({w, where});
            std::sort(lines.rbegin(), lines.rend());
            for (size_t j = 0; j < lines.size() && j < lines_shown; j++) {
                char b[160];
                std::snprintf(b, sizeof(b), " %s %.2f%%", lines[j].second.c_str(),
                              100.0 * double(lines[j].first) / double(total));
                hot += b;
            }
            offs.clear();
        }
        for (size_t j = 0; j < offs.size() && j < 3; j++) {
            char b[160];
            // with the source line where the PDB has it (file name only)
            IMAGEHLP_LINE64 line{sizeof(line)};
            DWORD column = 0;
            if (f.base && SymGetLineFromAddr64(process, f.base + offs[j].second, &column, &line) && line.FileName) {
                const char *file = std::max(std::strrchr(line.FileName, '\\'), std::strrchr(line.FileName, '/'));
                std::snprintf(b, sizeof(b), " +0x%llx (%s:%lu)", (unsigned long long)offs[j].second,
                              file ? file + 1 : line.FileName, (unsigned long)line.LineNumber);
            } else {
                std::snprintf(b, sizeof(b), " +0x%llx", (unsigned long long)offs[j].second);
            }
            hot += b;
        }
        // a far function's hottest address as its module's RVA, for a disassembly of the module
        if (IMAGEHLP_MODULE64 mod{sizeof(mod)};
            !offs.empty() && by_fn[k].second.ends_with(" (far)") && SymGetModuleInfo64(process, f.base, &mod)) {
            char b[48];
            std::snprintf(b, sizeof(b), " rva 0x%llx", (unsigned long long)(f.base + offs[0].second - mod.BaseOfImage));
            hot += b;
        }
        std::printf("  %5.2f%%  %-48s %s [%s ]\n", 100.0 * double(f.weight) / double(total), by_fn[k].second.c_str(),
                    f.cat.c_str(), hot.c_str());
    }
    if (stacks.empty()) return SymCleanup(process), void();
    // one thread's stacks: each function's share of the samples it was on
    // the stack for (counted once a sample, however deep it recurs)
    std::map<uint64_t, std::string> names;
    auto name_of = [&](uint64_t address) -> const std::string & {
        auto it = names.find(address);
        if (it != names.end()) return it->second;
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
        std::memset(buffer, 0, sizeof(buffer));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 displacement = 0;
        std::string name;
        // (a return address is the instruction after the call: look one byte back)
        if (SymFromAddr(process, address - 1, &displacement, sym)) {
            name = sym->Name;
        } else if (generated_at(address - 1, name, displacement) || generated_far(address - 1, name, displacement)) {
        } else {
            IMAGEHLP_MODULE64 mod{sizeof(mod)};
            name = SymGetModuleInfo64(process, address, &mod) ? std::string(mod.ModuleName) + "+?" : "?";
        }
        return names.emplace(address, std::move(name)).first->second;
    };
    std::map<std::string, uint64_t> inclusive;
    for (auto &[frames, w] : stacks) {
        std::vector<const std::string *> seen;
        for (uint64_t address : frames) {
            const std::string &n = name_of(address);
            if (std::find(seen.begin(), seen.end(), &n) != seen.end()) continue;
            bool again = false;
            for (const std::string *s : seen) again |= *s == n;
            if (again) continue;
            seen.push_back(&n);
            inclusive[n] += w;
        }
    }
    // who called the functions the samples were in: each with its caller and the caller's line
    std::map<std::string, uint64_t> callers;
    for (auto &[frames, w] : stacks) {
        if (frames.size() < 2) continue;
        std::string pair = name_of(frames[0] + 1) + " <- " + name_of(frames[1]);
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD column = 0;
        if (SymGetLineFromAddr64(process, frames[1] - 1, &column, &line) && line.FileName) {
            const char *file = std::max(std::strrchr(line.FileName, '\\'), std::strrchr(line.FileName, '/'));
            pair += std::string(" (") + (file ? file + 1 : line.FileName) + ":" + std::to_string(line.LineNumber) + ")";
        }
        callers[pair] += w;
    }
    SymCleanup(process);
    std::vector<std::pair<uint64_t, std::string>> by_caller;
    for (auto &[n, w] : callers) by_caller.push_back({w, n});
    std::sort(by_caller.rbegin(), by_caller.rend());
    std::printf("profile: callers (the function sampled and what called it)\n");
    for (size_t k = 0; k < by_caller.size() && k < 2 * top; k++)
        std::printf("  %5.2f%%  %s\n", 100.0 * double(by_caller[k].first) / double(total), by_caller[k].second.c_str());
    // XF_PROFILE_CHAINS=<name>[,<name>...]: for each function whose name holds one of them, the
    // chains of callers above it (six frames from the first such frame of a stack), by their samples
    if (const char *wanted = std::getenv("XF_PROFILE_CHAINS"); wanted && *wanted) {
        std::vector<std::string> patterns;
        for (std::string rest = wanted; !rest.empty();) {
            const size_t comma = rest.find(',');
            patterns.push_back(rest.substr(0, comma));
            rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
        }
        for (const std::string &pattern : patterns) {
            std::map<std::string, uint64_t> chains;
            uint64_t all = 0;
            for (auto &[frames, w] : stacks) {
                for (size_t k = 0; k < frames.size(); k++) {
                    if (name_of(k ? frames[k] : frames[k] + 1).find(pattern) == std::string::npos) continue;
                    std::string chain;
                    for (size_t j = k; j < frames.size() && j < k + 6; j++) {
                        std::string frame = name_of(j ? frames[j] : frames[j] + 1);
                        if (frame.size() > 60) frame.resize(60);
                        chain += (j > k ? " <- " : "") + frame;
                    }
                    chains[chain] += w, all += w;
                    break;
                }
            }
            std::vector<std::pair<uint64_t, std::string>> sorted;
            for (auto &[c, w] : chains) sorted.push_back({w, c});
            std::sort(sorted.rbegin(), sorted.rend());
            std::printf("profile: chains of %s (%.2f%% of the samples)\n", pattern.c_str(),
                        100.0 * double(all) / double(total));
            for (size_t k = 0; k < sorted.size() && k < 8; k++)
                std::printf("  %5.2f%%  %s\n", 100.0 * double(sorted[k].first) / double(total), sorted[k].second.c_str());
        }
    }
    std::vector<std::pair<uint64_t, std::string>> by_inclusive;
    for (auto &[n, w] : inclusive) by_inclusive.push_back({w, n});
    std::sort(by_inclusive.rbegin(), by_inclusive.rend());
    std::printf("profile: inclusive (the sampled thread's stacks: each function with all it calls)\n");
    for (size_t k = 0; k < by_inclusive.size() && k < 2 * top; k++)
        std::printf("  %5.2f%%  %s\n", 100.0 * double(by_inclusive[k].first) / double(total), by_inclusive[k].second.c_str());
}

} // namespace xf::sampler

#else

#include <sys/resource.h>

namespace xf::sampler {
// TODO: POSIX sampling (a profiling timer's signal reading the interrupted context). Until then the
// sampler does not start, and only the process's processor time is reported.
void scope(const char *) {}
bool start(unsigned, unsigned long) { return false; }
unsigned long current_thread() { return 0; }
void name_generated(const void *const *, const unsigned *, size_t) {}
double process_cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    auto value = [](timeval t) { return double(t.tv_sec) + double(t.tv_usec) * 1e-6; };
    return value(usage.ru_utime) + value(usage.ru_stime);
}
void stop_and_report(size_t) {}
} // namespace xf::sampler

#endif
