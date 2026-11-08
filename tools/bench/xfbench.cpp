// xfbench: the standing benchmark against Xenia (docs/renderer.md, "The
// standing benchmark"). It runs a program for a number of seconds and times
// it from outside, the same way whatever the program and its graphics API:
//
// - frames: the time of each present, recorded by xfbench_hook.dll, as a
//   Vulkan layer (--vk) or injected into a Direct3D 12 program before its
//   main thread runs (--dxgi);
// - GPU time: the process's running time on each GPU engine, as Windows'
//   scheduler counts it ("GPU Engine" performance counters, what the Task
//   Manager shows; no administrator needed, unlike ETW and PresentMon);
// - CPU time: the process's user and kernel time.
//
// Everything is summed over the window [--warmup, --seconds] and printed as
// one line of key=value pairs: presents a second, the median frame time and
// its 5th and 95th percentiles, GPU ms a frame and a second by engine type,
// CPU seconds a second.
//
// xfbench [--seconds N] [--warmup S] [--step S] [--timeline] [--vk | --dxgi] [--stdout <file>] [--label L]
//         -- <program> [args...]
//
// --step: how often the GPU and CPU times are sampled (5 s); --timeline
// prints each step's presents a second and GPU time, to find a run's phases.
//
// Windows only, as is Xenia.

#include <windows.h>

#include <pdh.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

std::wstring widen(const std::string &s) {
    std::wstring w(s.size() + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}

std::wstring quote(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') q += L'\\';
        q += c;
    }
    return q + L"\"";
}

double seconds_of(const FILETIME &f) {
    return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
}

double cpu_seconds(HANDLE process) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(process, &c, &e, &k, &u)) return 0;
    return seconds_of(k) + seconds_of(u);
}

// the process's GPU running time by engine type ("3d", "compute", "copy"...),
// in seconds; the counter's raw value is in 100 ns units
std::map<std::string, double> gpu_seconds(DWORD pid) {
    std::map<std::string, double> by_type;
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return by_type;
    wchar_t path[256];
    swprintf(path, 256, L"\\GPU Engine(pid_%lu_*)\\Running Time", pid);
    PDH_HCOUNTER counter = nullptr;
    if (PdhAddEnglishCounterW(query, path, 0, &counter) == ERROR_SUCCESS && PdhCollectQueryData(query) == ERROR_SUCCESS) {
        DWORD size = 0, count = 0;
        PdhGetRawCounterArrayW(counter, &size, &count, nullptr);
        std::vector<uint8_t> buffer(size);
        auto *items = (PDH_RAW_COUNTER_ITEM_W *)buffer.data();
        if (size && PdhGetRawCounterArrayW(counter, &size, &count, items) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < count; i++) {
                std::wstring name = items[i].szName;
                size_t at = name.find(L"engtype_");
                std::string type = at == std::wstring::npos ? "other" : std::string(name.begin() + at + 8, name.end());
                // "Compute_0", "Compute_1": one type
                if (size_t u = type.find('_'); u != std::string::npos) type.resize(u);
                for (char &c : type) c = char(tolower(c));
                by_type[type] += double(items[i].RawValue.FirstValue) * 1e-7;
            }
        }
    }
    PdhCloseQuery(query);
    return by_type;
}

bool inject(HANDLE process, const std::wstring &dll, std::string &error) {
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) return error = "write", false;
    auto load = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
    if (!t) return error = "CreateRemoteThread(LoadLibraryW)", false;
    WaitForSingleObject(t, 30000);
    CloseHandle(t);
    // the hook's base in the target, then its entry at the same offset
    HMODULE modules[1024];
    DWORD needed = 0;
    uintptr_t base = 0;
    if (EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
            wchar_t name[MAX_PATH];
            if (GetModuleFileNameExW(process, modules[i], name, MAX_PATH) &&
                std::filesystem::path(name).filename() == std::filesystem::path(dll).filename())
                base = uintptr_t(modules[i]);
        }
    }
    if (!base) return error = "the hook did not load in the target", false;
    HMODULE local = LoadLibraryW(dll.c_str());
    auto entry = uintptr_t(GetProcAddress(local, "xfbench_hook_dxgi"));
    if (!local || !entry) return error = "the hook has no xfbench_hook_dxgi", false;
    t = CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)(base + (entry - uintptr_t(local))), nullptr,
                           0, nullptr);
    if (!t) return error = "CreateRemoteThread(xfbench_hook_dxgi)", false;
    WaitForSingleObject(t, 30000);
    DWORD code = 99;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    if (code) return error = "xfbench_hook_dxgi failed: " + std::to_string(code), false;
    return true;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))];
}

} // namespace

