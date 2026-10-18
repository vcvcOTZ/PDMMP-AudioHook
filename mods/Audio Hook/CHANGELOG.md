# Changelog

All notable changes to **Audio Hook** for *Hatsune Miku: Project DIVA Mega Mix+*.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are the
DLL build tags. The mod manifest version in `config.toml` (`0.3.10`) is tracked separately
and only marks packaging changes.

---

## [13.10] — 2026-10-18

A full-file review pass, plus the first field use of a new ASIO instrument. The `asio` path
is confirmed clean over two minutes (`und=0`, zero starvation, the device within ±22 ppm of
the wall clock); `excl` is confirmed to be 3.7–3.9 % starved **in steady state**, not just
during its self-test. Both items the 13.9 entry had left deferred are fixed. `asio` is still
the recommended output.

### Added — driver-side drift instrumentation (`asio`)

`cbBufferSwitchTimeInfo` ignored `timeInfo.samplePosition` outright. It is the driver's own
frame counter - the only device-side truth on this path - and it separates the two failure
modes the ring counters cannot see:

- `posDelta − delivered` = frames the **device** played while we handed it nothing. A real
  audible gap; `RingAvail()` cannot tell it apart from a legitimately empty ring, which is
  why it was never caught before.
- `posTotal / wall time` = the device's true rate, i.e. its crystal against the wall clock -
  the part that shifts the whole timeline (music and chart together) against real time even
  when nothing is ever starved.

The realtime callback only accumulates (integers, no I/O, no logging; the time origin is
stamped there with `QueryPerformanceCounter`, which is lock-free and RT-safe). The monitor
thread prints `[asio] drift: dev=… game=… gap=…` as cumulative averages (~2 s in, then with
the per-minute status line). `dev − game == gap` holds exactly, so the line is
self-checking. `ASIOSamples` is the `{hi,lo}` struct in this build (`asiosys.h` sets
`NATIVE_INT64` to 0) and is reassembled by hand - a plain cast reads only `hi`. Drivers that
report no `samplePosition` are detected once and said so in the log.

The first cut of the instrument was wrong in a way the field run exposed: it timed the wall
clock from before `start()`, which added a fixed ~100-frame offset to `dev` (read −783 ppm
at 3 s and only faded as 1/t), and it read `g_fakePlayed` as-is, which still carried the
frames the pre-ASIO drain had discarded (~13.5k, i.e. a bogus +104 818 ppm "game" rate).
Both counters now start at the first callback that carries a `samplePosition`, so the
reading is honest from the first second. A run whose `gap` climbs is starving the device, no
matter what the other counters say.

### Changed — the `excl` self-test runs from `WatchdogThread`

It used to run inline in `WoStart()`, which `Hook_Initialize` calls on the game's init
thread: 200 ms of ramp plus 2 s of measurement blocked audio init, so the title screen came
up silent for 2.2 s on every launch with `output=excl`.

The watchdog now arms and judges it, overlapped with playback - nothing else has to know a
test is in progress, because the exclusive writer keeps the device fed on its own (see the
partial-period fix below). On failure the teardown happens on the same thread that rebuilds
`shared` on the next tick, so the backend is never reconfigured from two threads at once
(the trap an independent self-test thread walks into: `WoInit` releases `g_woClock` from
another thread and the reader is left with a stale pointer). The window is measured against
real elapsed time - the watchdog wakes once a second - instead of the old fixed 2 s, and the
new `[wo] exclusive self-test armed (runs during playback - game init is not blocked)` line
marks the change. `g_woSelftest` is gone: forcing the silence fill was its only job, and
exclusive now always receives a complete period.

### Fixed — `excl` could be handed a *partial* period

`n = min(free_, avail)`, with a full period forced only when `n` happened to be exactly 0. A
ring that was merely *short* (a CRI partial fill around a song change) left the rest of the
period holding frames the device had already played and let the queue drain towards the one
state the single-period buffer cannot survive - the failure this backend already documents
as "silent on the second launch / alt+F4 hangs". Exclusive now always requests `free_`;
`WoFill16`/`WoFillFloat` already zero the tail, so the shortfall is silence rather than
stale audio. This also covers the pre-mix startup window, where `n` used to be 0 and nothing
at all reached the device.

