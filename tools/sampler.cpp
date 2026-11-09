// The sampling profiler (sampler.h).

#include "sampler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
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
#include <thread>

namespace xf::sampler {

namespace {

struct Sampled {
    HANDLE handle = nullptr;
    uint64_t cycles = 0; // the thread's cycle count at the last sample
};

std::atomic<bool> running{false};
std::thread worker;
std::mutex mutex;
std::map<uint64_t, uint64_t> weights; // rip -> cycles spent there (the interval's)
uint64_t total = 0;
// with one thread sampled: each sample's stack (return addresses, the
// interrupted rip first) and its cycles
std::vector<std::pair<std::vector<uint64_t>, uint64_t>> stacks;
// name_generated's table, by host address
std::vector<std::pair<uint64_t, uint32_t>> generated;

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

// The suspended thread's stack by x64 unwind data (RtlVirtualUnwind), at
// most 48 frames; a frame without unwind data is a leaf (its return address
// at rsp)
void walk(CONTEXT ctx, std::vector<uint64_t> &out) {
    out.clear();
    for (int depth = 0; depth < 48 && ctx.Rip; depth++) {
        out.push_back(ctx.Rip);
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
            threads[e.th32ThreadID] = s;
        }
        CloseHandle(snap);
    };
    refresh();
    auto last_refresh = std::chrono::steady_clock::now();
    std::map<uint64_t, uint64_t> local;
    uint64_t local_total = 0;
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
            // a thread that ran: where it is now stands for the interval's cycles
            if (spent > 0 && SuspendThread(s.handle) != DWORD(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = only ? CONTEXT_FULL : CONTEXT_CONTROL;
                if (GetThreadContext(s.handle, &ctx)) {
                    local[ctx.Rip] += spent, local_total += spent;
                    if (only) {
                        walk(ctx, stack);
                        local_stacks.emplace_back(stack, spent);
                    }
                }
                ResumeThread(s.handle);
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
    stacks = std::move(local_stacks);
}

bool generated_name(std::string n) {
    // a module's functions sit in its namespace (xfm_default_xex::xf_822D5D18)
    if (size_t colons = n.rfind("::"); colons != std::string::npos) n = n.substr(colons + 2);
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

std::string category(const std::string &name, const std::string &module) {
    if (generated_name(name)) return "generated code";
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
        uint64_t base = 0; // the function's address (for its hot offsets' source lines)
    };
    std::map<std::string, Fn> fns;
    std::map<std::string, uint64_t> cats;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (auto &[rip, w] : weights) {
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
        std::memset(buffer, 0, sizeof(buffer));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 displacement = 0;
        std::string name = "?", module = "?";
        IMAGEHLP_MODULE64 mod{sizeof(mod)};
        if (SymGetModuleInfo64(process, rip, &mod)) module = mod.ModuleName;
        if (SymFromAddr(process, rip, &displacement, sym)) name = sym->Name;
        else if (!generated_at(rip, name, displacement)) name = module + "+?";
        Fn &f = fns[name];
        if (f.cat.empty()) f.cat = category(name, module);
        if (!f.base) f.base = rip - displacement;
        f.weight += w;
        f.offsets[displacement] += w;
        cats[f.cat] += w;
    }
    std::printf("profile: where the CPU time went (%llu Mcycles sampled)\n", (unsigned long long)(total / 1000000));
    std::vector<std::pair<uint64_t, std::string>> by_cat;
    for (auto &[c, w] : cats) by_cat.push_back({w, c});
    std::sort(by_cat.rbegin(), by_cat.rend());
    for (auto &[w, c] : by_cat) std::printf("  %5.1f%%  %s\n", 100.0 * double(w) / double(total), c.c_str());
    std::vector<std::pair<uint64_t, std::string>> by_fn;
    for (auto &[n, f] : fns) by_fn.push_back({f.weight, n});
    std::sort(by_fn.rbegin(), by_fn.rend());
    std::printf("profile: top functions\n");
    for (size_t k = 0; k < by_fn.size() && k < top; k++) {
        const Fn &f = fns[by_fn[k].second];
        std::vector<std::pair<uint64_t, uint64_t>> offs;
        for (auto &[o, w] : f.offsets) offs.push_back({w, o});
        std::sort(offs.rbegin(), offs.rend());
        std::string hot;
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
        } else if (generated_at(address - 1, name, displacement)) {
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
    SymCleanup(process);
    std::vector<std::pair<uint64_t, std::string>> by_inclusive;
    for (auto &[n, w] : inclusive) by_inclusive.push_back({w, n});
    std::sort(by_inclusive.rbegin(), by_inclusive.rend());
    std::printf("profile: inclusive (the sampled thread's stacks: each function with all it calls)\n");
    for (size_t k = 0; k < by_inclusive.size() && k < 2 * top; k++)
        std::printf("  %5.2f%%  %s\n", 100.0 * double(by_inclusive[k].first) / double(total), by_inclusive[k].second.c_str());
}

} // namespace xf::sampler

#else

namespace xf::sampler {
// TODO: POSIX (a profiling timer's signal reading the interrupted context)
bool start(unsigned, unsigned long) { return false; }
void name_generated(const void *const *, const unsigned *, size_t) {}
double process_cpu_seconds() { return -1; } // TODO: getrusage
void stop_and_report(size_t) {}
} // namespace xf::sampler

#endif
