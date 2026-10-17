# Audio Hook

> Low-latency audio mod for **Hatsune Miku: Project DIVA Mega Mix+** (Steam).

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Game](https://img.shields.io/badge/game-PDMM%2B-7b5cff)
![Version](https://img.shields.io/badge/version-v11.3-informational)
![License](https://img.shields.io/badge/license-MIT-blue)

An audio mod that shrinks the game's WASAPI buffers and can route the game audio through a native **ASIO** driver — without touching a single byte of the game's code. It patches only COM **vtable** slots at the data layer, so it is safe against the game's anti-tamper (Denuvo).

---

## Table of Contents

- [Why this exists](#why-this-exists)
- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Configuration](#configuration)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Portability](#portability)
- [Building from source](#building-from-source)
- [Known limitations](#known-limitations)
- [中文说明](#中文说明)
- [License](#license)

---

## Why this exists

PDMM+ ships with a **CRI ADX2** audio stack that runs two WASAPI shared-mode streams with unnecessarily large buffers:

| Stream | Content | Format | Engine-granted buffer |
|--------|---------|--------|----------------------|
| stream1 | game audio | 16-bit / 44.1 kHz | **44.5 ms** (requests 10 ms) |
| stream2 | movie audio | 32-bit float / 44.1 kHz | **66.7 ms** |

The 44.5 ms stream1 buffer is caused by the `AUDCLNT_STREAMFLAGS_RATEADJUST` flag.

This mod:

1. **Always on** — trims both WASAPI buffers to the platform's 22 ms floor; and
2. **Optionally** — routes the game audio straight through an **ASIO** driver for a single-digit-millisecond output path.

---

## Features

### 1. WASAPI shared-mode buffer optimization *(always active)*

- **stream1 (game audio):** removes `RATEADJUST` → buffer drops **44.5 ms → 22 ms**.
- **stream2 (movie audio):** duration **66.7 ms → 22 ms**.

This is the base behaviour and requires **no configuration** — it is active even when the ASIO bridge is disabled.

### 2. ASIO output bridge *(optional, `asio = 1`)*

- Taps the PCM that CRI writes into stream1, de-interleaves it correctly, and plays it out through a native ASIO driver.
- Verified on a **Focusrite Scarlett Solo 3rd Gen** at **block size 64 = 4.1 ms**.
- Lock-free single-producer/single-consumer ring buffer (`65536` frames) — the audio callback is allocation-free.
- **`tap-and-zero` handoff:** copies then zeroes the game buffer so the WASAPI path goes silent naturally, then starts ASIO at the exact frame the game stopped on. No `SetMute`, no double audio, seamless transition.
- **Self-healing:** if the stream is invalidated at runtime, the hook rolls back automatically and the game audio returns via WASAPI within ~1 s.
- **Bounded startup:** the tap-steady wait times out after **30 s** instead of spinning forever — if the game audio can't reach real-time, the bridge gives up and stays on WASAPI (v11.3).
- **Realtime-safe callback:** the ASIO callback does **no file I/O and no allocation** — underruns are counted atomically and reported by the monitor thread once a second (v11.3).
- **Sample-rate guard:** refuses to bridge on any device that is not 44.1 kHz, so it can never pitch-shift your audio.
- **Stale-mute cleanup:** clears the Windows per-app session-mute residue left by older builds on every boot.

### 3. Anti-tamper safe

Patches only COM **vtable** slots (data layer):

- `IAudioClient` `[3]` `Initialize`, `[14]` `GetService`
- `IAudioRenderClient` `[3]` `GetBuffer`, `[4]` `ReleaseBuffer`

No code bytes are written, no COM objects are wrapped, and no return values are forged. The executable is left byte-for-byte untouched.

---

## Requirements

- **Hatsune Miku: Project DIVA Mega Mix+** (Steam).
- **DML (Diva Mod Loader)** — the standard PDMM+ mod loader (`dinput8.dll` + `mods/<name>/config.toml`).
- *(Optional, for the ASIO bridge)* an **ASIO driver** for your audio interface, running at **44.1 kHz**.

---

## Installation

1. Make sure the DML mod loader is installed (`dinput8.dll` in the game root).
2. Copy the `Audio Hook` folder into `mods/`:

   ```
   mods/Audio Hook/
   ├── config.toml
   ├── audio_hook.dll
   └── audio_hook.txt
   ```

3. Ensure the root `config.toml` loads this mod — add `"Audio Hook"` to `priority`:

   ```toml
   priority = ["Audio Hook", "Disable Watermarks", "High Frame Rate", "ExPatch"]
   ```

4. Launch the game. Verify via `mods/Audio Hook/audio_hook.log` — the header should read `v11.3`.

---

## Configuration

Edit `mods/Audio Hook/audio_hook.txt`:

```toml
asio        = 1              # 1 = ASIO bridge; 0 = WASAPI optimization only
asio_driver = *  # ASIO driver name substring; "*" = first driver
mute_mode   = 1              # 1 = tap-and-zero (recommended); 0 = legacy SetMute
block       = 64             # ASIO block size 16..1024 (64 = 4.1 ms)
handoff_ms  = 0              # 0 = auto (measured buffer + 2 ms); >0 = override
```

| Key | Default | Meaning |
|-----|---------|---------|
| `asio` | `1` | Enable the ASIO bridge. `0` = pure WASAPI shared-mode optimization (the v9 behaviour). |
| `asio_driver` | `*` | Substring match against `HKLM\SOFTWARE\ASIO` driver names. `"*"` picks the first available driver; set a specific name (e.g. `Focusrite USB`) if you have several. |
| `mute_mode` | `1` | `1` = tap-and-zero seamless handoff; `0` = legacy `SetMute` (kept for A/B testing). |
| `block` | `64` | ASIO buffer size. Lower = lower latency, higher = more headroom. |
| `handoff_ms` | `0` | Handoff delay. `0` auto-computes from the measured game buffer. |

---

## How it works

```
CRI ADX2 (game)
   │  IAudioRenderClient::ReleaseBuffer
   ▼
[tap] ── copy PCM ──▶ [lock-free ring (65536 fr)] ──▶ [de-interleave] ──▶ ASIO driver ──▶ interface
   │
   └─ zero (tap-and-zero mode)  →  WASAPI engine plays silence → seamless handoff
```

1. `Initialize` is intercepted to rewrite the two streams' parameters (the always-on WASAPI optimization).
2. When the ASIO bridge is enabled, `GetService` arms the render-client tap once stream1 exists.
3. `ReleaseBuffer` copies the freshly written PCM into the ring and (in tap-and-zero mode) zeroes the game buffer.
4. A dedicated STA worker loads the ASIO driver, waits for a steady real-time tap, prepares the engine, and starts output at the exact handoff frame.
5. A watchdog monitors callbacks/underruns and rolls back to WASAPI on any fault.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No sound at all after disabling the mod | Run `modding-docs/dev/session_tool.exe --unmute DivaMegaMix` (older builds left a persistent session mute). v11.2+ clears it automatically. |
| Audio crackle only during the first ~3 s | Normal — the game's loading hitch. Raise `handoff_ms` to `40-60` or lower `block` to `32`. |
| Game exits silently a few seconds after launch | **Game's own bug** (foreground-focus check fails → `WinMain` returns early), unrelated to this mod. Keep the game window focused during startup, or install the community `MegaMixPlus-BackgroundLaunchFix` (`d3d11.dll` proxy). |
| Want to disable everything | Drop an empty file `audio_hook.off` next to `audio_hook.dll`. |
| Want to disable only ASIO | Set `asio = 0` and restart. |

Logs: `audio_hook.log` (main), `audio_hook_detach.log` (process-detach diagnostics).

---

## Portability

To use on a different machine / interface:

1. Install the target device's **ASIO driver**.
2. Set `asio_driver` to that driver's name substring (or `"*"`).
3. The device **must be 44.1 kHz** — otherwise the bridge refuses to start and stays on the (always-on) WASAPI optimization, so sound is never broken.
4. If WASAPI and ASIO cannot coexist on that device, the self-healing rollback keeps the game audible (just without the low-latency direct path).

---

## Building from source

- Compiler: **MSVC** (Visual Studio Build Tools) + Windows SDK.
- ASIO SDK headers are bundled under `src/asiosdk/`.
- Source: `src/audio_hook.cpp`.
- One-shot build: run `build.bat` from the `src/` directory (locates the toolchain via `vswhere`).

```
cl /nologo /utf-8 /LD /MT /O2 /W3 /I asiosdk /Fe:audio_hook.dll audio_hook.cpp /link /MACHINE:X64 user32.lib
```

> Copy the built `audio_hook.dll` to `mods/Audio Hook/`.

---

## Known limitations

- Only the **game audio stream (stream1)** is bridged to ASIO; the movie audio stream stays on WASAPI (already optimized to 22 ms).
- Exclusive-mode output was **permanently rejected** — on the test hardware it delivered only ~86–98 % real-time regardless of buffer size (structural limit: `RATEADJUST` forces `duration == periodicity`, leaving nowhere to buffer the shortfall). The code was fully removed in v11.2.

---

## 中文说明

一个音频模组：缩小游戏 WASAPI 缓冲、并可把游戏音频改走 ASIO 驱动直通，**不改游戏代码一个字节**，只改 COM 虚表（Denuvo 反篡改安全）。

- **两层能力：**
  1. **WASAPI 共享缓冲优化（始终生效，无需配置）**：去掉 `RATEADJUST` 标志，stream1 缓冲 44.5 ms→22 ms；stream2 66.7 ms→22 ms。
  2. **ASIO 直通（可选，`asio = 1`）**：截取 stream1 PCM → 无锁环形缓冲 → 解交错 → ASIO 驱动输出（Focusrite Solo 3，block 64 = 4.1 ms）。
- **安装**：DML 加载器下，把 `Audio Hook` 文件夹放进 `mods/`，根 `config.toml` 的 `priority` 加 `"Audio Hook"`。
- **配置**：改 `audio_hook.txt`（`asio` / `asio_driver` / `mute_mode` / `block` / `handoff_ms`）。
- **急救**：同目录放空文件 `audio_hook.off` 停用一切；`asio = 0` 只保留 WASAPI 优化。
- **关闭 mod 后无声**：用 `session_tool.exe --unmute DivaMegaMix`（v11.2 起自动清理）。

---

## License

[MIT](LICENSE) © 2026 vcvcOTZ
