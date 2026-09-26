# HLE coverage

What the framework's system libraries answer, and how far each answer has been checked. The list follows the imports of five of the maintainer's discs, run on Windows with the desktop app: God of War: Chains of Olympus (`UCES-00842`), Grand Theft Auto: Chinatown Wars (`ULUS-10490`), Grand Theft Auto: Vice City Stories (`ULES-00502`), Patapon (`UCES-00995`) and Monster Hunter Portable 2nd G (`ULJM-05500`), plus Phantasy Star Portable 2 Infinity (`NPJH-50332`).

Three kinds of answer:

- **Verified on a game**: traced against a game calling it, with what the game does afterwards; the game named is the one it was checked with.
- **UNVERIFIED**: written from documentation (the SDK's headers and public descriptions), not yet seen called by any game. Each prints one line with `UNVERIFIED` the first time a game calls it, so the first game that does shows up in its log.
- **Not connected, by design**: infrastructure networking. It answers as a PSP with networking but no access point: libraries initialise, nothing connects.

NIDs are added to `configs/nids.csv` only where the first word of SHA-1 of the name matches, except the three randomized ones named after public NID lists, which say so below.

## Unimplemented imports

`<prefix>_LIST_STUBS=1` prints them; the counts are of the main executable's imports.

| Game | Start | Now | What is left |
| --- | ---: | ---: | --- |
| God of War | 18 | 2 | `sceKernelLoadExec`, `sceAtracSetAA3DataAndGetID` |
| Chinatown Wars | 61 | 14 | `sceNetAdhocMatching*` (9), `0x91DE343C` (SysMem), `0x71EC4271` (ThreadMan), `0x05572A5F` (IoFileMgr), `0x20628E6F`, `0x46EBB729` |
| Vice City Stories | 28 | 11 | `sceNetAdhocMatching*` (9), `sceKernelReferThreadProfiler`, `sceKernelReferGlobalProfiler` |
| MHP2G | 55 | 0 | |
| Patapon | 37 | 0 | |

## Verified on a game

| Call | Game | What it answers |
| --- | --- | --- |
| `sceKernelIsCpuIntrEnable` | God of War | 1 while interrupts are enabled |
| `sceIoChdir`, opening disc files with write flags | God of War | paths without a device are relative to it; the open succeeds, writes fail |
| `sceIoDread` names | God of War | as the disc spells them (the game hashes the paths it lists) |
| `sceKernelReferThreadStatus` | God of War | `SceKernelThreadInfo` as `pspthreadman.h` lays it out |
| `sceKernelReferEventFlagStatus` | God of War, Patapon | `SceKernelEventFlagInfo` |
| `sceDisplayGetFramePerSec` | God of War, Patapon | 59.94 in `$f0` |
| `sceMpegAvcDecodeFlush` | God of War | drops the decoder's state |
| `sceAtracGetSecondBufferInfo` | God of War | "second buffer not needed" (0x80630022) |
| busy clock (`Kernel::charge_busy_time`) | God of War | a thread that never waits sees time pass (off: `<prefix>_NO_BUSY_CLOCK`) |
| `sceKernel*MsgPipe` | Patapon (Create, Send, TryReceive) | a byte stream; Cancel and ReferStatus UNVERIFIED |
| `scePsmf*` | Patapon (SetPsmf, GetPsmfVersion, GetNumberOf(Specific)Streams, SpecifyStream, GetCurrentStreamType, GetVideoInfo) | the PSMF header; the other calls UNVERIFIED |
| `sceMpegAvcCsc` | Patapon | a decoded picture into pixels, range in pixels |
| GE SIGNAL list flow | Patapon | 0x10 jump, 0x11 call, 0x12 return |
| ATRAC streaming through `sceAtracSetData`, looping refill | Patapon | a buffer smaller than the file streams; a loop is asked for again |
| `scePower*ClockFrequency*`, `scePower_EBD177D6` | God of War, Patapon, Chinatown Wars (Set), PSP2i (GetCpu) | the clocks set are reported back; `0xEBD177D6` is named after public NID lists, and the arguments games pass (333, 333, 166) fit |
| `sceKernelSetCompiledSdkVersion370` (`0x342061E5`) | God of War, Patapon, MHP2G | named after public NID lists; called with 0x03070010 |
| `sceKernelDcacheWritebackInvalidateRange`, `sceKernelReleaseWaitThread`, `sceRtcGetTime64_t` | PSP2i | nothing to do; RELEASE_WAIT; seconds since 1970 |
| `sceKernelPowerLock`, `sceKernelPowerUnlock` | God of War, PSP2i | 0 |
| PGD files (`sceIoIoctl 0x04100001`) | PSP2i (the call and the missing-key report) | see `docs/DESKTOP_APP.md`, "PGD data"; decryption itself not yet checked on a real file |

## UNVERIFIED

Written from documentation; each logs `UNVERIFIED` on its first call.

- **ThreadManForUser**: `sceKernelCancelMsgPipe`, `sceKernelReferMsgPipeStatus`, `sceKernelGetThreadStackFreeSize` (the space below the stack pointer, not the never-written part a PSP measures), `sceKernelCheckThreadStack`.
- **IoFileMgrForUser**: `sceIoMkdir`, `sceIoRmdir`, `sceIoRemove` (memory stick only).
- **sceRtc**: `GetTick`, `SetTick`, `CompareTick`, `GetCurrentClock`, `GetDayOfWeek`, `GetDaysInMonth`, `IsLeapYear`, `CheckValid`, `GetTickResolution`, `TickAdd{Ticks,Microseconds,Seconds,Minutes,Hours,Days,Weeks}`; the calendar has unit tests (`portablekit_rtc_tests`). What an invalid date returns is not traced.
- **scePsmf**: `VerifyPsmf`, `SpecifyStreamWithStreamType`, `SpecifyStreamWithStreamTypeNumber`, `GetCurrentStreamNumber`, `GetAudioInfo`, `GetHeaderSize`, `GetStreamSize`, `GetPresentationStartTime`, `GetPresentationEndTime`; error codes are the library's documented ones.
- **sceDisplay**: `GetCurrentHcount`, `IsVblank` (from emulated time, 286 lines a frame); `GetFrameBuf`.
- **sceAudio**: `Output2ChangeLength`. **sceAtrac3plus**: `GetInternalErrorInfo`.
- **sceNetAdhocctl**: `Create`, `Connect`, `Join`, `GetNameByAddr`; **sceNet**: `EtherNtostr`; **sceNetAdhocDiscover**: `RequestSuspend`.
- **sceUtility**: `GetSystemParamString` (nickname).
- **scePspNpDrm_user**: `SetLicenseeKey`, `EdataSetupKey` (accepted; nothing decrypts with them yet).
- **sceUmdUser**: `Deactivate`.

## Not connected, by design

`hle_net_offline.cpp`. Every call logs `UNVERIFIED` on first use.

| Library | Answer |
| --- | --- |
| sceNetInet | Init/Term 0; sockets can be made; `connect` fails with ENETUNREACH, `send`/`recv` with ENOTCONN, `accept`/`recvfrom` with EWOULDBLOCK; `select` finds nothing ready; `InetAddr`/`InetNtop` convert; `GetErrno` reports the last errno |
| sceNetApctl | Init/Term/handlers 0; state disconnected; `GetInfo` and `Connect` fail |
| sceNetResolver | Init/Term/Create/Delete 0; `StartNtoA` fails |
| sceHttp, sceHttps | templates, connections and requests can be made; `SendRequest`, `ReadData` and the response queries fail |
| sceSsl | Init/End 0 |
| sceUtility | Load/UnloadNetModule 0; the browser dialog (`HtmlViewer`) opens and closes at once |

Ad hoc play is `hle_adhoc.cpp` (Yakumo's, generalised); `sceNetAdhocMatching` is not implemented yet.