### Fixed — a fast `Fake_Stop()` → `Fake_Start()` could leave two drain threads

`Fake_Start` set `g_fake.running` straight back to 1 while the previous thread was still
asleep in up to 20 ms of `Sleep()`; it then woke, saw 1 and kept looping, and two threads
consumed the same ring at once (chopped, half-speed audio). The thread handle is kept now:
the old thread is joined, or simply handed the flag back when it is still inside its loop,
with a re-check for the "left between the timeout and the flag flip" window so that "reuse"
cannot come to mean "no driver thread at all". This closes the second item v13.9 left
deferred.

### Fixed — the idle drain paced the game clock ~9 % slow

With no output backend up, `FakeDriverThread` drained a fixed 220-frame chunk per `Sleep(5)`.
`RingRead()` advances the fake clock that CRI reads as "played" and the game syncs its chart
to, so the whole timeline - music and chart alike - dragged behind real time for as long as
no backend was up, and for the rest of the session whenever every backend had failed. The
amount drained is now what the elapsed wall clock owes (44 100 fr/s, clamped to the staging
buffer), so the drain rate, the clock and real time agree.

### Fixed — a concurrent ring discard could be undone

`RingRead` published `g_rr` with a plain store, while `RingResyncToLatest` - CRI's thread,
via `Fake_Stop` / `Fake_Reset` - could move it forward mid-copy. The store then put the
cursor *back* and re-handed frames the discard had just thrown away (a burst of the previous
song). The publish is a compare-exchange now, so a discard always wins. This closes the
first item v13.9 left deferred.

### Fixed — smaller ones

- The legacy tap left the slot's `ptr` / `frames` populated after use, so a release with no
  matching `GetBuffer()` (a seek / restart, or a failed `GetBuffer`) could re-inject the
  previous block into the ring - an audible repeat. The slot is emptied again, length first,
  pointer last.
- `RsFlush()` ran on CRI's thread while `RsPull()` was mid-sample on `WoThread`, sharing
  `g_rsCur` / `g_rsNext` / `g_rsBuf` / `g_rsI` / `g_rsN`; a torn state emits a click. CRI now
  only requests the flush, and `WoThread` - the single owner - applies it at the top of its
  loop. One loop iteration of delay is invisible; the point is only that the previous song's
  tail is gone before the next one starts.
- The render-client failure logs were unbounded, and `Log()` `fflush`es every line: a device
  that is gone made every call fail, so the same line was written thousands of times a
  second on CRI's mixing thread. Front 8 failures, then every 500th.
- `excl_period_ms` above the device default is known to stop event delivery, but the config
  still accepted it and the code advertised it as the fix for in-game load. `WoInit` now
  says so loudly instead.

### Verified — first field run of the instruments

`asio`, two minutes (block 64): **`gap=0 fr` at every sample and `und=0` end to end** - the
device never played without data, i.e. zero starvation, and no rollback. `dev` settled at
**−22 ppm** against the wall clock (≈1.3 ms/min), so on this device the timeline does not
drift against real time. (The callback counter alone could not have shown this: 689 ±1
callbacks/s quantises to ±1450 ppm.)

`excl`, four measurements across two launches: self-test 96.0 % and 96.3 % `OK`, and the
independent steady-state tempo monitor read `fake-clock=42 460 fr/s (96.28 %)`. So the
sub-100 % self-test reading is **not** the self-test-phase transient v13.9 assumed - the
exclusive writer genuinely services only ~96 of the 100 events per second, i.e. ~37 ms of
silence is injected per second. With the gate at 90 % an `excl` session passes while playing
with that dropout; tightening the gate (or retiring `excl`) is left as a decision, since
`asio` is the recommended path.

Audio init is no longer blocked: `[wo] ready: EXCLUSIVE` now arrives **83 ms** after the
game's `Initialize` call, and the game's own log lines (stream2's `Initialize`, the fake
render client, the heartbeat) continue straight through the window that used to stall them.

### Known limitation — `excl` holds the endpoint, so `stream2` fails

