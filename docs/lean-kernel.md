# What the games ask of the system (2026-11-04)

The kernel XenoForge implements is the one its games ask for, measured. This
page holds the measurements and what follows from them. It is a list of
names of the system's own functions; nothing of any game is in it.

## How it was measured

`xfinspect <game.iso> --imports` reads the disc in place, decrypts and
unpacks `default.xex`, and lists each import record of the image by library
and ordinal, named from Xenia's export tables. `docs/imports.tsv` is the
union, with a column per game.

These are **imports**: what the executable can call. What it does call, and
how often, comes when XenoForge runs the games (step A4 of
[the plan](plan.md)): the kernel counts every call to an export and every
export asked for that it lacks, as the 3DS runtime's `RUNTIME_USAGE` does.
An import nothing calls stays a stub that logs once.

## The two games

| | Game A | Game B |
| --- | --- | --- |
| Engine | Unreal Engine 3 | its own |
| XDK | 2.0.21250 | 2.0.8276 |
| `default.xex` | 30 MB, basic compression, encrypted | 8.7 MB, basic compression, encrypted |
| Image | 0x82000000, 0x1DB0000 bytes | 0x82000000, 0x8E0000 bytes |
| xboxkrnl imports | 176 functions, 13 variables | 127 functions, 13 variables |
| xam imports | 118 functions | 49 functions |

The LatAm and USA discs of game A import the same (and their
executables are the same file). Both games use basic compression; normal
(LZX) compression came with game C.

XDK libraries linked into each (the static libraries header):

- Game A: D3D9LTCG, D3DX9, XGRAPHC, XAUDIO2, XAPOBA, XAPOFX,
  X3DAUDL, XWMAD, XMCORE, XMP, XMIC, XHV2, XNET, XONLINE, XPARTY, XAPILIB
  (QFE 6), XBOXKRNL, LIBCMT; built with LINK 10 / C1, C2 16.
- Game B: D3D9, D3DX9, XGRAPHC, XAUDIO2, XAUD, XAPOBA, XMCORE, XMP, XMIC,
  XONLINE, XAPILIB, XBOXKRNL, LIBCPMT.

D3D9 is a static library in both, at different XDK versions: the case for
identifying its functions by signature per XDK version (plan, step N1).

## The union: 309 imports

| Library | Imports | Of which |
| --- | --- | --- |
| xboxkrnl.exe | 189 | 176 functions, 13 variables |
| xam.xex | 120 | 42 `NetDll_*` (sockets), 66 `Xam*`, the rest `XMsg*`, `XNotify*`, `XGet*` |

187 imports are shared; 120 are game A's alone, 2 game B's alone
(`XamInputGetKeystrokeEx`, `XGetLanguage`).

xboxkrnl by family:

| Family | Count | What it is |
| --- | --- | --- |
| `Nt*` | 34 | files, events, semaphores, mutants, timers, waits, virtual memory |
| `Ke*`, `Kf*`, `Ki*` | 35 | threads, priorities, affinity, DPCs, spin locks, critical regions, TLS |
| `Vd*` | 25 | the video driver: display mode, ring buffer, swap, interrupts |
| `Rtl*` | 22 | critical sections, strings, unwinding, time |
| `Ex*` | 11 | thread creation, pools, settings |
| `XAudio*` | 11 | the audio driver: render, register, ducker |
| `Io*` | 9 | device objects (game A only) |
| `Xex*` | 8 | modules: handles, sections, loading images |
| `Mm*` | 7 | physical memory, address queries |
| `Ob*` | 6 | object references and names |
| other | 21 | `XMA*` contexts, `Xe*` crypto and keys, `Stfs*`, `Dbg*`, `Hal*`, `Fsc*`, `Interlocked*SList`, the microphone's and the remote's device requests, the `Xbox*` variables, `__C_specific_handler`, CRT printf |

The 13 variables (data the kernel exports, mapped into the image's import
slots): `ExEventObjectType`, `ExThreadObjectType`, `ExLoadedCommandLine`,
`KeDebugMonitorData`, `KeCertMonitorData`, `KeTimeStampBundle`,
`XboxHardwareInfo`, `XboxKrnlVersion`, `XexExecutableModuleHandle`,
`VdGlobalDevice`, `VdGlobalXamDevice`, `VdGpuClockInMHz`,
`VdHSIOCalibrationLock`.

## What follows

- The kernel's core is threads, synchronization, memory and files: the
  `Nt`, `Ke`, `Rtl`, `Ex`, `Mm` and `Ob` families, 115 imports, 94 of
  them shared by both games.
- Video (`Vd*`) and audio (`XAudio*`, `XMA*`) are the driver side of the GPU
  and the APU: they belong with steps B and A6, not with the kernel's core.
- Of xam, the network (`NetDll_*`, 42) answers offline; the `XamShow*` UI
  (14) answers "cancelled"; users, content and input are real.
- Game A's own extras (device objects, `XexLoadImage`, voice, STFS,
  the guide's UI) are imported; whether anything calls them is the first
  question for the counts of step A4.

## What the eight discs import that nothing implemented (2026-11-09)

`xfinspect <disc> --imports` over the eight test discs, set against the exports
the kernel implements: 37 functions of xboxkrnl and xam were imported by a
disc and answered by nothing (Xenia has none of them either), and a run of each
disc with `boot --render` found ten more asked for through
`XexGetProcedureAddress` (the XInputdFF* family,
`XamInputGetUserVibrationLevel`, `XamVoiceSetMicArrayIdleUsers`). Each answers
now as the console does with the device absent; the answers and the tests are
in docs/runtime.md, "Exports nothing implemented".

| Export | Discs |
| --- | --- |
| `NetDll_XNetConnect`, `_XNetQosLookup`, `_XNetServerToInAddr` | A C D E F G |
| `NetDll_XNetUnregisterInAddr` | A D E F G |
| `NetDll_XNetGetConnectStatus` | C D F G |
| `NetDll_XNetQosGetListenStats` | A E F |
| `NetDll_getpeername` | C D |
| `RmcDeviceRequest` | A B |
| `XCustomGetCurrentGamercard`, `XCustomGetLastActionPressEx`, `XCustomSetDynamicActions`, `XCustomUnregisterDynamicActions` | D F |
| `EtxProducerRegister`, `EtxProducerLog`, `EtxProducerUnregister`, `PsCamDeviceRequest`, `LDICreateDecompression`, `LDIDecompress`, `LDIDestroyDecompression` | D |
| `XamCacheOpenFile`, `XamCacheCloseFile`, `XamCacheReset`, `XamInputRawState`, `XamNuiCamera*` (4), `XamReadBiometricData`, `XamWriteBiometricData`, `XamUserGetAgeGroup`, `XamUserNuiGetEnrollmentIndex`, `XamXlfs*` (4) | D |
| `XamBackgroundDownloadSetMode`, `XamContentInstall` | C, D |
