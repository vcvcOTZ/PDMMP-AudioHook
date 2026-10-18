# Changelog

All notable changes to **Audio Hook** for *Hatsune Miku: Project DIVA Mega Mix+*.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are the
DLL build tags. The mod manifest version in `config.toml` (`0.3.0`) is tracked separately
and only marks packaging changes.

---

## [13.1] — 2026-10-18

Fixes the issues raised in `AUDIT-v13.0.md` (48 findings; the ones deliberately
left open are listed at the end). The normal path (`fake_client=1`, `output=asio`)
behaves exactly as before — everything below is about failure paths, non-default
backends and non-44.1 kHz devices.

### Fixed — crash / process-level

- **fake_client=1 with asio=0 crashed on the first `ReleaseBuffer`.** The ring was
  allocated only `if (g_asioEn)`; `RingAvail()` on a NULL ring returns 0 without
  dereferencing, so the capacity check passed and `RingWrite()` memcpy'd into NULL.
  The ring is now always allocated, every ring access is NULL-guarded, and the
  combination is auto-corrected to `output = shared` (logged).
- **Exclusive rollback freed COM objects under its writer thread.** The code logged
  "leaking instead of crashing" and then released unconditionally. All teardown now
  goes through `WoShutdown()`, which waits up to 2 s for the thread to leave its
  loop and, if it does not, leaks on purpose.
- **`AsioPrepare()` leaked the driver on every failure path** after `init()`
  succeeded (wrong driver name, <2 outputs, bad rate, `createBuffers` failure,
  allocation failure). All paths now converge on one `fail:` label that calls
  `stop()` / `disposeBuffers()` / `Release()`, so the device is free again.

### Fixed — functional

- **ASIO rollback in fake mode was permanent silence.** With the client faked there
  is no real WASAPI stream to "return" to; the rollback now starts the shared
  backend, and `g_asioStartReq` is reset so a later retry is possible.
- **`shared` had no sample-rate guard:** a 48 kHz endpoint played the whole game
  8.8 % fast (~1.5 semitones up). Added a linear resampler (44.1 kHz → device rate),
  verified against 32 / 48 / 96 kHz: frame count within 2 frames, error ≤1 LSB.
- **`shared` reported success even when `Start()` failed**, which blocked the ASIO
  fallback. `WoStart()` now waits for the writer thread to report running (1 s).
- **`g_woAlive` was left set on the `Start()` failure path**, so rollback waited 2 s
  for a thread that had already exited.
- **Stale session-mute cleanup did nothing with the fake client**
  (`GetService(ISimpleAudioVolume)` → `0x88890001`). It now walks this process's
  sessions via `IAudioSessionManager2`, and runs a second time 5 s after boot (when
  stream2 / the output backend has created the session).
- **Half-patched vtables could hook our own hook.** `PatchSlot(a) && PatchSlot(b)`
  left `a` rewritten when `b` failed, and the retry captured our function as "the
  original" → infinite recursion / stack overflow. `PatchSlots()` now pre-checks all
  slots, installs all-or-nothing and rolls back on partial failure.
- **`FakeInit()` failure was returned to the game** instead of falling back, leaving
  it silent. It now restores the client and calls the real `Initialize`.
- **Stream1 client table overflow:** after 8 rebuilds new clients were dropped
  silently and stale pointers stayed behind. Both tables are rings now.