An exclusive claim excludes every other stream on the endpoint, so `stream2` (movie audio)
gets `AUDCLNT_E_DEVICE_IN_USE` whenever `excl` is running. That was already true whenever
the self-test passed (16:53, and both of the runs above), but the async gate makes it
unconditional: a *failing* self-test used to have released the device before `stream2` was
created (17:10 / 17:36 / 18:28), and now `stream2` always initializes against a claimed
device. Only the "dead device → roll back to `shared`" path loses its movie audio; `shared`
and `asio` are unaffected.

### Docs

- README / README.zh-CN: version badge, boot-banner string, the `excl` self-test bullet (it
  no longer runs on the game's init thread) and the ASIO-rollback bullet (the ring-discard
  race is closed).

### Deferred (documented, not changed)

- `FakeRC::GetBuffer` hands out one shared staging buffer and keeps no record of the
  outstanding length; the observed CRI pattern never nests `GetBuffer` calls.
- `NotifyLogger` is never unregistered and its extra reference is never released - COM calls
  are not safe from `DllMain`, so it stays a deliberate process-lifetime leak.
- `RingResyncToLatest(keep > 0)` has no caller, and a non-zero `keep` would move `g_rr`
  backwards - a latent trap rather than a bug.

### Build

`/W4`: zero warnings (DLL).

---

## [13.9] — 2026-10-18

Makes the recommended `asio` backend survive song / stream transitions, and settles the
`excl` line for good. `shared` and `excl` stay available; the default output is still
`asio`.

### Fixed — `asio` rolled back to `shared` at every stream transition

`AsioFill` counted an empty ring as an underrun whenever the fake driver was running. The
game pauses its producer for a moment around a stream transition, and discards the ring
via `Fake_Stop` / `Fake_Reset`, so the ring was legitimately empty for a second or two -
the counter flooded past the monitor's `runaway underruns` bar (2 consecutive seconds with
> 100 underruns/s) and dropped ASIO mid-song. Field data (12:32 / 14:19 / 14:31) showed
the ASIO callback itself healthy (`und=0` for 60 s) right up to the transition:

- The count is now suppressed for `ASIO_UND_GRACE_MS` (3 s) after any fake-driver
  start / stop / reset.
- …and while the producer has written nothing for `ASIO_PRODUCER_IDLE_MS` (1 s): an **idle
  producer is not a fault** - only an empty ring with a *busy* producer is a real underrun.
  A real ASIO stall lasts far longer than either guard and is still caught (stalled
  callbacks and ring drops are checked separately).
- `Fake_Reset` stamps the grace too, because a seek / restart discards the ring without a
  preceding `Stop` - the one gap the v13.8 Start/Stop stamps left open.
- Verified: a 6 m 43 s `asio` run (block 64, 4.1 ms) held `und=0` end to end, including the
  `[fake] Stop()` at the end.

### Added

- `excl_period_ms` (config): the exclusive backend's event period in ms, `0` = the device
  default (10 ms). See the note below - only `0` is usable in practice.

### Changed — the `excl` self-test is 2 s again, with a 90 % gate

v13.8's 5 s window bought nothing (the reading does not converge with window length) and
delayed the first audio by 3 s, so the title screen came up silent for that whole stretch.
The window is **2 s** again, and the rollback bar is **90 %** (was 95 %): on a healthy
device the reading is a self-test-phase transient that lands in a 93.5–98.5 % band, and
every session that cleared the old bar then ran stably, while the only genuinely broken
sessions read 0 %.

### Documented — `excl` is capped at the device's default period

Setting `excl_period_ms` above the device default makes the device stop firing events and
the stream is invalidated, so it falls back to `shared`. 10 ms is the only value that
works, and that leaves the exclusive buffer with no slack - the backend keeps a small
residual risk and stays non-recommended. `asio` is the recommended path.

### Fixed — `shared` under-used the ring on endpoints above 44.1 kHz

`WoThread` converted the ring's capacity from source frames (44.1 kHz) to output frames
(device rate) only when `g_rsStep > 1.0`. A 48 / 96 kHz endpoint has `step < 1`, so it used
just `1/step` (94 % at 48 kHz) of what the ring could supply every period and padded the
rest with silence. The conversion now runs in both directions.

### Fixed — resampler extrapolation on starvation

`RsPull` could leave `g_rsFrac >= 1.0` when `RsNext` came back empty, so the next call
extrapolated past `g_rsNext` (an audible click) instead of interpolating. The fraction is
normalised before the starvation break.

