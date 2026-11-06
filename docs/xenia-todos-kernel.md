# Xenia's kernel TODOs, classified (2026-11-06)

The plan's "Accuracy beyond Xenia" item, the first, cheap and read-only step: every `TODO` comment in
`xenia-canary/src/xenia/kernel` (`*.cc`, `*.h`), what each says, and what XenoForge can do about it.

- **Xenia revision:** `838850fed8389bdb28dc38f89bcb73bb43db1494` (xenia-canary, 2026-11-04), the revision of
  `src/exports/`. Line numbers are that revision's; to update, grep `TODO` again and compare by file and text.
- **XenoForge base:** master `c7a147c`.
- **Count:** 118 comments in 53 files.
- Games: Chavo (El Chavo Kart), Ben10, BC2 (Battlefield: Bad Company 2), FH (Forza Horizon), GoW3, Halo (Reach),
  RDR (Red Dead Redemption), Sonic (Unleashed). "Imports" is `xfinspect --imports`; "calls" is the export's
  count in a 15 second boot of the master build (interpreted, no game library, so the games are at different
  points; a call count is a floor, not a rate). BC2's boot (`--xex Game.xex`) ends at once, so it has imports only:
  Game.xex is a DLL its default.xex loads after Splash.xex's "press START" (runtime.md, "BC2's launcher chain").
- Nothing of the console's XAM (code, data, strings) is in this page; the oracle's answers are behavior only.

## Counts

| Category | TODOs | What |
| --- | ---: | --- |
| A. Known behavior Xenia skips or approximates | 8 | doable now; effort in each row |
| B. Done, or not applicable, in XenoForge | 45 | where, or why |
| C. The console's XAM can answer | 15 | 8 answered by the oracle, 7 need state it lacks |
| D. Unknown, needs hardware | 17 | test list for the Trinity |
| E. Not about accuracy | 33 | cleanup, speed, logging, Xenia's own UI |
| Total | 118 | |

Two of the oracle's answers (C) also leave work in ours, so the work list has ten items: the eight of A and
those two (marked "work" in C).

## A. Known behavior Xenia skips or approximates (by impact)

