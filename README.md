# Audio Hook

> Low-latency audio mod for **Hatsune Miku: Project DIVA Mega Mix+** (Steam).

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Game](https://img.shields.io/badge/game-PDMM%2B-7b5cff)
![Version](https://img.shields.io/badge/version-v14.0-informational)
![License](https://img.shields.io/badge/license-MIT-blue)

[English](README.md) · [简体中文](README.zh-CN.md)

Replaces the game's audio **client** (not its code) so the engine buffer disappears from the path, then renders the game's PCM through a backend of your choice: **ASIO**, **WASAPI exclusive** or **WASAPI shared**.

Only COM **vtable** slots are patched — the executable is left byte-for-byte untouched, so the game's anti-tamper (Denuvo) is not triggered.

---

## Table of Contents

- [Why this exists](#why-this-exists)
- [Features](#features)
- [Measured buffer sizes](#measured-buffer-sizes)
- [Requirements](#requirements)
- [Installation](#installation)
- [Configuration](#configuration)
- [Output backends](#output-backends)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Portability](#portability)
- [Companion tools](#companion-tools)
- [Building from source](#building-from-source)
- [Known limitations](#known-limitations)
- [Changelog](CHANGELOG.md)
- [简体中文](README.zh-CN.md)
- [License](#license)

---

## Why this exists

PDMM+ uses a **CRI ADX2** audio stack that opens two WASAPI shared-mode streams with oversized buffers:

| Stream | Content | Format | Engine-granted buffer |
|--------|---------|--------|----------------------|
| stream1 | game audio | 16-bit / 44.1 kHz | **44.5 ms** (asks for 10 ms) |
| stream2 | movie audio | 32-bit float / 44.1 kHz | **66.7 ms** |

The 44.5 ms is caused by the `AUDCLNT_STREAMFLAGS_RATEADJUST` flag. Trimming it only gets you to the platform's shared-mode floor (22 ms on most drivers), because the audio still has to pass through the engine buffer.

v13 takes a different route: **CRI is handed a client object owned by this mod.** The game mixes into our memory, so the engine buffer is not in the path at all, and the audio device stays completely free for a backend we control.

---

## Features

### 1. Client takeover *(always active, `fake_client = 1`)*

- `IAudioClient::Initialize` is intercepted for stream1; the real `Initialize` is **never called**.
- CRI instead talks to our own implementation (`GetBufferSize`, `GetCurrentPadding`, `GetBuffer`, `ReleaseBuffer`, `SetEventHandle`, `GetService`, `Start`/`Stop`/`Reset`, …).
- Result: **no 22 ms engine buffer, no WASAPI stream holding the device.**
- CRI is paced from the real output clock (`FakePoke()` runs inside the output callback), so producer and consumer share one clock and cannot drift.

### 2. Three output backends (`output = …`)

| Backend | What it does |
|---|---|
| `asio` *(default)* | Native ASIO driver. Lowest, most consistent result on the test hardware. |
| `shared` | Our own WASAPI shared-mode stream. Works on **any** device, no ASIO driver required. |
| `excl` | WASAPI exclusive in **push mode** (no event callbacks) with a target fill of `excl_target_ms`, 12 ms by default. A watchdog monitor compares the **device clock** with the wall clock once a second and falls back to `shared` if it stays below 99.5 % for three seconds. Measured 99.9 % delivery with zero injected silence on the test device; stable and usable since v14.0 — it only costs you the endpoint (movie audio goes silent), and `asio` is still the default. |

With `asio_driver = *` (the default) the ASIO backend tries every registered driver in
registry order and keeps the first one that fully initialises — the log shows each attempt
and the driver that won, so a machine with several drivers is never silently routed to a
disconnected or virtual device. Probing really `init()`s each candidate (bounded at 8), and
some drivers pop a control panel or briefly claim the device while that happens, so on a
machine with several ASIO drivers (ASIO4ALL, FlexASIO, virtual mixers, …) run `asio_list`
once and pin the interface's name in `asio_driver` to skip the probe.

### 3. Anti-tamper safe

Only vtable slots are patched (data layer):

- `IAudioClient` — `[3]` `Initialize`, `[4]`–`[12]` (`GetBufferSize` … `Reset`), `[13]` `SetEventHandle`, `[14]` `GetService`
- `IAudioRenderClient` — `[3]` `GetBuffer`, `[4]` `ReleaseBuffer`

No code bytes are written. The game's own **COM implementation** is replaced (our client answers `GetBufferSize` / `GetBuffer` / …), which is exactly what keeps the engine buffer out of the path — but no executable memory is modified.

### 4. Robustness

- **All-or-nothing vtable patches.** Both the client takeover (9 slots) and the render-client tap (2 slots) are installed as a set; if any slot cannot be patched, the ones already written are rolled back. A half-patched table used to leave `slot 3 = our hook` while the "patched" flag stayed 0, so the next attempt hooked our own function onto itself (stack overflow).
- **Staged fallback with real verification:** `output = excl` → `shared` → ASIO. Each stage now *waits* for its thread to report success, so a failed `Start()` no longer looks like a working backend.
- **Exclusive monitor** (watchdog thread, one sample a second): the **device clock** (`IAudioClock`) is compared with the wall clock, and three consecutive seconds below 99.5 % tear the stream down and fall back to `shared`. The bar is that high because the old event-mode backend did not jitter around 96 % — its device clock genuinely ran 3.5 % slow (the engine skips the beat it is not fed, so the whole timeline drifts against real time). The same reading raises the fill target by 2 ms while the writer is starving.
- **Fake mode has a real fallback:** if ASIO dies while the client is faked, the mod starts the shared backend (`WoStart(0)`) instead of the old "game audio returns via WASAPI", which was impossible with no real stream.
- **No engine handoff in fake mode:** the tap-and-zero dance (resync + 23 ms wait) is skipped, so starting a song no longer discards ~10 ms of already mixed audio.
- **Ring memory is allocated unconditionally** and every ring access is NULL-guarded, so `fake_client = 1` + `asio = 0` (previously a guaranteed crash) now works and is auto-corrected to `output = shared`.
- **Arbitrary channel counts** on the output side (2.0 / 5.1 / 7.1 devices), FL/FR mapped, extra channels silenced.
- **Partial writes**: only as many frames as the ring actually holds are released, so a device whose engine buffer is larger than CRI's queue does not get zero-padded every period.
- **Bounded waits**: the tap-steady wait times out after 30 s; the exclusive rollback waits for its writer thread to actually exit (2 s) and leaks rather than frees if it doesn't (no use-after-free).
- **Dead output stream is rebuilt**: if the shared backend's device disappears (headphones unplugged, default device changed, stream pre-empted), the writer marks the backend failed instead of spinning in silence, and a watchdog restarts it (3 s backoff, up to 5 attempts, every attempt logged).
- **Realtime-safe callbacks**: no file I/O and no allocation inside the ASIO/WASAPI callbacks; counters are reported by a monitor thread.
- **Session mute cleanup** now walks the process's own audio sessions (`IAudioSessionManager2`), so it also works when stream1 is faked — it used to fail with `AUDCLNT_E_NOT_INITIALIZED` and silently do nothing.
- **Logs**: one atomic write per line, append mode with rotation (`audio_hook.log.1`), so the run that crashed is still readable.

---

## Measured buffer sizes

Objective numbers, measured with a USB ASIO interface at 44.1 kHz:

| Component | Value |
|---|---|
| CRI staging buffer we hand the game | 441 frames = **10.0 ms** (`FAKE_FRAMES`) |
| ASIO block size | 64 frames = **1.45 ms** |
| ASIO driver-reported latency | 181 frames in / out = **4.1 ms** |
| WASAPI shared engine buffer | 970 frames = **22.0 ms** @44.1 kHz (1056 @48 kHz) |
| WASAPI exclusive, push mode | ≈ **12 ms** (`excl_target_ms`; the granted buffer is 3528 frames = 80 ms, but only the target fill is held) |

Steady-state validation of the `asio` and `shared` backends: **44,100 frames/s delivered, 0 underruns, 0 dropped frames** over 25 s runs.

> Exclusive mode is **stable and usable** since v14.0: it is a push-mode writer (not event callbacks), which is what brings it to 99.9 % delivery with zero injected silence. It does claim the endpoint exclusively, so movie (stream2) audio is silent, and `asio` remains the default.

---

## Requirements

- **Hatsune Miku: Project DIVA Mega Mix+** (Steam).
- **DML (Diva Mod Loader)** — `dinput8.dll` + `mods/<name>/config.toml`.
- *(Only for `output = asio`)* an **ASIO driver** running at **44.1 kHz**.

---

## Installation

1. Make sure DML is installed (`dinput8.dll` in the game root).
2. Copy the `Audio Hook` folder into `mods/`:

   ```
   mods/Audio Hook/
   ├── config.toml
   ├── audio_hook.dll
   └── audio_hook.txt
   ```

3. Add `"Audio Hook"` to the root `config.toml` `priority` list:

   ```toml
   priority = ["Audio Hook", "Disable Watermarks", "High Frame Rate", "ExPatch"]
   ```

4. Launch the game and check `mods/Audio Hook/audio_hook.log` — the header must read `v14.0` and `MODE: v13 takeover`.

---

## Configuration

`mods/Audio Hook/audio_hook.txt`:

```toml
fake_client = 1
output      = asio
asio        = 1
asio_driver = *          # "*" = auto (first usable driver); or pin a name from asio_list
block       = 64
handoff_ms  = 0
mute_mode   = 1
```

| Key | Default | Meaning |
|-----|---------|---------|
| `fake_client` | `1` | Replace CRI's stream1 client with ours. **Required** for any non-ASIO backend; without it you get the old v11 behaviour. |
| `output` | `asio` | `asio` / `shared` / `excl`. |
| `asio` | `1` | Enable the ASIO worker when `output = asio`. |
| `asio_driver` | `*` | Case-insensitive substring match against the names listed by `asio_list`. `*` (or `auto`, or empty) = **first driver that actually initialises**: every registered driver is tried in turn until one reports ≥2 outputs, 44.1 kHz and a usable sample format. Each probe calls the driver's `init()`, which can pop a control panel or briefly claim the device — pin the exact name (from `asio_list`) when several drivers are installed. |
| `block` | `64` | ASIO block size (16…1024). |
| `handoff_ms` | `0` | Handoff delay; `0` = auto. |
| `mute_mode` | `1` | `1` = tap-and-zero handoff; `0` = legacy `SetMute`. |
| `excl_period_ms` | `0` | Requested exclusive **buffer depth** in ms; `0` = 20 ms (the engine rounds it up, 80 ms / 3528 frames on the test device). It no longer describes an event period — exclusive is push mode since v14.0. |
| `excl_target_ms` | `12` | Exclusive push writer's target fill in ms (6…30) — this is the steady-state latency. The watchdog nudges it **up** 2 ms only when the device clock reads below 99.5 % for two seconds in a row, and **down** 1 ms after ten clean seconds; it never goes below the value you set. |

---

## Output backends

### `asio` (recommended)

Loads the ASIO driver on an **STA** worker thread, taps CRI's PCM directly from our client, and plays it out with a lock-free ring in between. The ASIO callback also paces CRI (`FakePoke`), so the two never drift.

### `shared` (no ASIO driver required)

Opens a normal WASAPI shared stream on the default endpoint (mix format, float32 or 16-bit) and writes the frames the ring currently holds. A mix format of 24-bit packed or 32-bit *integer* PCM is refused (the log says `refusing`, and the mod falls back to `asio`) — writing it as int16 would be full-scale noise. This keeps the engine's ~22 ms buffer in the path — that buffer is a platform floor, not something a client can shrink — but it removes CRI's own buffer and works everywhere.

If the endpoint is **not 44.1 kHz** (48 kHz HDMI / Bluetooth / onboard is common), the backend resamples 44.1 kHz → device rate with linear interpolation (≈1 frame of latency, inaudible at these ratios) instead of playing the whole game 8.8 % fast. The log line says `resample=yes`.

### `excl`

Opens `AUDCLNT_SHAREMODE_EXCLUSIVE` **without** event callbacks, on a real (multi-period)
buffer. Because exclusive mode bypasses the engine, the device must be free — and since
stream1 no longer opens a real stream, it is.

It also offers several formats in turn instead of only 44.1 kHz / 16-bit: 44.1 kHz 16-bit
first (no resampling), then 32-bit float, then the device's own mix rate with either — so an
endpoint that only answers 48 kHz or float in exclusive mode no longer makes the backend fail
outright. The rate difference is handled by the same resampler the shared backend uses.

Event callbacks are what used to cap this backend. In exclusive mode they force
`duration == periodicity`, i.e. a buffer exactly one period deep, so the deadline after
every event is **zero**: any wake-up that misses the period loses that beat's audio. A
standalone probe measured **96.5 % while completely idle**, and — the part that made the
old 90 % gate useless — the *device clock itself* ran 3.5 % slow, because the engine skips
the beat instead of substituting silence. That is not stutter, it is the whole timeline
(music and chart together) drifting against real time.

v14.0 drops the event mode entirely. `WoPushThread` polls every millisecond and tops the
buffer up to `excl_target_ms` of **not-yet-heard** audio — device fill plus ring backlog —
with a device-fill floor so the device itself is always pulled from first and can never be
left thin. The plan's measurements: **99.9 % delivery, zero injected silence**, with or
without six CPU-hogging threads, not even needing MMCSS (which is still enabled because it
is free). Some devices are genuinely unusable: one onboard codec initialises but keeps its
device clock at **~0.3 %**.

The watchdog then samples the device clock once a second against the wall clock and rolls
back to `shared` when it stays below **99.5 %** for three consecutive seconds — a far
higher bar than the old 90 %, and deliberately so. The same reading drives the fill target,
with hysteresis in the sane direction: only two consecutive seconds below 99.5 % raise it
(2 ms at a time, capped at 30), ten clean seconds lower it by 1 ms, and it never drops below
the configured value. The window is timed with `QueryPerformanceCounter`, **not**
`GetTickCount64` — the latter ticks at ~15.6 ms, and ±1.6 % of quantisation error over a one
second window is wider than the deviation being measured; it used to walk the target, and
with it the whole audio latency, upward on pure noise (which shows up as the music drifting
further and further behind the chart).
After a genuine fall-back, exclusive is **not re-attempted** for the rest of the process — the
watchdog only rebuilds `shared` — so changing `output = excl` to stick requires a game
restart. Use `tools/excl_rate_probe.exe` to check your own device:

```
excl_rate_probe --list
excl_rate_probe "Speakers" --seconds 2
```

`excl_period_ms`, when set, is the requested **buffer depth** in ms; `0` asks for 20 ms and the engine rounds it up (80 ms / 3528 frames in the plan's measurements). `excl_target_ms` is the fill the writer holds, which is the steady-state latency — 12 ms by default. The device is held exclusively either way, so `excl` still costs you movie (stream2) audio; it is still not the default (`asio` is), but since v14.0 it is a stable and usable backend.

---

## How it works

```
CRI ADX2 mix ──▶ [our IAudioClient]  (GetBuffer / ReleaseBuffer, 441 frames)
                        │
                        ▼
              [lock-free ring 65536 fr]
                        │
        ┌───────────────┼────────────────┐
        ▼               ▼                ▼
   ASIO driver    WASAPI exclusive   WASAPI shared
        │               │                │
        └────── FakePoke() drives CRI from the output clock ──────┘
```

1. `Initialize` for stream1 is intercepted; the real one is skipped and our vtable slots are installed.
2. `GetService(IAudioRenderClient)` returns our render client; `Start` launches the driver thread.
3. Each output callback pulls from the ring and, when the queue runs low, signals CRI's event so it mixes the next chunk.
4. Because the signal comes from the real output clock, production and consumption are locked together — no clock drift (the `shared` backend resamples only to convert 44.1 kHz → device rate, never to chase drift).
5. A monitor thread reports callbacks / underruns / queue depth once per second.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No sound at all | Check the log for `[fake] stream1 client replaced` and either `[asio] started` or `[wo] output running`. If a backend failed, the mod logs the stage it fell back to. |
| Sound stutters / repeats | Only expected on `output = excl` with a bad driver; v13 should self-test and fall back. Set `output = asio` (or `shared`). |
| No sound after disabling the mod | Run `session_tool.exe --unmute DivaMegaMix` (older builds left a persistent session mute); v11.2+ clears it automatically. |
| Game exits silently a few seconds after launch | **Game's own bug** (foreground-focus check → `WinMain` returns early). Keep the window focused during startup, or install the community `MegaMixPlus-BackgroundLaunchFix` (`d3d11.dll` proxy). |
| Disable everything | Drop an empty file `audio_hook.off` next to `audio_hook.dll`. |
| Back to the old behaviour | Set `fake_client = 0` and `asio = 0`: pure v11 WASAPI buffer optimization. |

Logs: `audio_hook.log` (main), `audio_hook_detach.log` (detach diagnostics).

---

## Portability

- **Any device, no ASIO driver:** `fake_client = 1`, `output = shared`.
- **ASIO interface:** `fake_client = 1`, `output = asio`, and leave `asio_driver = *` (auto) or pin the name reported by `asio_list`. The interface must run at 44.1 kHz; otherwise the bridge refuses and falls back.
- **WASAPI exclusive:** `output = excl` — stable and usable since v14.0; run `excl_rate_probe` first, and the mod still falls back to `shared` if your device's clock cannot keep up.
- Multi-channel outputs (5.1/7.1) are handled: FL/FR are mapped, remaining channels are silenced.

---

## Companion tools

Three small console tools ship with the mod, in `tools/` (also attached to every
[Release](https://github.com/vcvcOTZ/PDMMP-AudioHook/releases), or build them with
`src/build.bat`). None of them touch the game — they are read-only diagnostics, safe to
run while the game is playing unless noted.

| If you want to know… | Run |
|---|---|
| which ASIO driver is which, and what to put in `asio_driver` | `asio_list` |
| whether WASAPI exclusive is actually usable on my device | `excl_rate_probe` |
| why the game is silent (session muted / no peak) | `session_tool` |

### `asio_list` — identify and audition ASIO drivers

```
asio_list                          interactive menu (arrow keys)
asio_list --list                   plain text list, no menu
asio_list --test "USB ASIO"        play the tone on that driver directly
asio_list --test "*"               play on the first registered driver
asio_list --help
```

Exit codes: `0` played · `1` no drivers registered · `2` nothing matched / playback
failed · `3` bad command line · `4` the driver crashed.

The interactive menu lists every driver registered under
`HKLM\SOFTWARE\ASIO`; **Up/Down** moves the selection, **Enter** plays a 3 s 440 Hz
tone on it, any key stops, **Q** quits.

```
=== ASIO Driver Test  (5 drivers) ===
  Up/Down: select   Enter: play 3 s tone   Q: quit

  [ 1] USB Audio ASIO Driver
  [ 2] Thunderbolt Audio ASIO
  [ 3] Onboard ASIO
  [ 4] Virtual Mixer ASIO       <- highlighted = selected
  [ 5] Guitar Multi-Effects ASIO
```
The tone is deliberately gentle: `0.12` amplitude with a 120 ms fade-in, so it will not
blast your monitors. **Whichever speaker/headphone the tone comes out of is the physical
output that driver drives** — that is the whole point of the tool.

Use the printed name in `asio_driver`. Matching is a **case-insensitive substring**
search that takes the **first** hit, so a short string can select the wrong driver (on a
machine that registers both `Thunderbolt Audio ASIO` and `USB Audio ASIO`, writing
`Audio` picks the first one). Always copy the full name.

A name that matches **nothing is an error** (`exit 2`, the registered names are printed):
it is never silently redirected to the first driver, which used to let you "verify" the
wrong device.

Drivers that crash while **loading or initialising** (it happens) are caught
with SEH and reported as `driver crashed`, so one bad driver does not stop you from testing
the rest. Each test closes its stream (`stop` → `disposeBuffers` → `Release`) and waits for
the audio callback to leave before freeing the buffers, so the next driver really gets the
device. (A driver that faults *inside its own callback* runs on its own thread, outside our
SEH frame — that can still take the process down; nothing in user mode can prevent it.)

### `excl_rate_probe` — is exclusive mode real on this device?

```
excl_rate_probe --list                       list render endpoints, default marked
excl_rate_probe "<device substring>" [--seconds N] [--shared]
```

Defaults: the **default render endpoint**, **2 s per format** (`--seconds` accepts 1…60;
a bad value or an unknown option is a usage error, not silently ignored). It tries every
plausible format (44.1 / 48 kHz × 16 / 24 / 24-in-32 / 32-bit float), re-activates the
client for each one, and only trusts a format if the **device clock really advances** —
`Initialize()` returning `S_OK` proves nothing.

Exit codes: `0` measured ≥ 95 % of real time · `3` measured below that (or nothing
worked) · `1` device/COM error · `2` bad command line.

```
-- 44100 Hz / 16 bit / 2 ch --
   initialized: buffer 441 fr (10.00 ms) - measuring...
   -> written 43773 fr/s, device clock 43950 fr/s

== SUMMARY 'Speakers (USB Audio)'
   best: 44100 Hz / 16 bit / 2 ch  buffer 441 fr  ->  99.7% of real time
```

| Line | Meaning |
|---|---|
| `written … fr/s` | frames the tool actually pushed into the buffer |
| `device clock … fr/s` | `IAudioClock` advance, converted to frames — **the honest number** |
| `(352800 units/s)` | the clock's own tick rate; `GetFrequency()` is used to normalise it |
| `not supported (0x88890008)` | the driver refuses that format in exclusive mode |
| `Initialize 0x88890019` | buffer size not aligned — that candidate is skipped |
| `(clock not advancing)` | the clock object exists but its position never moved — scored 0 % |

> Device clocks do **not** count in samples at the nominal rate: a shared-mode stream was
> measured reporting **352800 units/s** (8× 44.1 kHz). The tool divides by
> `GetFrequency()` before turning the number into a percentage, so 100 % really means
> real time. If a driver has no usable `IAudioClock`, the written rate is used instead and
> the line says so.

How to read the summary:

- **≥ 99.5 %** — exclusive is genuinely usable; `output = excl` will keep it. (The mod's own gate — see the `excl` section. A lower reading here no longer means the mod will drop it, but push mode is built to sit at 99.9 %.)
- **90–95 %** — marginal: the queue slowly drains or fills, expect drift/stutter.
- **< 50 %** — the driver accepted the stream but never starts the hardware clock;
  exclusive cannot be used on that device.

That last case is exactly why the mod monitors `excl` on the watchdog thread and rolls
back to `shared` if the device clock stays below 99.5 % for three consecutive seconds.
`--shared` runs the same measurement on a normal shared
stream, which is the quickest way to prove an endpoint is alive at all.

### `session_tool` — session mute / volume / peak inspector

```
session_tool                    list sessions on every render device
session_tool --list             same, explicit
session_tool --unmute <substr>  clear mute on every session whose process name matches
session_tool --mute <substr>    set mute (diagnostic only)
```

The match is **case-insensitive** (`divamegamix` works), the system-sounds session
(`pid 0`) is never touched, and the exit code is `2` when a pattern matched nothing —
so a script can tell "unmuted" from "the game was not running". A plain-text report is
appended next to the exe (`session_tool_report.txt`, rotated to
`session_tool_report.1.txt` at 4 MB), and both device and process names are printed as
UTF-8, so Chinese/Japanese names are readable and still matchable.

```
== device 'Speakers (USB Audio)' [{...}]
    session pid=16672  DivaMegaMix.exe   [Active] muted=no volume=1.00 peak=0.4182
    session pid=13564  msedge.exe        [Inactive] muted=no volume=1.00 peak=0.0000
```

`peak` is the live `IAudioMeterInformation` value — the fastest way to tell "the game has
an active stream but is outputting digital silence" (state `Active`, `peak=0.0000`) from
"the game is simply not playing anything".

Run it **while the game is running** — the session only exists then. `--unmute DivaMegaMix`
fixes the persistent per-app mute that builds before v11.1 could leave behind (Windows
remembers that state per executable path, including after the mod is removed).

---

## Building from source

- **MSVC** (Visual Studio Build Tools) + Windows SDK.
- ASIO SDK headers are bundled in `src/asiosdk/`.
- Sources: `src/audio_hook.cpp`, `src/asio_list.cpp`, `src/excl_rate_probe.cpp`.
- `src/build.bat` locates the toolchain via `vswhere` and builds the DLL.

```
cl /nologo /utf-8 /LD /MT /O2 /W3 /I asiosdk /Fe:audio_hook.dll /Fo:obj\ audio_hook.cpp /link /MACHINE:X64
```

> Copy the resulting `audio_hook.dll` into `mods/Audio Hook/`.

---

## Known limitations

- **WASAPI exclusive is a push-mode writer** (since v14.0): a millisecond writer holding `excl_target_ms` (12 ms by default) of not-yet-heard audio, monitored by the device clock against the wall clock. The old event-callback backend measured 96.5 % even idle, with the device clock itself 3.5 % slow; push mode measures 99.9 % with zero injected silence and is stable enough to use. Some devices (an onboard codec) never move their clock (~0.3 %) and are still rolled back to `shared`. It holds the endpoint exclusively, so movie (stream2) audio is silent, but `asio` remains the default.
- **The exclusive monitor runs on the watchdog thread**, overlapped with playback, so `output = excl` never delays the game's audio initialisation — it used to add ~2.2 s on the game's init thread. Verdicts land within the first seconds of playback, and it takes three consecutive sub-99.5 % seconds before it falls back.
- **Movie audio (stream2)** stays on a real WASAPI stream in `asio`/`shared` mode. In `excl` mode the device is taken exclusively, so stream2 fails to initialize (`AUDCLNT_E_DEVICE_IN_USE`) and cutscene audio is silent — this is expected and harmless for gameplay.
- **Shared mode cannot go below the engine buffer** (~22 ms on most drivers). That is a platform floor, independent of this mod.
- Only the default render endpoint is used by `shared`/`excl`.
- **No more spurious ASIO rollback:** the `asio` backend no longer rolls back to `shared` at song/stream transitions (fixed in v13.9 by an underrun grace window plus a producer-idle guard), and the ring discard that used to race the consumer thread was closed in v13.10 — the read cursor is published with a compare-exchange, so a discard can never be undone by an in-flight read.

---

## 中文说明

完整中文文档请见 [README.zh-CN.md](README.zh-CN.md)。

---

## License

[MIT](LICENSE) © 2026 vcvcOTZ