### Docs

- README / README.zh-CN: version badge, boot-banner string, the `excl` section (period
  cap, 2 s self-test, 90 % gate) and the `excl_period_ms` config key. `config.toml` →
  0.3.9.

### Build

`/W3` and `/W4`: zero warnings (DLL and all three tools).

### Deferred (documented, not changed)

- The ring discard (`RingResyncToLatest`) still writes `g_rr` from the game's stream
  thread while a backend may be reading; it is only ever reached from the game's own
  serialised stream calls, so it is left as-is.
- `FakeInit` failure still clears the global fake flags, and the fake driver's idle drain
  can overlap a just-started backend for one tick.

---

## [13.8] — 2026-10-18

### Fixed — clean exit (one alt+F4, no burst of noise on quit)

On exit the game stops stream1, which empties the ring while the ASIO callback kept
running. The callback treated that empty ring as an underrun — flooding the counter
(500+ per second) until the monitor triggered `runaway underruns` and rolled ASIO back to
a shared stream the game no longer needed, stalling the exit (it took two alt+F4 to quit).

- The ASIO callback no longer counts an empty ring as an underrun while the fake driver is
  stopped (`g_fake.running`); the legacy tap path keeps counting as before.
- `DllMain` now stops the writer and fake-driver threads on `DLL_PROCESS_DETACH` (FreeLibrary
  case only; on process exit the OS is already tearing them down).
- `struct FakeState` moved up so the ASIO realtime callback can read `g_fake.running`.
- `audio_hook.txt` ships with `output = asio` again (v13.7 accidentally packed `output = excl`).

---

## [13.7] — 2026-10-18

Fixes the exclusive self-test (a 0 % false-negative, then a wrong "slow device"
conclusion) and settles exclusive back to a correct but non-recommended backend.

### Fixed — the self-test read 0 % because the exclusive stream never started

v13.6's ramp-up-skipping self-test still failed with `written=0 clock=0 → 0.0 %`:

1. **Exclusive + EVENTCALLBACK never signals the event unless the stream is pre-filled
   before `Start()`.** With nothing pre-filled the device has no data, never signals, so
   the writer thread never ran at all.
2. Even after the engine starts, the game does not begin mixing until ~2.5 s after the
   client is installed, so the ring was empty during the 2 s measurement window.

Fix: pre-fill one buffer with silence in `WoInit` before `Start()`, and keep the writer
filling **silence** while the self-test runs (`g_woSelftest`).

### Corrected — the "88.5 % / 99.5 %" numbers were misread

The v13.6 note that a healthy 16-bit device delivers **99.5 %** and that the 88.5 % was
only a measurement artifact was wrong. The self-test now reports the *real* delivery rate,
and inside the game exclusive consistently lands at **~94.5 %** (a 5 ms period: ~88 %)
because its software event loop is sensitive to system and background load — a couple of
CPU-heavy background apps (e.g. an Android emulator) drag even a clean `excl_rate_probe`
run from 99.5 % down to ~96 %. That is not a driver fault, but it is exactly why exclusive
is not a recommended path.

### Changed

- Use the device's **default period** (10 ms) instead of a hard-coded 5 ms, and check
  `GetDevicePeriod`'s return value (10 ms fallback).
- Remove the failed exclusive experiments (`THREAD_PRIORITY_TIME_CRITICAL` writer, 2 ms
  polling); the default output stays `asio`.
- `audio_hook.txt` now documents `excl` as experimental / load-sensitive.

- Boot banner reads `v13.7`.

---

## [13.6] — 2026-10-18

Corrects a long-standing wrong conclusion about WASAPI exclusive mode.

### Fixed — exclusive self-test misread the clock ramp-up as a slow device

The `output = excl` self-test measured the device clock over a **600 ms** window taken
right after `Start()`, when the clock was still ramping up (~70 ms). `(600 − 70) / 600 ≈
88.3 %`, so a healthy **99.5 %** USB interface read as **88.5 %** and was rolled back on
every launch — which is where the "this driver cannot run exclusive" conclusion came from.