- **`disposeBuffers()` could race the ASIO callback** (use-after-free on the "device
  kicked" path). Added a callback-in-flight counter; teardown waits 200 ms and skips
  `disposeBuffers()` rather than freeing under a running callback.
- **SILENT releases were not tapped** (CRI sends `flags=0x2`), which starved the ring
  during silence and tripped the "runaway underruns" rollback. They now write zeros.
- **Config defaults contradicted the README** (`asio=0`, `fake_client=0`), so a
  missing/old config silently downgraded to v9. Both default to 1, and the log prints
  `MODE: v13 takeover` / `MODE: v9 buffer trim only`.
- **`exclusive2` dead code removed** (globals, thread, SetEventHandle branch, slot-13
  patching, `Hook_Initialize` rewrite). If an old config still sets it, the mod logs
  that it was removed and ignores it.
- **`Fake_Stop` / `Fake_Reset` now flush the ring**, so a restart/seek cannot replay
  up to 10 ms of the previous song.
- **Fake `IAudioClock::GetPosition` was frozen at 0** (`g_fakePlayed` was never
  written). It now reports frames actually consumed plus a real QPC timestamp.
- **`GetBuffer` used `AUDCLNT_E_BUFFER_TOO_LARGE` as back-pressure.** It now waits up
  to 5 ms for the consumer before failing (this is CRI's mixing thread, not a
  realtime callback).

### Fixed — concurrency, timing, resources

- Fake-driver drain no longer competes with a live consumer (it is gated on the writer
  thread actually reading, not on a state flag).
- `timeBeginPeriod(1)` at start-up: `Sleep(1)` in the handoff no longer rounds up to
  the 15.6 ms default tick.
- Fake mode skips the tap-and-zero handoff entirely (it used to discard up to 10 ms of
  already mixed audio at every song start).
- `MsNow()` guards against a zero QPC frequency (NaN used to make the handoff wait
  exit immediately).
- Render-client slot writes are ordered (length, barrier, pointer) so the release hook
  cannot pair a new pointer with an old length.
- `Log()` writes one complete line per `fwrite` (no more timestamp/body interleaving),
  appends instead of truncating, and rotates at 4 MB to `audio_hook.log.1`.
- `WoInit()` releases the client / event / format on every failure path; a failed
  exclusive attempt no longer leaves the device claimed.
- A failed backend can be retried (`g_woState == 4` is claimable again).
- Monitor loops no longer stop after 24 h / 1 h.
- `AsioChooseBlock` no longer spins forever when a driver reports `minBuffSize = 0`.
- `getChannelInfo` failures and unsupported sample types now abort ASIO preparation
  (they used to run silently, producing silence or noise).
- Unknown ASIO formats are cleared at their real sample width (8-byte formats used to
  keep half of every frame's stale data).
- Fake `GetMixFormat` returns a valid 44.1 kHz / 16-bit / stereo format before
  `Initialize` is ever called.
- Float detection checks `WAVEFORMATEXTENSIBLE.SubFormat` (32-bit integer PCM was
  interpreted as float → full-scale noise).
- Config parser works line by line (a line without `=` used to swallow the following
  keys).
- `[notify]` timestamps are labelled `ms` (they always were milliseconds).
- Pointer logging uses `0x%llX` instead of `%p`.
- Boot device probing runs for 3 s instead of 20 s, and the `[out]` statistics line
  only appears when a WASAPI backend is in use.

### Deferred (documented, not changed)

- **P2-4** — the exclusive self-test still runs synchronously on the game's init
  thread (~600 ms) and claims the device before stream2 exists.
- **P3-4** — `CoInitializeEx` on worker threads is not paired (they live for the
  process).
- **P3-5** — the endpoint-notification callback is not unregistered at detach (COM
  calls inside `DllMain` are unsafe).
- **P4-13** — device names are still converted with `CP_ACP`.
- **P4-14** — the detach report is still written from `DllMain`.
- **P5-6** — the single-file layout is unchanged (hook / fake client / backends / diag
  in one translation unit).

### Build

Compiles with **zero warnings at `/W3` and `/W4`.

---

## [13.0] — 2026-10-17

The release that removes the engine buffer from the audio path.

### Added

- **stream1 client takeover.** `IAudioClient::Initialize` for the game's audio stream is
  intercepted and the real one is **never called**; CRI is handed our own
  `IAudioClient` / `IAudioRenderClient` / `IAudioClock`. The game mixes into our memory, so
  the engine buffer (22 ms) is out of the path and the device stays free.
- **Output backend selection** (`output` in `audio_hook.txt`): `asio`, `shared`, `excl`,
  with staged fallback — exclusive → shared → ASIO, each stage logged.
- **600 ms self-test for `excl`.** If the device delivers less than 95 % of real time, the
  stream is torn down and the backend falls back to `shared` automatically.
- **Arbitrary channel counts** on the output side: FL/FR are mapped, extra channels are
  silenced, so 5.1 / 7.1 endpoints work.
- **Companion tools** shipped in `tools/` and built by CI: `asio_list`,
  `excl_rate_probe`, `session_tool` (usage below and in the README).
- CI: `permissions: contents: write`, so a `v*` tag build attaches all four binaries to the
  Release instead of failing at the upload step.

### Fixed

- `shared` backend sent a truncated `WAVEFORMATEX` (18 bytes) where the driver expected the
  full `WAVEFORMATEXTENSIBLE` (40 bytes) → `Initialize` returned `E_INVALIDARG`.
- Output writer released `buffer − padding` frames unconditionally. A device whose engine
  buffer is larger than CRI's queue (1056 vs 441 frames) therefore got zero-padded every
  period, i.e. an audible underrun on every callback. It now releases only what the ring
  actually holds.
- The fake client was flagged as ours **before** the vtable patch ran; a failed patch left
  the hooks serving an uninitialised client. It is now flagged only after success.
- `excl` rollback released the COM objects while the writer thread could still be inside
  `WaitForSingleObject` → use-after-free. The rollback now waits for the thread to exit
  (up to 2 s) and leaks rather than frees if it does not.
- `excl` underrun accounting (per-write shortage instead of per-callback).

### Changed

- With `fake_client = 1` the tap-and-zero handoff is no longer part of the audio path —
  there is no real stream left to hand off from.
- README restructured around the three backends.

### Measured (Focusrite Scarlett Solo 3rd Gen, 44.1 kHz)

| Component | Value |
|---|---|
| CRI staging buffer handed to the game | 441 frames = **10.0 ms** |
| ASIO block | 64 frames = **1.45 ms** |
| ASIO driver-reported latency | 181 frames in/out = **4.1 ms** |
| WASAPI shared engine buffer | 970 frames = **22.0 ms** |

Steady state over 25 s runs: **44,100 frames/s, 0 underruns, 0 dropped frames** on both
`asio` and `shared`. Exclusive: Focusrite **88.5 %**, Realtek onboard **0.3 %** of real
time → both roll back automatically.

---

## [12.0] — 2026-10-17 *(experimental, never released)*

### Added

- `exclusive2`: EXCLUSIVE + polling prototype — drop `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`
  and pace CRI from our own thread, so the engine clock would no longer lead the device.

### Removed

- **Abandoned.** With polling, the Focusrite driver only grants **1768 frames (40 ms)**
  instead of 221 frames (5 ms), which is worse than shared mode and fails CRI's own buffer
  validation → no audio, game hangs. Exclusive + event callbacks keeps 5 ms but inherits
  the ~88 % delivery rate, which cannot be fixed in software.

---

## [11.3] — 2026-10-17

### Added

- `asio_list` companion tool: lists registered ASIO drivers, plays a 3 s 440 Hz tone
  (0.12 amplitude, 120 ms fade-in) so you can hear which output a driver drives.

### Fixed

- `WaitForTapSteady()` could spin forever if the game's audio never reached real time;
  it now times out after 30 s and keeps the plain WASAPI path.
- The ASIO realtime callback called `fprintf` + `fflush`; disk I/O on the audio thread
  made start-up underruns worse. Callbacks now only bump atomic counters, reported once
  per second by the monitor thread.
- `asio_list`: silence was written with a fixed 4-byte frame size, so 16-bit drivers
  (Realtek) had twice their buffer overwritten → heap corruption and a crash after
  playback. Frame size is now derived from the stream's sample format.
- `asio_list`: block size clamped to the driver's min/max/granularity; `outputReady()` is
  only called when the driver supports it.

### Robustness (audit)

- Verified distribution: x64, `-MT` static CRT, imports only `KERNEL32` / `USER32` /
  `ole32` / `ADVAPI32` — no VC++ runtime required on the target machine.

---

## [11.2] — 2026-10-17

### Removed

- All WASAPI exclusive-mode experiment code (proven non-realtime on this hardware).

### Fixed

- A stale per-app session mute left behind by older builds made the game silent after the
  mod was disabled. It is now cleared automatically on boot (Windows persists that state
  per executable path).

---

## [11.1] — 2026-10-17

### Fixed

- `SetMute` residue surviving in the Windows per-app volume store ("no sound after
  disabling the mod"). Replaced by the tap-and-zero handoff, plus a boot-time cleanup.

---

## [11.0] — 2026-10-17

### Added

- **ASIO bridge**: tap CRI's render buffer → lock-free ring (65,536 frames) → ASIO output,
  with `asio_driver` (substring match), `block`, `handoff_ms`, `mute_mode` options.
- tap-and-zero handoff: copy the frames, zero the game's buffer, let the engine queue drain,
  then continue on ASIO without overlap.
- Self-healing: if the stream is invalidated (`AUDCLNT_E_DEVICE_INVALIDATED`), the bridge
  detaches and the game returns to WASAPI within ~1 s.
- Sample-rate guard: refuses to bridge on a device that is not 44.1 kHz (no pitch shift).

### Fixed

- **De-interleave bug** inherited from v10d: interleaved stereo was written straight into
  each ASIO channel, halving the time base → one octave down plus tearing.
- ASIO drivers must be loaded on an **STA** thread (MTA returns `E_NOINTERFACE`).

---

## [9.0] — 2026-10-16

### Added

- **WASAPI buffer optimization** (still the fallback behaviour, and what you get with
  `fake_client = 0`): dropping `AUDCLNT_STREAMFLAGS_RATEADJUST` on stream1 takes the
  engine buffer from 44.5 ms down to the 22 ms platform floor; stream2 is rewritten from
  66.7 ms to 22 ms.

### Removed

- WASAPI exclusive output. Across eight endpoints the device delivered only 86–98 % of real
  time (the device clock itself running slow), so playback was time-stretched and the
  music audibly repeated/rolled back. Replaced by the ASIO bridge in v11.

---

## Companion tools — quick reference

Full details, sample output and how to read the numbers are in the
[README](README.md#companion-tools).

### `asio_list` — which driver is which?

```bat
asio_list                          :: interactive: Up/Down, Enter = 3 s tone, Q = quit
asio_list --list                   :: plain list
asio_list --test "Focusrite USB"   :: play on that driver directly
```

Copy the **full** driver name it prints into `asio_driver`. Matching is a substring search
that takes the first hit, so a short string can select a different driver.

### `excl_rate_probe` — is exclusive mode real?

```bat
excl_rate_probe --list
excl_rate_probe "Realtek" --seconds 2
excl_rate_probe default --shared
```

Read the **`device clock`** percentage from the summary: ≥ 95 % usable, 90–95 % marginal,
< 50 % the driver never starts the hardware clock (the mod would roll back).

### `session_tool` — why is it silent?

```bat
session_tool                    :: every render session: state / muted / volume / peak
session_tool --unmute DivaMegaMix
```

Run it while the game is running. `[Active]` with `peak=0.0000` means an open stream
carrying digital silence, not a missing stream.