| # | Xenia | What it says | What is known, and where it goes in ours | Effort | Games |
| --- | --- | --- | --- | --- | --- |
| A1 | xboxkrnl_debug.cc:77 | after SetThreadName's exception (RtlRaiseException) "is unwinding needed here?" | NT's exception dispatch: RtlRaiseException walks the frames, asks each frame's handler (`__C_specific_handler`, the C++ frame handler) in two passes, then unwinds to the one that takes it; an unhandled one ends the thread or the process. Ours returns after logging (misc.cpp, RtlRaiseException): a throw or an `__except` never reaches its handler. The unwind data is the image's exception table; `RtlUnwind` and `__C_specific_handler` are imported by all eight. The runtime already unwinds host frames for catch continuations (docs/runtime.md, "The Unwind across the boundary"), so the missing part is the search and the handlers' protocol. Sources: Microsoft's SEH documents, Wine's `ntdll` exception tests (run on Windows), ReactOS `rtl`. | L | all 8 import RtlRaiseException and RtlUnwind; RtlRaiseException calls: Halo 12, Sonic 11, GoW3 2, Chavo 1, Ben10 1 (mostly SetThreadName and C++ throws) |
| A2 | xboxkrnl_debug.cc:142 | a breakpoint raised by the guest has no unwinding ("this is going to suck") | NT raises STATUS_BREAKPOINT through the same dispatch (A1); with no handler the title dies, it does not continue. Ours (misc.cpp, DbgBreakPoint) logs and returns, as Xenia does. Follows A1. | S after A1 | all but BC2 import DbgBreakPoint; no calls seen |
| A3 | xboxkrnl_io.cc:187, :267, :327 | NtReadFile, NtReadFileScatter, NtWriteFile have no asynchronous path | NT's contract for a handle opened without a synchronous flag: the call returns STATUS_PENDING with the status block still pending, and the event, APC and completion port fire when the request finishes. Xenia and ours finish at once and write the result before returning (files.cpp, `finish_request`; the PENDING answer is only Xenia's `out_pending` for unbuffered handles). A later completion on the scheduler's clock (a request queue and a timer with the disc's latency) would be NT's; the order seen by a polling or APC-driven loader changes. Three TODOs, one mechanism. | M | all 8 import all three; calls: NtReadFile Chavo 3256, GoW3 1592, RDR 689, Halo 274, FH 262; NtWriteFile FH 44, Halo 47 |
| A4 | xboxkrnl_misc.cc:84 | KeEnableFpuExceptions does little: the FP state is not saved in the thread, no IRQL change, no machine-state change | NT sets the thread's FP-exception mode (MSR bits), so an enabled FPSCR exception raises a program interrupt that becomes a structured exception. Ours only stores a flag in the thread (threads.cpp, KeEnableFpuExceptions); nothing reads it. Needs the FPSCR's exception bits exact (plan: the CPU's exactness item) and A1 to deliver the exception. | L (after A1 and the FPSCR work) | Chavo 16, Ben10 320, Sonic 16 calls; Halo imports |
| A5 | xboxkrnl_threading.cc:476 | should a null interval to KeDelayExecutionThread count as zero? | NT reads the interval through the pointer (the user-mode form probes it): a null pointer is an access violation, not a zero wait. The oracle confirmed it: the console's kernel faults reading address 0. Done: ours fails the call with STATUS_ACCESS_VIOLATION and logs it once (no exception delivery yet, A1; a game that did it would crash on the console). | S | all 8 import it; calls Chavo 3.7 M, GoW3 13 M, Halo 631 k (with real intervals) |
| A6 | xfile.h:76 | validate path components one by one | FATX (the hard drive, memory units): a name is at most 42 characters, a path 240, and the names seen on disks use only space, `$`, `.`, digits, letters and `_` (Free60's FATX page, "Limitations" and the character list). Ours checks Xenia's whole-path character test (files.cpp:71). The kernel's own rejection of other characters is not proven by that page (it lists what is found), so the character rule would stay a warning; the length limits are firm. | S | all 8 create files (NtCreateFile: FH 732, Chavo 33, Halo 81) |

The other two A-sized items are in C (marked "work").

## C. What the console's XAM answers

`xfxam <xam.xex> call <Export> ...`, one run each, the master build, the 2.0.16202 XAM of El Chavo Kart's
system update. "Needs state": the oracle's start has no title launched through it, no user, no content device
and no network adapter (docs/runtime.md, "XAM's oracle"), so the export stops before the question.

### Answered by the oracle

| Xenia | What it says | Answer | Ours | Games |
| --- | --- | --- | --- | --- |
| xam_task.cc:30 (work) | the handle XamTaskSchedule writes: "figure out what this is for" | The task is not a thread of its own. The call enters a critical region, makes a task object, takes a work-queue lock, resets an event, releases a semaphore that wakes one of XAM's worker threads (the eight its start makes), and returns 0. The handle written is a pointer into XAM's heap with its low bit set (not a fixed number). | xam.cpp:527 starts a new system thread per task and hands out its handle. Work (M): a worker pool and a queue; the handle an object that XamTaskCloseHandle, XamTaskShouldExit (also imported) can find. Visible: thread count and ids, and tasks serialized when workers are all busy. | Chavo, BC2, FH, GoW3, Halo, RDR import XamTaskSchedule; FH and Halo call it once |
| xam_nui.cc:139 (work) | Kinect tilt status not reported | With no sensor the call first asks the kernel for the Kinect's USB hardware version (UsbdGetNatalHardwareVersion) and answers 0x8007048F (device not connected), even for a zeroed argument. XamNuiGetDeviceStatus answers 0 and writes a status block. | XamNuiCameraTiltGetStatus answers E_FAIL (0x80004005), XamNuiGetDeviceStatus 0xC0050006. Work (S): the console's two answers. | FH only (imports XamNuiCameraTiltGetStatus, ElevationGetAngle, GetDeviceStatus) |
| xam_info.cc:789 | Win32 `WaitForSingleObjectEx` shim is "janky" | Matches ours. It calls NtWaitForSingleObjectEx with wait mode 1 (user), the alertable flag, a null timeout for INFINITE and a pointer to a relative timeout otherwise, then maps a failure with RtlNtStatusToDosError and returns -1. | same (xam_win32.cpp) | not imported by the games directly |
| xam_video.cc:27 | are XGetVideoMode and VdQueryVideoMode the same? | The same: XGetVideoMode calls VdQueryVideoMode with the caller's buffer and returns. Matches. | same | all 8 call XGetVideoMode (RDR 406 calls) |
| xam_ui.cc:137, :186 | do something with extended_error and length in the path without an overlapped | The dialogs are never synchronous: with no overlapped, XamShowMessageBoxUI(Ex) answers ERROR_INVALID_PARAMETER (docs/runtime.md, second survey). The path these two TODOs sit in does not exist on the console. | done (xam_ui.cpp checks it) | BC2, FH, Sonic (MessageBoxUI); Chavo, Ben10, FH, GoW3, Halo, RDR, Sonic (Ex) |
| xam_user.cc:1133 | XamSessionRefObjByHandle "properly" | A handle that is not the session's is ERROR_INVALID_HANDLE (it references the object through the kernel and maps the status). | done (xam.cpp:584), cleared output | Chavo, BC2, FH, GoW3, Halo, RDR import |
| xboxkrnl_hid.cc:18 | HidReadKeys errors games check (0xC000009D to 0x48F, 0x103 to 0x10D2) | XamInputGetKeystroke(0xFF, ...) on the console's XAM answers 0x48F with our HidReadKeys (0xC000009D) behind it: the mapping is the console's own. The 0x103 case cannot be made from here. | HidReadKeys answers 0xC000009D (misc.cpp:246); matches | none imports HidReadKeys |

### Needs state the oracle lacks

| Xenia | What it says | What the run showed | Games |
| --- | --- | --- | --- |
| xam_content.cc:194 | on a bad device, should XamContentCreateEnumerator zero the data? | The console makes the enumerator object first (0xB0 bytes, from the kernel's object manager) and only then looks at the device; with no title heap that allocation fails (0xC000009A mapped to 0x5AA) and it leaves the output buffer-size and handle untouched. The unknown-device path is not reached. Ours zeroes the size on a bad device (Xenia's). Test on hardware: D-list candidate. | Chavo, Ben10, BC2, FH, GoW3, Halo, RDR import; FH called it once |
| xam_content.cc:505 | XamContentOpenFile's arguments "assumed"; Xenia returns FILE_NOT_FOUND | needs a content device and a user (second survey: content calls with a name answer invalid argument whatever is given) | FH imports |
| content_manager.cc:583 | the real error for deleting content that is open | needs content state | Chavo, BC2, FH, GoW3, Halo, Sonic import XamContentDelete |
| xam_net.cc:522 | a proper MAC address in XNADDR | Without XNetStartup by a title and with no adapter, NetDll_XNetGetTitleXnAddr answers 0 (pending) and zero-fills the structure, taking only the network lock; ours answers "static" with the loopback and 0xCC x 6 for the Ethernet address at once (xam_net.cpp:580; RakNet needs it non-zero). Work (S, small): answer pending and zeroed before NetDll_XNetStartup. The real adapter's MAC needs the Nic state. | Halo calls it 136 times; all 8 import |
| xam_net.cc:608 | XNetDnsLookup "actually implement this" | Allocates the XNDNS from the kernel pool (a 'NETt'-tagged block), writes its pointer, answers 0; the status field inside needs the adapter and the resolver. Ours answers 0 with the field set to failed at once (offline). | Chavo, BC2, GoW3, RDR import |
| xam_user.cc:625 | does XamUserContentRestrictionCheckAccess's overlapped need the access argument | needs a signed-in user | none calls it |
| xcontent.h:24 | does the console have a value for "the running title" in a content structure (Xenia uses 0xFFFFFFFF) | needs content state | - |

## D. Unknown, needs hardware: tests for the Trinity

Each is one small guest program run once on the console; the first column is the TODO. Games are those that import the export
(calls in parentheses).

| Xenia | Measure | Games |
| --- | --- | --- |
| xboxkrnl_memory.cc:468 | MmAllocatePhysicalMemoryEx with min and max address ranges, one allocation per page size (4 KB, 64 KB, 16 MB): the address returned, MmGetPhysicalAddress of it, whether the range is honored exactly and whether the first physical page is 0 or 0x1000. Xenia adds 0x1000 and has a switch to drop it for ranged calls. | all 8 (Ben10 1513, RDR 2020, Sonic 1559 calls) |
| xboxkrnl_memory.cc:618 | MmQueryStatistics: every field before and after allocating known numbers of pages of each kind (pool, stack, image, heap, virtual, physical, page table, cache); Xenia's title section is mostly guessed. | FH, Halo (28 calls), RDR, Sonic |
| xboxkrnl_memory.cc:760 | KeGetImagePageTableEntry for pages of the image in 4 KB and 64 KB mappings, read-only and writable: which bits come back (bit 0, 0x40000000). | none imports it (dashboards) |
| xboxkrnl_io_info.cc:362 | NtQueryVolumeInformationFile (FileFsSizeInformation, FileFsDeviceInformation) for the disc, the hard drive's partition, the flash and a memory unit: bytes per sector (0x200 or 0x800), units, device type and characteristics. Ours: 0x200 everywhere (vfs.cpp:201); the hard drive's 0x200 agrees with Free60's FATX page. | FH 8, Halo 31 calls |
| xfile.cc:238 | NtReadFileScatter with segment buffers in 4 KB-page and in 64 KB-page memory (the physical heaps): how many bytes each segment element takes. Ours: 4096 always (files.cpp `segmented`). | all 8 import; none called in 15 s |
| xfile.cc:300 | NtWriteFile to a handle opened without a synchronous flag and without buffering, at an offset inside, at and past the end: the returned status (success or pending) and the status block. | FH 44, Halo 47 calls |
| xboxkrnl_rtl.cc:358, :383 | RtlMultiByteToUnicodeN and RtlUnicodeToMultiByteN for every byte 0x80-0xFF and for code points past 0xFF: the table the kernel uses (Latin-1 as Xenia, or a Windows code page), the "?" substitution, odd lengths. | Chavo 17 k and 31 k, GoW3 2 k and 40 k, Sonic 158 k and 26 k calls |
| xboxkrnl_strings.cc:236 | the kernel's formatted print (DbgPrint, the `_vsnprintf` family) on `%Ld`, `%Lf`, an unknown size or type character: what is printed, what is skipped. | not known which |
| xboxkrnl_video.cc:428 | VdCallGraphicsNotificationRoutines: which routines run, with what (r3, r4), in what thread and IRQL; and the export that registers them. | all 8 call it once at start |
| xboxkrnl_crypt.cc:67, :74 | XeCryptSha256, 384, 512 and Sha state structures: fill a buffer with a pattern, Init, Update with 1 and 100 bytes, Final; record the written extent. Ours follows Xenia (0x64 for SHA-256). | FH (129 SHA-256 updates), Halo |
| kernel_state.cc:903 | ExRegisterTitleTerminateNotification: when the routines run (HalReturnToFirmware, XamLoaderTerminateTitle, a title launch), their order by priority, r3 and r4, the thread and the IRQL, whether the terminator waits. Ours registers and never runs them (modules.cpp:1020), as Xenia. | all 8 register 3-4 routines |
| kernel_module.cc:139 | XexGetProcedureAddress by name on the handles of xboxkrnl.exe and xam.xex (and on a DLL loaded with XexLoadImage): the status and the address. Ours fails by name for kernel modules, as Xenia. | all 8 import (Halo 453 calls, mostly by ordinal) |
| kernel_state.cc:399 | XexGetModuleHandle(NULL) called from a DLL loaded with XexLoadImage: the title's handle or the DLL's. Ours: the executable. | Halo 187 calls, FH 15; Chavo, BC2, GoW3, RDR also load DLLs |
| signin_ui.cc:223 | XamShowSigninUI needing 2 or more users: which slots are chosen by default. | all 8 import |
| xam_ui.cc:874 | XamShowMarketplaceDownloadItemsUI: the HRESULT written for cancel, for done and with no Live. Xenia writes success. | BC2, FH, Halo import |

## B. Done, or not applicable, in XenoForge

Grouped by reason. Where one of these shows a real difference from Xenia, the code's header says so.

**Done in our code (as the TODO wants, or better than Xenia):**

| Xenia | Where in ours |
| --- | --- |
| xmutant.cc:51 (abandoning a mutant) | sync.cpp, KeReleaseMutant: NT's abandon (signaled, owner cleared); a thread that exits abandons its mutants (threads.cpp, exit) |
| xthread.cc:977 (an absolute timeout) | objects.cpp, `deadline_at`: a positive timeout is a system time on the scheduler's clock |
| xthread.cc:478, :484 (exit order, waking waiters) | threads.cpp (exit): DLL thread-end notifications, then terminated and signaled, waiters woken, off its process, in NT's order |
| xthread.cc:446 (translate a creation error) | our threads are scheduler objects, not host threads; `create_thread` answers NT statuses |
| kernel_state.cc:1144 (overlapped length read as success) | xam.cpp, `complete_overlapped_immediate`: -1 when it failed |
| xobject.h:148 (guest types not all mapped) | objects.cpp, `Objects::native`: events, semaphores, mutants, timers, threads; no game imports the queue functions |
| xsymboliclink.cc:29 (links not registered) | vfs.cpp: the kernel's symbolic-link table, longest-prefix match |
| xam_content.cc:809 (XamSwapDisc: is it the requested disc) | xam.cpp: a disc other than the one in ends the run |
| xcontent.h:321 (updates' header size) | package.cpp reads the header size field (0x340, as Free60's STFS page); the system update's `xam.xex` was extracted with it |
| xboxkrnl_module.cc:186 (the command line's module name) | modules.cpp:777: `"<name>.xex"` of the executable loaded |
| xboxkrnl_hal.cc:33 (a graceful stop) | misc.cpp: HalReturnToFirmware and KeBugCheckEx end the process cleanly with the reason |
| user_module.h:120 (attach on the main thread) | modules.cpp: DLL_PROCESS_ATTACH runs on the loading thread, before XexLoadImage returns, as NT's loader does |
| gamercard_ui.cc:677 (notify only the changed player) | xam_user.cpp:388, :638: XN_SYS_PROFILESETTINGCHANGED with the user's bit |
| xam_content_device.cc:25 (real device information) | xam_content.cpp:446: the hard drive's size is the content root's volume; the disc's is Xenia's dummy |
| xam_net.cc:245, :784 (XNet shutdown; delete the socket) | xam_net.cpp: startup counts, cleanup per Winsock; sockets are kernel objects released with their handle |
| xam_net.cc:417 (WSASendTo's overlapped) | xam_net.cpp: completes at once (runtime.md, "XAM") |
| xam_net.cc:857, :1056 (ioctlsocket, select's count) | xam_net.cpp:886 (FIONBIO, FIONREAD), :987 (select answers the count of ready sockets) |
| xsocket.cc:344 (limit incoming packets) | xam_net.cpp:345: the queue is limited by SO_RCVBUF |
| xbdm_misc.cc:197 (DmGetXbeInfo success) | xbdm.cpp:220 |
| video.cc:220 (display information) | video.cpp:96: from the settings (screen size, aspect, region), no host display is asked |

**Not applicable (no host sockets, no UI, no save states, no Kinect, no network, no per-title patches):**

- xsocket.cc:42, :106, :148, :165: host socket options and Winsock error codes (ours has no host socket; errors are Winsock's, set in xam_net.cpp). :263, :312: Xenia's unfinished XNet path.
- game_info_database.cc:277 and .h:162 (matchmaking attributes from the SPA), presence_string_builder.cc:107 (presence strings): the network is offline and nothing shows presence.
- xam_net.cc:690 (XampXAuthStartup details): no game imports it; XAuth is offline.
- xam_user.cc:563 (network storage status 2): the network storage is offline.
- xam_nui.cc:151, :168, :190, nui.cc:19, xam_app.cc:205, xam_ui.cc:442: Kinect is never set up (its angle, the controller type, identity callbacks, selecting buttons).
- user_module.cc:392, xmodule.cc:101: Xenia's save-state restore; XenoForge has replays (plan A5), not save states.
- kernel_state.cc:668 (custom patches on load): a port's hooks, in the SDK (plan E), not the loader.
- user_profile.h:77 (tile file names in a profile): gamer tiles are never found here.
- xboxkrnl_crypt.cc:134 (the keys' array): console-unique keys cannot be had; XeKeysConsolePrivateKeySign (Chavo, BC2, FH, GoW3, Halo, RDR import it; FH 3, Halo 9 calls) answers as Xenia's stub.

## E. Not about accuracy

33 TODOs: cleanup, speed, logging, Xenia's own ImGui screens, assertions. Locations:
kernel_module.cc:31; kernel_state.cc:549, :1491; user_module.cc:82, :713, :720, :848, :1191; shim_utils.h:581;
xmp_app.h:332; content_manager.cc:612; content_manager.h:143; gamercard_ui.cc:438; game_achievements_ui.cc:159;
xam_locale.cc:24; xam_net.cc:301; xam_ui.cc:42; xam_user.cc:1000; xcontent_package_container.cc:54;
xboxkrnl_debug.cc:42, :57; xboxkrnl_error.cc:27; xboxkrnl_io.cc:269; xboxkrnl_io_info.cc:34, :75, :161, :316;
xfile.cc:35; xfile.h:204; xobject.cc:44, :476; xthread.cc:98, :522.

## What changes in the plan

- "Accuracy beyond Xenia", the first step (Xenia's TODOs): the kernel's 118 are classified here; the 8 A items
  and the 2 oracle-found ones are the kernel's work list (structured exception dispatch first).
- Structured exception dispatch (RtlRaiseException, RtlUnwind, `__C_specific_handler`, STATUS_BREAKPOINT) belongs
  with the FPSCR exactness work, which needs it for KeEnableFpuExceptions.
- XamTaskSchedule as a worker pool, and the Kinect tilt and device-status answers, are small oracle-found items.
- The Trinity's list is 15 small programs (covering 17 TODOs) (section D); the highest value are MmAllocatePhysicalMemoryEx's ranges,
  MmQueryStatistics, the volume sector sizes and the termination notifications.