The self-test now skips the ramp-up (200 ms), re-reads the clock/frame counter, then
measures over a **2 s** window. A fixed `excl_rate_probe --seconds 10` run confirms the
truth: **16-bit = 99.5 %** (usable), **24-bit = 94 %** (genuinely slow — the device clock
really does lag there, it is not a measurement artifact).

### Docs

- README / README.zh-CN / `audio_hook.txt`: replaced the "88.5 % / cannot run exclusive"
  wording with the corrected 16-bit 99.5 % / 24-bit 94 % split, and the 600 ms self-test
  description with the 2 s ramp-up-skipping one. `config.toml` → 0.3.6.

---

## [13.5] — 2026-10-18

Robustness hardening pass: seven items from a follow-up review, all field-tested.

### Fixed / hardened

- **Allocation-failure checks.** `FakeRC` / `FakeClock` / the notification logger now use
  `new (std::nothrow)` and are NULL-checked instead of relying on an exception that would
  `terminate()` the process.
- **Back-pressure degrades when the timer is not raised.** `FakeRC::GetBuffer`'s 5×`Sleep(1)`
  becomes a single attempt when `timeBeginPeriod(1)` failed, so the mixing thread is never
  blocked for ~78 ms.
- **`WoThread` initialises COM.** The shared writer now calls
  `CoInitializeEx(NULL, COINIT_MULTITHREADED)` before its cross-apartment `IAudioClient`
  calls.
- **`GetService(IUnknown)` identity.** The fake client's `GetService` now returns the real
  client's `IUnknown` (matching a `QueryInterface`), not the render client's.
- **Boot mute cleanup is skipped in fake mode.** `Hook_Initialize` no longer walks the
  session list when there is no real stream1 session (the watchdog's 5 s deferred cleanup
  handles it).

### Docs

- The header comment now lists the fake client's `[4]..[12]` slots; "How it works" no longer
  claims "no resampling" (the `shared` backend resamples non-44.1 kHz endpoints).
- README now calls out the ASIO probe's `init()` side effects (pin `asio_driver` on a
  multi-driver machine), the one-way `excl` → `shared` fall-back, and the refusal of
  24-bit/32-bit integer shared mix formats.

### Build

`/W3` and `/W4`: zero warnings.

---

## [13.4] — 2026-10-18

Fourth-pass review fixes: two defects in `audio_hook.cpp` found in a fresh read-through
(the three companion tools were re-checked and are clean).

### Fixed

- **`FakeDriverThread` idle-drain buffer overflow.** The idle drain used a `static short
  tmp[512]` (256 stereo frames) but can drain `n = (a < FAKE_CHUNK) ? a : FAKE_CHUNK/2`
  with `RingAvail()` capped at `FAKE_FRAMES` by `GetBuffer`'s back-pressure. A 257..440-frame
  chunk (partial fill / song tail) wrote up to 880 shorts into a 512-short buffer, corrupting
  adjacent BSS. Sized to `FAKE_CHUNK * 2` (441 frames).
- **Detach report `%lld` misread.** `WriteDetachReport` formatted two 64-bit counters with
  `wsprintfA "%lld"`, but `wsprintfA` only understands `%l` and `%I64`, so `taps`/`zeroed`
  printed garbage and shifted every following argument. Switched to `%I64d`, which keeps the
  DllMain detach path free of CRT dependencies.

### Build

`/W3` and `/W4`: zero warnings.

---

## [13.3] — 2026-10-18

Generic defaults for distribution, plus the items `AUDIT-v13.1.md` left open.

### Changed — `asio_driver` defaults to auto, and auto really probes every driver

The shipped config used to name the author's own interface, which is wrong on every other
machine. It is now `*`, and `*` means "first **usable** driver": the old code stopped at
the first driver whose COM server answered, so a machine with several registered drivers
could end up on a disconnected interface, a virtual device, or one that cannot run at
44.1 kHz — and then sit in "prepare failed" while a perfectly good driver was never tried.
Each candidate now goes through init → channels → buffer size → sample rate →
createBuffers → channel format; a failure releases it and moves on. Capped at 8 attempts
(three dead drivers cost ~18 ms). The log names every attempt and the winner.

`asio_driver` also accepts a quoted value (`"*"`) and the keyword `auto`.

### Fixed — remaining `AUDIT-v13.1.md` findings