int main(int argc, char **argv) {
    double seconds = 60, warmup = 20, step = 5;
    bool vk = false, dxgi = false, timeline = false;
    std::string stdout_file, label = "run";
    int first = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)
            warmup = std::atof(argv[++i]);
        else if (a == "--step" && i + 1 < argc)
            step = std::max(0.5, std::atof(argv[++i]));
        else if (a == "--timeline")
            timeline = true;
        else if (a == "--vk")
            vk = true;
        else if (a == "--dxgi")
            dxgi = true;
        else if (a == "--stdout" && i + 1 < argc)
            stdout_file = argv[++i];
        else if (a == "--label" && i + 1 < argc)
            label = argv[++i];
        else if (a == "--") {
            first = i + 1;
            break;
        }
    }
    if (!first || first >= argc || warmup >= seconds) {
        std::fprintf(stderr, "usage: xfbench [--seconds N] [--warmup S] [--step S] [--timeline] [--vk | --dxgi] [--stdout <file>] "
                             "[--label L] -- <program> [args...]\n");
        return 2;
    }

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(self).parent_path();
    const std::filesystem::path hook = dir / "xfbench_hook.dll";
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path log =
        std::filesystem::path(temp) / ("xfbench_" + std::to_string(GetCurrentProcessId()) + ".presents");
    std::filesystem::remove(log);
    SetEnvironmentVariableW(L"XF_BENCH_LOG", log.wstring().c_str());
    if (vk) {
        // the layer's manifest (xfbench_hook.json) is beside the DLL
        SetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", dir.wstring().c_str());
        SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", L"VK_LAYER_XF_bench");
        SetEnvironmentVariableW(L"VK_LOADER_LAYERS_ENABLE", L"VK_LAYER_XF_bench");
    }

    std::wstring command;
    for (int i = first; i < argc; i++) command += (command.empty() ? L"" : L" ") + quote(widen(argv[i]));
    const std::filesystem::path program = std::filesystem::absolute(widen(argv[first]));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE out = INVALID_HANDLE_VALUE;
    if (!stdout_file.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        out = CreateFileW(widen(stdout_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = out;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    // the program and anything it starts end with xfbench
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, out != INVALID_HANDLE_VALUE, CREATE_SUSPENDED,
                        nullptr, program.parent_path().wstring().c_str(), &si, &pi)) {
        std::fprintf(stderr, "xfbench: cannot start the program (%lu)\n", GetLastError());
        return 1;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    if (dxgi) {
        std::string error;
        if (!inject(pi.hProcess, hook.wstring(), error)) {
            std::fprintf(stderr, "xfbench: %s\n", error.c_str());
            TerminateProcess(pi.hProcess, 1);
            return 1;
        }
    }
    LARGE_INTEGER qpf, started;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&started);
    ResumeThread(pi.hThread);

    // sampled every --step seconds, the window's start and end among them
    struct Sample {
        int64_t t;
        std::map<std::string, double> gpu;
        double cpu;
    };
    std::vector<Sample> samples;
    auto sample = [&] {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        samples.push_back({t.QuadPart, gpu_seconds(pi.dwProcessId), cpu_seconds(pi.hProcess)});
    };
    bool exited = false;
    size_t window_first = 0;
    sample();
    for (double at = step; at <= seconds + 1e-6; at += step) {
        const int64_t due = started.QuadPart + int64_t(at * qpf.QuadPart);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const DWORD wait = due > now.QuadPart ? DWORD((due - now.QuadPart) * 1000 / qpf.QuadPart) : 0;
        if (WaitForSingleObject(pi.hProcess, wait) == WAIT_OBJECT_0) {
            exited = true;
            break;
        }
        sample();
        if (at <= warmup + 1e-6) window_first = samples.size() - 1;
    }
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(job);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (exited) {
        std::fprintf(stderr, "xfbench: the program ended before the window did\n");
        return 1;
    }
    const Sample &first_sample = samples[window_first], &last_sample = samples.back();
    LARGE_INTEGER window_start{}, window_end{};
    window_start.QuadPart = first_sample.t;
    window_end.QuadPart = last_sample.t;
    const std::map<std::string, double> &gpu_start = first_sample.gpu, &gpu_end = last_sample.gpu;
    const double cpu_start = first_sample.cpu, cpu_end = last_sample.cpu;
    auto gpu_busy = [](const Sample &s) {
        double total = 0;
        for (auto &[type, v] : s.gpu)
            if (type != "copy") total += v;
        return total;
    };

    // the presents in the window
    std::vector<int64_t> presents, all;
    if (FILE *f = _wfopen(log.wstring().c_str(), L"rb")) {
        int64_t t;
        while (std::fread(&t, sizeof(t), 1, f) == 1) {
            all.push_back(t);
            if (t >= window_start.QuadPart && t <= window_end.QuadPart) presents.push_back(t);
        }
        std::fclose(f);
    }
    // each step's presents a second and GPU time a frame, to see the run's phases
    if (timeline) {
        for (size_t i = 1; i < samples.size(); i++) {
            const double dt = double(samples[i].t - samples[i - 1].t) / qpf.QuadPart;
            const auto n = std::count_if(all.begin(), all.end(),
                                         [&](int64_t t) { return t > samples[i - 1].t && t <= samples[i].t; });
            const double busy = (gpu_busy(samples[i]) - gpu_busy(samples[i - 1])) * 1000.0;
            std::printf("t=%.0f fps=%.1f gpu_ms_per_frame=%.2f gpu_ms_per_s=%.1f cpu_s_per_s=%.2f\n",
                        double(samples[i].t - started.QuadPart) / qpf.QuadPart, n / dt, n ? busy / n : 0.0, busy / dt,
                        (samples[i].cpu - samples[i - 1].cpu) / dt);
        }
    }
    std::filesystem::remove(log);
    const double window = double(window_end.QuadPart - window_start.QuadPart) / qpf.QuadPart;
    std::vector<double> intervals;
    for (size_t i = 1; i < presents.size(); i++)
        intervals.push_back(double(presents[i] - presents[i - 1]) * 1000.0 / qpf.QuadPart);
    const double frames = double(presents.size());
    std::printf("label=%s window=%.1f presents=%.0f fps=%.2f frame_ms_median=%.2f p5=%.2f p95=%.2f p99=%.2f", label.c_str(),
                window, frames, frames / window, percentile(intervals, 0.5), percentile(intervals, 0.05),
                percentile(intervals, 0.95), percentile(intervals, 0.99));
    double gpu_total = 0;
    for (auto &[type, end] : gpu_end) {
        const auto start = gpu_start.find(type);
        const double busy = end - (start == gpu_start.end() ? 0.0 : start->second);
        if (busy <= 0) continue;
        if (type != "copy") gpu_total += busy;
        std::printf(" gpu_%s_ms_per_s=%.2f", type.c_str(), busy * 1000.0 / window);
    }
    std::printf(" gpu_ms_per_frame=%.3f gpu_ms_per_s=%.2f cpu_s_per_s=%.3f\n", frames ? gpu_total * 1000.0 / frames : 0,
                gpu_total * 1000.0 / window, (cpu_end - cpu_start) / window);
    return 0;
}
