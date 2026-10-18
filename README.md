# Audio Hook

> Low-latency audio mod for **Hatsune Miku: Project DIVA Mega Mix+** (Steam).

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Game](https://img.shields.io/badge/game-PDMM%2B-7b5cff)
![Version](https://img.shields.io/badge/version-v13.2-informational)
![License](https://img.shields.io/badge/license-MIT-blue)

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
- [中文说明](#中文说明)
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
| `excl` | WASAPI exclusive, 5 ms period when the driver honours it. Guarded by a **600 ms self-test**: if the device delivers less than 95 % of real time, the backend is torn down and the mod falls back to `shared`. |

### 3. Anti-tamper safe

Only vtable slots are patched (data layer):

- `IAudioClient` — `[3]` `Initialize`, `[4]`–`[12]` (`GetBufferSize` … `Reset`), `[13]` `SetEventHandle`, `[14]` `GetService`
- `IAudioRenderClient` — `[3]` `GetBuffer`, `[4]` `ReleaseBuffer`

No code bytes are written. The game's own **COM implementation** is replaced (our client answers `GetBufferSize` / `GetBuffer` / …), which is exactly what keeps the engine buffer out of the path — but no executable memory is modified.

### 4. Robustness

- **All-or-nothing vtable patches.** Both the client takeover (9 slots) and the render-client tap (2 slots) are installed as a set; if any slot cannot be patched, the ones already written are rolled back. A half-patched table used to leave `slot 3 = our hook` while the "patched" flag stayed 0, so the next attempt hooked our own function onto itself (stack overflow).
- **Staged fallback with real verification:** `output = excl` → `shared` → ASIO. Each stage now *waits* for its thread to report success, so a failed `Start()` no longer looks like a working backend.
- **Exclusive self-test** (600 ms) checks both delivered frames and the **device clock** (`IAudioClock`); below 95 % of real time it tears the stream down and falls back.
- **Fake mode has a real fallback:** if ASIO dies while the client is faked, the mod starts the shared backend (`WoStart(0)`) instead of the old "game audio returns via WASAPI", which was impossible with no real stream.
- **No engine handoff in fake mode:** the tap-and-zero dance (resync + 23 ms wait) is skipped, so starting a song no longer discards ~10 ms of already mixed audio.
- **Ring memory is allocated unconditionally** and every ring access is NULL-guarded, so `fake_client = 1` + `asio = 0` (previously a guaranteed crash) now works and is auto-corrected to `output = shared`.
- **Arbitrary channel counts** on the output side (2.0 / 5.1 / 7.1 devices), FL/FR mapped, extra channels silenced.
- **Partial writes**: only as many frames as the ring actually holds are released, so a device whose engine buffer is larger than CRI's queue does not get zero-padded every period.
- **Bounded waits**: the tap-steady wait times out after 30 s; the exclusive rollback waits for its writer thread to actually exit (2 s) and leaks rather than frees if it doesn't (no use-after-free).
- **Realtime-safe callbacks**: no file I/O and no allocation inside the ASIO/WASAPI callbacks; counters are reported by a monitor thread.
- **Session mute cleanup** now walks the process's own audio sessions (`IAudioSessionManager2`), so it also works when stream1 is faked — it used to fail with `AUDCLNT_E_NOT_INITIALIZED` and silently do nothing.
- **Logs**: one atomic write per line, append mode with rotation (`audio_hook.log.1`), so the run that crashed is still readable.

---

## Measured buffer sizes

Objective numbers from the test machine (Focusrite Scarlett Solo 3rd Gen, 44.1 kHz):

| Component | Value |
|---|---|
| CRI staging buffer we hand the game | 441 frames = **10.0 ms** (`FAKE_FRAMES`) |
| ASIO block size | 64 frames = **1.45 ms** |
| ASIO driver-reported latency | 181 frames in / out = **4.1 ms** |
| WASAPI shared engine buffer | 970 frames = **22.0 ms** @44.1 kHz (1056 @48 kHz) |
| WASAPI exclusive period (Focusrite) | 221 frames = **5.01 ms** |

Steady-state validation of the `asio` and `shared` backends: **44,100 frames/s delivered, 0 underruns, 0 dropped frames** over 25 s runs.

> Exclusive mode is listed for completeness only — see [Known limitations](#known-limitations). It is not a recommended path.

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

4. Launch the game and check `mods/Audio Hook/audio_hook.log` — the header must read `v13.2` and `MODE: v13 takeover`.

---

## Configuration

`mods/Audio Hook/audio_hook.txt`:

```toml
fake_client = 1
output      = asio
asio        = 1
asio_driver = *          # or an exact name: Focusrite USB ASIO
block       = 64
handoff_ms  = 0
mute_mode   = 1
```

| Key | Default | Meaning |
|-----|---------|---------|
| `fake_client` | `1` | Replace CRI's stream1 client with ours. **Required** for any non-ASIO backend; without it you get the old v11 behaviour. |
| `output` | `asio` | `asio` / `shared` / `excl`. |
| `asio` | `1` | Enable the ASIO worker when `output = asio`. |
| `asio_driver` | `*` | Substring match against driver names listed by `asio_list`. `"*"` = first driver. Prefer an exact name when several drivers exist. |
| `block` | `64` | ASIO block size (16…1024). |
| `handoff_ms` | `0` | Handoff delay; `0` = auto. |
| `mute_mode` | `1` | `1` = tap-and-zero handoff; `0` = legacy `SetMute`. |

---

## Output backends

### `asio` (recommended)

Loads the ASIO driver on an **STA** worker thread, taps CRI's PCM directly from our client, and plays it out with a lock-free ring in between. The ASIO callback also paces CRI (`FakePoke`), so the two never drift.

### `shared` (no ASIO driver required)

Opens a normal WASAPI shared stream on the default endpoint (mix format, float32 or 16-bit) and writes the frames the ring currently holds. This keeps the engine's ~22 ms buffer in the path — that buffer is a platform floor, not something a client can shrink — but it removes CRI's own buffer and works everywhere.

If the endpoint is **not 44.1 kHz** (48 kHz HDMI / Bluetooth / onboard is common), the backend resamples 44.1 kHz → device rate with linear interpolation (≈1 frame of latency, inaudible at these ratios) instead of playing the whole game 8.8 % fast. The log line says `resample=yes`.

### `excl`

Opens `AUDCLNT_SHAREMODE_EXCLUSIVE` with `duration == periodicity` (5 ms) and event-driven callbacks. Because exclusive mode bypasses the engine, the device must be free — and since stream1 no longer opens a real stream, it is.

The catch is driver quality. On this machine:

- Focusrite USB — initializes with 221 frames, but the **device clock advances at 88.5 %** of nominal;
- Realtek onboard — initializes with 480 frames at 48 kHz, but the **device clock stays at ~0.3 %**.

Both would starve the queue and make the music stutter, so v13 measures 600 ms at startup and rolls back to `shared` when the rate is under 95 %. Use `tools/excl_rate_probe.exe` to check your own device:

```
excl_rate_probe --list
excl_rate_probe Realtek --seconds 2
```

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
4. Because the signal comes from the real output clock, production and consumption are locked together — no resampling, no drift.
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
- **ASIO interface:** `fake_client = 1`, `output = asio`, and set `asio_driver` to the name reported by `asio_list`. The interface must run at 44.1 kHz; otherwise the bridge refuses and falls back.
- **Exclusive experiment:** `output = excl` — run `excl_rate_probe` first; if the device does not report ≈100 %, the mod will fall back anyway.
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
asio_list --test "Focusrite USB"   play the tone on that driver directly
asio_list --test "*"               play on the first registered driver
asio_list --help
```

Exit codes: `0` played · `1` no drivers registered · `2` nothing matched / playback
failed · `3` bad command line · `4` the driver crashed.

The interactive menu lists every driver registered under
`HKLM\SOFTWARE\ASIO`; **Up/Down** moves the selection, **Enter** plays a 3 s 440 Hz
tone on it, any key stops, **Q** quits.

```
=== ASIO Driver Test  (9 drivers) ===
  Up/Down: select   Enter: play 3 s tone   Q: quit

  [ 1] Ableton Move
  [ 2] Ableton Push
  [ 3] Focusrite Thunderbolt ASIO
  [ 4] Focusrite USB ASIO          <- highlighted = selected
  [ 5] GT-1000
  ...
```

The tone is deliberately gentle: `0.12` amplitude with a 120 ms fade-in, so it will not
blast your monitors. **Whichever speaker/headphone the tone comes out of is the physical
output that driver drives** — that is the whole point of the tool.

Use the printed name in `asio_driver`. Matching is a **case-insensitive substring**
search that takes the **first** hit, so a short string can select the wrong driver (on a
machine with both `Focusrite Thunderbolt ASIO` and `Focusrite USB ASIO`, writing
`Focusrite` picks Thunderbolt). Always copy the full name.

A name that matches **nothing is an error** (`exit 2`, the registered names are printed):
it is never silently redirected to the first driver, which used to let you "verify" the
wrong device.

Drivers that crash on load (some Ableton builds do) are caught with SEH and reported as
`driver crashed` — one bad driver cannot take down the tool, and each test now closes its
stream (`stop` → `disposeBuffers` → `Release`) so the next driver really gets the device.

### `excl_rate_probe` — is exclusive mode real on this device?

```
excl_rate_probe --list                       list render endpoints, default marked
excl_rate_probe "<device substring>" [--seconds N] [--shared]
```

Defaults: the **default render endpoint**, **2 s per format** (`--seconds` accepts 1…60;
a bad value or an unknown option is a usage error, not silently ignored). It tries every
plausible format (44.1 / 48 kHz × 16 / 24 / 32-bit float), re-activates the client for
each one, and only trusts a format if the **device clock really advances** —
`Initialize()` returning `S_OK` proves nothing.

Exit codes: `0` measured ≥ 95 % of real time · `3` measured below that (or nothing
worked) · `1` device/COM error · `2` bad command line.

```
-- 44100 Hz / 16 bit / 2 ch --
   initialized: buffer 441 fr (10.00 ms) - measuring...
   -> written 43773 fr/s, device clock 43950 fr/s

== SUMMARY 'Focusrite USB Audio'
   best: 44100 Hz / 16 bit / 2 ch  buffer 441 fr  ->  99.7% of real time
```

| Line | Meaning |
|---|---|
| `written … fr/s` | frames the tool actually pushed into the buffer |
| `device clock … fr/s` | `IAudioClock` advance, converted to frames — **the honest number** |
| `(352800 units/s)` | the clock's own tick rate; `GetFrequency()` is used to normalise it |
| `not supported (0x88890008)` | the driver refuses that format in exclusive mode |
| `Initialize 0x88890019` | buffer size not aligned — that candidate is skipped |

> Device clocks do **not** count in samples at the nominal rate: a shared-mode stream on
> the test machine reports **352800 units/s** (8× 44.1 kHz). The tool divides by
> `GetFrequency()` before turning the number into a percentage, so 100 % really means
> real time. If a driver has no usable `IAudioClock`, the written rate is used instead and
> the line says so.

How to read the summary:

- **≥ 95 %** — exclusive is genuinely usable; `output = excl` will keep it.
- **90–95 %** — marginal: the queue slowly drains or fills, expect drift/stutter.
- **< 50 %** — the driver accepted the stream but never starts the hardware clock;
  exclusive cannot be used on that device.

That last case is exactly why the mod runs a **600 ms self-test** on `excl` and rolls
back to `shared` below 95 %. `--shared` runs the same measurement on a normal shared
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
written next to the exe (`session_tool_report.txt`), and device names are printed as
UTF-8, so Chinese/Japanese endpoint names are readable.

```
== device '扬声器 (4- Focusrite USB Audio)' [{...}]
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

- **WASAPI exclusive is driver-dependent.** Measured on the test machine: Focusrite USB **88.5 %**, Realtek onboard **0.3 %** of real time. The backend therefore self-tests and falls back; it is not the default and not a recommended path.
- **The exclusive self-test runs on the game's init thread**, so `output = excl` adds ~600 ms to the first audio initialisation (and claims the device before stream2 exists). Acceptable for an experiment; not used by default.
- **Movie audio (stream2)** stays on a real WASAPI stream in `asio`/`shared` mode. In `excl` mode the device is taken exclusively, so stream2 fails to initialize (`AUDCLNT_E_DEVICE_IN_USE`) and cutscene audio is silent — this is expected and harmless for gameplay.
- **Shared mode cannot go below the engine buffer** (~22 ms on most drivers). That is a platform floor, independent of this mod.
- Only the default render endpoint is used by `shared`/`excl`.
- `exclusive2` from older configs is **ignored** (it was removed in v13 and cannot work on any tested device); the mod logs a line when it sees the key.

---

## 中文说明

把游戏（CRI ADX2）的 **stream1 音频客户端整个换掉**：CRI 以为在往 WASAPI 写，实际写进本模组的内存。于是游戏自带的 **22 ms 引擎缓冲彻底不在链路里**，设备也完全空出来给自选输出后端。

- **三个输出后端**（`audio_hook.txt` 的 `output`）：
  - `asio`（默认，推荐）：原生 ASIO 驱动，block 64 = 1.45 ms，驱动报告 4.1 ms。
  - `shared`：自己开一条 WASAPI 共享流，**不需要 ASIO 驱动**，任何设备可用；引擎 22 ms 缓冲是平台地板。
  - `excl`：WASAPI 独占（5 ms 周期），带 **600 ms 启动自检**，设备跑不满 95% 就自动回退到 shared。
- **实测**（本机 Focusrite）：CRI 缓冲区 441 帧 = 10 ms；ASIO 64 帧 = 1.45 ms；共享引擎 970 帧 = 22 ms。稳态 44,100 帧/秒、0 欠载。
- **独占实测结论**：Focusrite 设备时钟只有 88.5%、板载 Realtek 只有 0.3% —— 驱动接受独占流但不真正跑，所以**独占不适合当主推**，本模组会自动降级。
- **安装**：`Audio Hook` 文件夹放进 `mods/`，根 `config.toml` 的 `priority` 加 `"Audio Hook"`。
- **急救**：同目录放空文件 `audio_hook.off`；或 `fake_client = 0` + `asio = 0` 回到旧的纯 WASAPI 优化。
- **配套工具**（`tools/`，也可从 Release 下载）：
  - `asio_list` —— 列出本机所有 ASIO 驱动，↑↓ 选择、回车播放 3 秒测试音，**哪个声卡出声就是它**；`--list` 纯列表、`--test "名字"` 直接试听。填进 `asio_driver` 的就是它显示的名字（要用完整名，子串匹配取第一个）。
  - `excl_rate_probe` —— 测某设备的独占/共享流**是否真的实时**：`excl_rate_probe --list`、`excl_rate_probe Realtek --seconds 2`、`excl_rate_probe default --shared`。看 `device clock` 的百分比：≥95% 可用、90~95% 勉强、<50% 驱动根本没启动硬件。
  - `session_tool` —— 会话诊断：不带参数列出各设备所有音频会话（状态/静音/音量/峰值）；`--unmute DivaMegaMix` 清除旧版遗留的持久静音（需游戏运行中执行）。
- **更新日志**：见 [CHANGELOG.md](CHANGELOG.md)。

---

## License

[MIT](LICENSE) © 2026 vcvcOTZ