- **N6 (P1)** — `output = shared` never recovered from a dead endpoint: a failing
  `GetCurrentPadding` just looped on `Sleep(2)` forever, with no log line and the state
  stuck at "running". The usual cause is Windows switching the default device when
  headphones are plugged/unplugged. The writer now bails out and marks the backend failed;
  a watchdog rebuilds the shared stream (3 s apart for the first 5 tries, then every 30 s
  until it recovers) and logs every attempt.
- **N22** — `Int16MSB` / `Int24MSB` / `Int32MSB*` / `Float32MSB` / `Float64*` passed the
  "unsupported format" gate but had no writer, so the mod announced a healthy format and
  played silence. Preparation now checks an `IsTypeRenderable()` whitelist instead.
- **N2/N21** — the staging buffer was sized from the first block and never resized; a retry
  that got a larger block overflowed it.
- **N25** — `Stop()` / `Reset()` flushed the ring but not the resampler's own 441 frames,
  so a seek could replay a fragment of the previous song. (`RsReset()` would also have
  reset the step and broken non-44.1 kHz endpoints) → new `RsFlush()`.
- **N11** — `timeBeginPeriod(1)` is paired with `timeEndPeriod(1)` at detach and skipped
  when `audio_hook.off` disables the mod.
- **N12** — a truncated log line was dropped whole (`_snprintf_s` returns -1); it now
  writes what fits.
- **N9** — documented that the fake `GetCurrentPadding` counts source frames.
- **N26** — the misleading "allow a later retry" comments on `g_asioStartReq` are gone.
  There is deliberately **no active ASIO re-try**: once ASIO fails the mod stays on the
  fallback (shared / WASAPI) until the game is restarted; only a rebuilt stream1 client
  tries ASIO again. The comment now says that instead of promising a retry that never fires.

### Fixed — `AUDIT-v13.3.md` (third-pass review)

- **R1 (P1)** — ASIO retry and the shared fallback could run at the same time, both
  `RingRead`-ing the one ring (each got half the frames → torn, pitch-shifted audio). The
  trigger is real: ASIO fails → shared fallback starts → stream1 is rebuilt → ASIO is
  retried and succeeds without stopping shared. `AsioStartThread` now stops the shared
  stream (`WoShutdown`) right before `start()`, and `WoStart` refuses to start a WASAPI
  backend while ASIO is running.
- **R2 (P2)** — the shared backend treated every non-float mix format as int16. A shared
  mix format of 24-bit packed or 32-bit **integer** PCM would have been written 2 bytes per
  sample → full-scale noise. `WoInit` now refuses `!float && wBitsPerSample != 16` and lets
  the caller fall back (the reverse of the v13.1 float-misread fix).
- **R5** — `asio_driver` matching was case-sensitive despite the README promising
  case-insensitive; `focusrite` missed `Focusrite USB ASIO`. Now uses a case-insensitive
  substring (`StrIStr`).
- **R6** — `asio_driver` now also strips single quotes, not just double quotes.
- **R7** — `kAsioOverload` no longer calls `Log()` from the realtime callback (it counts and
  the monitor thread reports it); the `kAsioSelectorSupported` handler no longer advertises
  resync / latencies-changed / reset requests that it never handled.
- **R8** — the `timeBeginPeriod(1)` result is checked before the paired `timeEndPeriod` flag
  is set, so a failed request is not wrongly undone at detach.
- **R12** — documented that `FakeRC::GetBuffer`'s 5×`Sleep(1)` back-pressure depends on
  `timeBeginPeriod(1)`.
- **R3 / R4** — documented as deliberate trade-offs: probing `*` really does `init()` each
  driver (bounded, and pinning `asio_driver` avoids it), and exclusive is intentionally
  never re-attempted after its self-test fails (the failure is a device property, not a
  glitch, and exclusive is not a recommended path). R9/R10/R11 are diagnostic/leak notes
  already marked acceptable in the source.

### Removed

- `exclusive2` is gone entirely — the dead config branch, its log line, the README note and
  the stale "allow a later retry" comments on `g_asioStartReq` were removed. An old config
  that still sets the key now falls through to the generic `unknown key` line and is ignored.

### Docs

- Every remaining machine-specific detail (interface model, test-host wording) was replaced
  with generic text; examples use placeholder driver/device names.
- Config examples and the generated `audio_hook.txt` are brand-neutral.

### Still deferred

- **P2-4** the exclusive self-test still runs synchronously (~600 ms) on the game's init
  thread; **P3-4** `CoInitializeEx` is not paired on worker threads; **P3-5** the
  endpoint-notification callback is not unregistered; **P4-13** device names still use
  `CP_ACP`; **P4-14** the detach report is still written from `DllMain`; **P5-6** the
  single-file layout is unchanged.
- **N17** (worst-case start-up blocking) and **N18** (apartment mixing) — documented, no
  code change: both are design-level and the current behaviour is verified working.

### Build

`/W3` and `/W4`: zero warnings.

---

## [13.2] — 2026-10-18

Second pass over `AUDIT-v13.1.md` (the re-audit of 13.1) plus a full review of the three
companion tools. The main path is unchanged; the fixes are a regression in the new
resampler and a set of correctness/robustness issues in the tools.

### Fixed — shared backend on endpoints below 44.1 kHz (N1, regression)

`WoThread` computed `n = min(free, RingAvail())` with mixed units: `free` is **output**
frames (device rate) while `RingAvail()` is **source** frames (44.1 kHz). With
`step > 1` (any endpoint below 44.1 kHz, e.g. 32 kHz) one output frame consumes more than
one source frame, so the resampler ran dry and ~27 % of every period was padded with
silence — music played at **72.6 % speed**, in periodic chunks.

```c
UINT32 avail = RingAvail();
if (g_rsStep > 1.0) avail = (UINT32)((double)avail / g_rsStep);  /* source -> output */
UINT32 n = free_ < avail ? free_ : avail;
```

`src/test_rs.cpp` now has a second test that reproduces `WoThread`'s 441-frame gate. It
reports `music/total = 72.6 %` (and `50.0 %` at 22.05 kHz) against the old formula and
`100.0 %` against the fixed one, so this cannot regress silently again.

### Fixed — 24-bit ASIO output wrote the wrong byte (N27)

`WriteSamples` converted 16-bit samples to 24-bit with `<< 16`. The three stored
bytes are bits 0-23 of that word, i.e. **bits 16-31 of the shifted value** — only
the sample's low byte survived: values below 256 became silence, 16384 became 0,
32767 wrapped to a tiny value. It is `<< 8` (the `asio_list` copy always had it
right). The interface used for testing reports `ASIOSTInt32LSB` (type 18), which is why
neither audit run heard it; a driver reporting type 17 would have been silent.

### Fixed — smaller accuracy issues in the same file

- **N3** — when the client takeover fails and the mod falls back to the real
  `Initialize`, it now still drops `RATEADJUST` (`flags = 0x00040000`), so the fallback
  keeps the 22 ms buffer instead of silently losing that trim as well.
- **N4** — `WoShutdown`'s comment said "blocks ≤ 200 ms"; the loop is 400 × 5 ms = 2 s.
- **N5** — `WoInit` refuses an implausible endpoint rate (outside 8 000…384 000 Hz)
  instead of turning it into a resampler step of `inf`.

### Companion tools (all three re-reviewed)

**`asio_list`**
- A `--test NAME` that matches nothing used to fall back to the **first** driver — so you
  could "verify" the wrong device. It is now an error (exit 2) that lists the real names.
- Case-insensitive matching (`usb asio` works); `--test` without a value and unknown
  arguments are usage errors instead of being ignored.
- Cleanup now runs `stop` → `disposeBuffers` → `Release` on every path. The old build
  never freed the buffers, so testing driver after driver could leave the previous one
  (and sometimes its device) claimed.
- The tone's phase was advanced once per **channel**, so left and right were one block
  apart and the fade-in ran twice as fast. Phase is advanced once per buffer now.
- `getBufferSize` / `getChannels` / `getChannelInfo` are checked; unknown sample types are
  refused (the old build played silence when `getChannelInfo` failed); a reported
  `maxBufferSize` of 0 can no longer produce a block size of 0.
- Registry entries without a `clsid` are skipped instead of being listed as unplayable.
- An in-flight counter now guards `disposeBuffers()`: `stop()` does not promise that
  `bufferSwitch` has returned, and a callback writing into freed buffers runs on the
  driver's thread — outside the tool's `__try`. It waits 200 ms, then leaks instead.
- Exit codes: 0 ok · 1 no drivers · 2 failed/no match · 3 usage · 4 driver crashed.

**`excl_rate_probe`**
- **Device clocks do not tick in samples.** A shared-mode stream was measured reporting
  **352 800 units/s** (8 × 44.1 kHz), so the old `clock ÷ nominal` math printed a healthy
  endpoint as **784 %**. The percentage is now `clock delta ÷ GetFrequency()`, and the raw
  unit rate is printed for auditability.
- `--shared` no longer falls through into the EXCLUSIVE verdict (it used to print
  "NO working exclusive format" after a successful shared measurement).
- `--list` dereferenced a NULL device collection when `EnumAudioEndpoints` failed; it now
  reports the error, marks the default endpoint, and exits non-zero.
- The `0x88890019` "retry" re-sent byte-identical parameters (dead code) — removed, the
  alignment error is reported per candidate.
- Device names are UTF-8 (they printed as `?????`); `--seconds` is validated and clamped
  to 1…60; unknown options are usage errors.
- Falls back to the written rate only when there is **no** `IAudioClock` object; a clock
  that exists but never advances is now scored 0 % and labelled `(clock not advancing)`,
  which is the actual failure mode being hunted.
- Counts the pre-filled buffer in `written` (it is delivered too), so short runs no
  longer under-report by one buffer.
- Offers **24-in-32** candidates (`wValidBitsPerSample = 24`, 32-bit container): pro
  interfaces often accept only that, and the old table always set valid == container.
- Exit codes: 0 measured ≥ 95 % · 3 below that / nothing worked · 1 device error · 2 usage.

**`session_tool`**
- Matching is case-insensitive; it used to fail silently on `divamegamix`.
- Exit code 2 when `--unmute` / `--mute` matched no session (with a hint), so the tool no
  longer claims success when the game was simply not running.
- Device names are UTF-8; `PropVariantClear` runs for **any** variant type; a device header
  is always printed (a failed `GetId` used to misfile sessions under the previous device).
- Checks `EnumAudioEndpoints` / `GetSessionEnumerator` / `GetCount` instead of assuming
  success; missing arguments and unknown flags are usage errors; the system-sounds session
  (`pid 0`) is never muted by a pattern.
- The report is written next to the exe (append + 4 MB rotation to
  `session_tool_report.1.txt`), and a read-only folder now produces a warning instead of
  silently no report. Process names are read as UTF-16 and printed as UTF-8, so an exe
  with a non-ASCII name can still be matched by `--unmute`.
- Processes that cannot be queried show `(denied)` rather than `(gone)`.

### Build

`/W3` and `/W4`: zero warnings (DLL and all three tools).

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

### Measured (USB ASIO interface, 44.1 kHz)

| Component | Value |
|---|---|
| CRI staging buffer handed to the game | 441 frames = **10.0 ms** |
| ASIO block | 64 frames = **1.45 ms** |
| ASIO driver-reported latency | 181 frames in/out = **4.1 ms** |
| WASAPI shared engine buffer | 970 frames = **22.0 ms** |

Steady state over 25 s runs: **44,100 frames/s, 0 underruns, 0 dropped frames** on both
`asio` and `shared`. Exclusive: a USB interface **88.5 %**, an onboard codec **0.3 %** of
real time → both roll back automatically.

---

## [12.0] — 2026-10-17 *(experimental, never released)*

### Added

- `exclusive2`: EXCLUSIVE + polling prototype — drop `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`
  and pace CRI from our own thread, so the engine clock would no longer lead the device.

### Removed

- **Abandoned.** With polling, that driver only granted **1768 frames (40 ms)**
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
  (many onboard codecs) had twice their buffer overwritten → heap corruption and a crash
  after playback. Frame size is now derived from the stream's sample format.
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
asio_list --test "USB ASIO"        :: play on that driver directly
```

Copy the **full** driver name it prints into `asio_driver`. Matching is a substring search
that takes the first hit, so a short string can select a different driver.

### `excl_rate_probe` — is exclusive mode real?

```bat
excl_rate_probe --list
excl_rate_probe "Speakers" --seconds 2
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
