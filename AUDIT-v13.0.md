# audio_hook.cpp 源码审查报告（v13.0 / 2026-10-18）

审查对象：`src/audio_hook.cpp`（1831 行，build 2026-10-17 23:36）
参照：`README.md`（v13.0）、`CHANGELOG.md`（13.0 / 12.0 / 11.x）、`mods/Audio Hook/audio_hook.log`（2026-10-17 23:36 实跑日志）、`audio_hook.txt`（现役配置）

---

## 0. 结论摘要

代码整体设计是成立的：只改 COM vtable 数据层、fake client 接管 stream1、统一环形缓冲 + 三个输出后端，实测日志 `und=0 / drops=0 / 44,100 fr/s` 也确实是干净稳态。**问题集中在失败路径和"非主流配置"上**：正常配置（`fake_client=1 + output=asio + asio=1`）能跑，但只要换个后端、换个采样率、或者某个后端初始化失败，就会命中崩溃或无声。

共列出 **48 条**，按严重度：

| 级别 | 数量 | 含义 |
|---|---|---|
| P0 | 3 | 必然/高概率崩溃或进程级故障 |
| P1 | 15 | 功能性错误、与 README/CHANGELOG 承诺不符 |
| P2 | 7 | 并发与时序缺陷 |
| P3 | 8 | 资源泄漏、状态不可恢复 |
| P4 | 16 | 边界条件、健壮性、诊断准确性 |
| P5 | 8 | 可维护性、文档与代码不一致 |

最需要立刻处理的三件事：

1. **P0-1**：`fake_client=1` + `asio=0`（README 明确推荐的"无 ASIO 驱动"配置）会写空指针崩溃。
2. **P0-2**：`excl` 回滚的 use-after-free 并没有像 CHANGELOG 说的那样被修好，代码注释和实际行为自相矛盾。
3. **P1-2**：`output=shared` 缺 44.1 kHz 守卫，48 kHz 端点会把整条音频放快 8.8%（升调）。这是"任何设备可用"这一宣传语的直接反例。

---

## P0 — 崩溃 / 进程级故障

### P0-1 `fake_client=1` 且 `asio=0` 时 `RingWrite` 写 NULL 指针

- 位置：`PreInit()` 1785-1789；`RingWrite()` 282-293；`FakeRC::ReleaseBuffer()` 950-951
- 代码事实：环形缓冲只在 `if (g_asioEn)` 里分配：

  ```c
  if (g_asioEn) {
      g_ring = (short*)malloc((size_t)RING_CAP * 2 * sizeof(short));
      if (!g_ring) { Log("... ring alloc failed"); g_asioEn = 0; }
  }
  ```

- 触发链：`fake_client=1` → stream1 走 fake client → `FakeRC::ReleaseBuffer()` → `RingWrite()` → `memcpy(g_ring + idx*2, ...)`，`g_ring == NULL` → AV。
- 为什么现有防护挡不住：`FakeRC::GetBuffer()` 的容量检查用的是 `RingAvail()`，而 `RingAvail()` 在 `g_ring` 为 NULL 时只读 `g_rw/g_rr`，**不 deref**，返回 0 → `0 + 441 > 441` 为假 → 检查通过 → 必然走到 `ReleaseBuffer` 崩溃。
- 为什么这是"正常用户会遇到"的配置：README 232 行的可移植性建议原文就是 —— *"Any device, no ASIO driver: `fake_client = 1`, `output = shared`"*，而 156 行对 `asio` 键的说明是 *"Enable the ASIO worker when `output = asio`"*。用户按字面理解就会写 `asio = 0`。同理 `P0-3`（ASIO 准备失败后 `g_asioEn` 被清零）也会走到同一崩溃点。
- 修复建议：
  - 环形缓冲与 ASIO 开关解耦：只要 `g_fakeClient || g_asioEn` 就分配；
  - `RingWrite/RingRead` 首行加 `if (!g_ring) return;` 作为兜底；
  - `ReadConfig()` 末尾加一致性校验：`g_fakeClient && !g_ring` → 直接关掉 fake，退回 v9，并明确打一行日志。

### P0-2 `excl` 自检回滚中的 use-after-free（CHANGELOG 声明的修复未落地）

- 位置：`WoStart()` 1396-1404
- 代码事实：

  ```c
  InterlockedExchange(&g_woRun, 0);
  for (int i = 0; i < 400 && g_woAlive; i++) Sleep(5);
  if (g_woAlive) Log("[wo] !! thread did not stop in 2 s - leaking objects instead of crashing");
  if (!g_woAlive && g_woClient) g_woClient->Stop();
  if (g_woRender) { g_woRender->Release(); g_woRender = NULL; }   /* ← 无条件 */
  if (g_woClient) { g_woClient->Release(); g_woClient = NULL; }   /* ← 无条件 */
  if (g_woEvent)  { CloseHandle(g_woEvent);  g_woEvent  = NULL; } /* ← 线程可能正在等这个事件 */
  ```

  日志说"改为泄漏而不释放"，紧接着的代码却是无条件释放。
- 后果：`WoThread` 若卡在 `WaitForSingleObject(g_woEvent, 200)` 或 `GetCurrentPadding()`，对象被释放 + 事件句柄被关闭 → UAF / 句柄失效 → 游戏崩溃。
- CHANGELOG 13.0 明确写着 *"`excl` rollback released the COM objects while the writer thread could still be inside `WaitForSingleObject` → use-after-free. The rollback now waits for the thread to exit (up to 2 s) and leaks rather than frees if it does not."* —— 行为与声明直接冲突。
- 修复建议：把释放整段放进 `if (!g_woAlive) { ... }` 里，`g_woAlive` 仍为 1 时只打日志、把指针置 NULL 之外的引用计数交出去（真泄漏），绝不 CloseHandle。

### P0-3 `AsioPrepare()` 失败路径泄漏 ASIO 驱动对象，驱动被占用到进程结束

- 位置：`AsioPrepare()` 482-553（499 / 502 / 514 / 528 / 545 的 5 个 `return FALSE`）、`AsioStartThread()` 619-629
- 代码事实：`AsioPrepare()` 里 `g_asio->init()` 成功之后的所有失败分支都直接 `return FALSE`，既没有 `AsioTeardown()` 也没有 `Release()`；调用方 `AsioStartThread()` 失败时只把 `g_asioState` 置 4，同样不回收。
- 后果：ASIO 驱动在游戏整个生命周期内保持打开状态（`init()` 已调用，`createBuffers` 可能已建好 buffer）。其它 DAW / 音频程序会看到设备被占用；下一次也无法重试（`g_asioStartReq` 永不复位）。
- 触发场景不需要巧合：`src/asio_list.cpp` 的文档都提到"有些驱动加载会崩"，`asio_driver` 写错名字、设备被别的程序占用、采样率不是 44.1k 都会命中。
- 修复建议：所有失败路径收敛到 `fail:` 标签统一 `AsioTeardown()`；`AsioStartThread` 的 Prepare 失败分支同样调用 `AsioRollback()`。

---

## P1 — 功能性错误 / 与文档承诺不符

### P1-1 fake_client 模式下 ASIO 回滚 = 永久无声，且日志谎报"回到 WASAPI"

- 位置：`AsioRollback()` 566-574；触发点 685-688 / 692 / 706
- 代码事实：`AsioRollback` 只做三件事——`g_zeroTap=0`、`g_tapOn=0`、拆 ASIO。日志写 *"game audio returns via WASAPI"*，暗示 v11 的自愈行为。
- 但在 `fake_client=1` 下**根本没有真实 WASAPI 流**：stream1 的 `Initialize` 从未调真实实现，所以"把 tap 关掉让 WASAPI 出声"这句话不成立。实际效果是 `FakeDriverThread()` 的 drain 分支（1071-1077）以实时速率把 ring 里的音频读出来丢掉 → 永久静音，且 `g_asioStartReq` 不会复位 → 不重试、不降级。
- 触发：ASIO 连续 3 秒无回调、ASIO 驱动 overload、设备被抢占。
- 修复建议：fake 模式下回滚目标应改为 `WoStart(0)`（shared 后端），并在 monitor 里允许重建后端；如果 shared 也失败，再做"尽量出声"的退化（例如恢复真实 Initialize 已不可能，此时只能提示用户）。

### P1-2 `output=shared` 没有 44.1 kHz 守卫 → 48 kHz 端点整条音频放快 8.8%

- 位置：`WoInit()` 1337-1341（只取 `GetMixFormat`，不校验采样率）、`WoFill16/Float` 1212-1254（把 44.1k 帧直接按帧数塞进 48k 流，全程无重采样）
- 代码事实：ASIO 路径有守卫（`AsioPrepare()` 511-515：`fabs(sr - 44100.0) > 1.0` 就拒绝并回退），WASAPI shared 路径**完全没有等价检查**。
- 后果：端点混音格式为 48 kHz（很常见，尤其是 HDMI / 蓝牙 / 部分板载）时，44100 帧的数据被当作 48000 帧输出 → 速度快 8.84%，音高升约 1.5 个半音。这不是"低延迟"，是"变速播放"。
- 与文档的冲突：README 第 111 行只对 ASIO 提"must run at 44.1 kHz"；第 232 行却把 `output=shared` 描述成"Any device"。第 233 行 *"otherwise the bridge refuses and falls back"* 这句话没有对应的 shared 实现。
- 修复建议：`WoInit` 里读 `wf->nSamplesPerSec`，≠44100 时要么明确失败并回退 ASIO，要么在写入前做一次线性/三次插值重采样（44100→N 的比例是固定值，环形缓冲里做整帧重采样代价可控）。

### P1-3 `shared` 后端 `Start()` 失败被当成成功返回

- 位置：`WoStart()` 1376-1385 + `WoThread()` 1261-1266
- 代码事实：`WoThread` 里 `if (FAILED(g_woClient->Start())) { state=4; return 0; }`，而 `WoStart()` 只对 `exclusive` 分支做 600 ms 自检；shared 分支 `CreateThread` 后立刻 `return TRUE`。
- 后果：shared 启动失败（设备被独占、端点失效）时，调用方认为成功 → 不再尝试 ASIO → 完全无声，日志里只有一行 `[wo] !! Start failed`。README 80 行的"staged fallback，each logged"在这一级断链。
- 修复建议：`WoStart` 里等 `g_woState` 变为 3 或 4（带 300~600 ms 超时），失败返回 FALSE 让上层继续 fallback。

### P1-4 `g_woAlive` 在 `Start()` 失败的早退路径没有清零

- 位置：`WoThread()` 1259 置 1，1265 早退时未置 0（只有正常出口 1296 置 0）
- 后果：excl 回滚路径会白等 2 秒（`for (i<400 && g_woAlive; ) Sleep(5)`），然后打出"thread did not stop"的误导日志，接着命中 P0-2。

### P1-5 陈旧 session-mute 清理在 v13 主路径上已失效（日志实证）

- 位置：`Hook_Initialize()` 1523-1524 → `MuteSession(FALSE)` → 420-431
- 日志实证（`audio_hook.log` 第 30 行）：

  ```
  [23:36:56.440] [asio] stream1 client registered: 0000000009526228 (total 1)
  [23:36:56.440] [asio] GetService(ISimpleAudioVolume) hr=0x88890001
  ```

  `0x88890001` = `AUDCLNT_E_NOT_INITIALIZED`。原因是 `g_s1Clients[0]` 现在是 fake client（真实 `Initialize` 从未调用），而 `MuteSession` 刻意用 `oGS`（真实 `GetService`）绕过了 hook。
- 后果：README 85 行 / CHANGELOG 11.1-11.2 承诺的"每次启动自动清除 v10d 遗留的持久静音"在 `fake_client=1`（现在的默认行为）下**完全不起作用**。一旦历史上被 `SetMute(TRUE)` 过，用户会看到"关掉模组也没声音"，而且没有任何补救。
- 修复建议：清理逻辑不要依赖 stream1 client。用 `IMMDeviceEnumerator` → `IAudioSessionManager2::GetSessionEnumerator` 遍历本进程会话 → `ISimpleAudioVolume::SetMute(FALSE)`（即 `session_tool --unmute` 的做法），进程级、与 stream1 无关。

### P1-6 vtable 半补丁 → 原始函数被覆盖成自己的 hook → 无限递归爆栈

- 位置：`Hook_GetService()` 810-821；`FakeInit()` 1168-1184
- 代码事实：

  ```c
  if (!g_rcPatched) {
      if (PatchSlot(vt,3,...) && PatchSlot(vt,4,...)) { g_rcPatched = 1; ... }
      else Log("!! RC patch failed");
  }
  ```

  `&&` 短路：若 slot 3 成功、slot 4 失败，slot 3 的改动**已经生效**但 `g_rcPatched` 仍是 0。下次再进这个分支时 `PatchSlot(vt,3,...)` 捕获到的"原始函数"就是我们自己的 `Hook_RC_GetBuffer` → `oRCGB` 指向自己 → 一旦调用立刻栈溢出。
- 同样模式在 `FakeInit` 里更危险：9 个 slot 用 `&&` 串联，任意一个失败都会留下 1~8 个已改写的槽位 + `g_fakePatched=0`。
- 修复建议：先对所有目标槽位做 `IsExecPtr` 预检；补丁失败时回滚已改的槽位；`g_rcPatched`/`g_fakePatched` 用 `InterlockedCompareExchange` 抢占，避免并发双补丁（见 P2 系列）。

### P1-7 `FakeInit()` 失败时把错误直接返回给游戏，而不是回退真实 Initialize

- 位置：1463-1464 `HRESULT hr = fake ? FakeInit(self, pFormat) : g_orig(...)`
- 后果：`FakeInit` 返回 `E_FAIL`（内存不足、vtable 补丁失败）时，游戏的音频初始化直接失败 → 没声音，而且没有任何 v9 保底（本来 `fake_client` 是"可选增强"）。
- 修复建议：`FakeInit` 失败 → 清 `g_fake.on`、还原已补丁槽位 → 落到 `g_orig(...)` 正常初始化，日志记一行降级原因即可。

### P1-8 `S1_MAX` / `S1RC_MAX` 只增不淘汰 → 重建 stream1 后 tap 静默失效 + 悬垂指针

- 位置：`AddS1Client()` 200-205、`IsS1Client()` 193-198、`MuteSession()` 420-431
- 代码事实：

  ```c
  LONG n = InterlockedIncrement(&g_s1ClientN);
  if (n <= S1_MAX) g_s1Clients[n-1] = client;   /* 超过 8 次就再也不登记 */
  ```

  而 `IsS1Client` 会 clamp 到 `S1_MAX` 去读——第 9 个 client 永远登记不进去 → `g_tapOn` 永不置 1 → tap 静默失效。同时 `g_s1Clients[0..7]` 里的旧 client 若已 Release（设备切换、游戏内重置音频），`MuteSession` 会对悬垂指针调 `oGS` → AV。
- 触发：切默认设备、拔插耳机、ASIO 驱动被抢占后游戏重建流。属于"用得久就会遇到"的类型。
- 修复建议：改成环形覆盖（`n % S1_MAX`）+ 记录时间戳；或在 hook 到 `IUnknown::Release` 时摘除（成本更高）。至少把 `n > S1_MAX` 的情况打日志，不要静默。

### P1-9 回滚时 `disposeBuffers()` 与 ASIO 回调竞争（UAF）

- 位置：`AsioTeardown()` 555-563；回调 `AsioFill()` 372-386 读 `g_abufs[c].buffers[index]`
- 说明：ASIO 规范里 `stop()` 返回并不保证 `bufferSwitch` 已经退出（驱动实现差异很大）。紧接着 `disposeBuffers()` 释放 buffer，若回调仍在跑就是写已释放内存 → 驱动/进程崩溃。这个风险在"设备被踢"这条最需要自愈的路径上最高。
- 修复建议：加 `volatile LONG g_asioBusy`，回调入口 `InterlockedIncrement`、出口 `Decrement`；`stop()` 之后轮询等归零（上限 200 ms）再 `disposeBuffers()`；超时则只 `stop()` 不 dispose。

### P1-10 tap 路径不转发静音帧，会被"runaway underruns"误判回滚

- 位置：`Hook_RC_ReleaseBuffer()` 756 的 `!(flags & AUDCLNT_BUFFERFLAGS_SILENT)`
- 代码事实：SILENT 时直接跳过整个 tap 块——既不 `RingWrite` 也不清零。而 fake 路径（950-951）处理正确：`RingWrite(g_fake.zero, n)`。
- 日志实证：CRI 确实会发这个标志 —— `[fake-rc] ReleaseBuffer(441, flags=0x2)`。
- 后果（`fake_client=0` + ASIO 模式）：歌间静音 / 过场静音期间 ring 断供 → `g_underruns` 每秒涨几百 → 704-711 的 `dUnd > 100` 连续 2 次 → `AsioRollback("runaway underruns")` → 本来只是静音，结果被打回 WASAPI（再叠加 P1-1 就是无声）。
- 修复建议：SILENT 分支也 `RingWrite(零缓冲, n)` 并计入 `g_tapFrames`，让 underrun 计数只在真断流时增长。

### P1-11 配置默认值与 README 表格不符

| 键 | README 声称默认 | 代码实际默认 | 位置 |
|---|---|---|---|
| `asio` | `1` | **`0`** | 70 行 |
| `fake_client` | `1` | **`0`** | 76 行 |

- 后果：`audio_hook.txt` 缺失、或被旧版文件覆盖（v11 的配置只有 `asio=1`）时，v13 的核心功能静默不生效，用户以为在跑新版本。日志里只有 `config: ... fake_client=0` 一行可查，README 133 行却让用户"看 header 是不是 v13.0"——header 是 v13.0，行为是 v9。
- 修复建议：代码默认值改成 README 的值；或反过来把 README 的默认列改成"缺文件 = v9 模式"，并在 header 日志里显式打印 `MODE: v9 fallback` / `MODE: v13 takeover`。

### P1-12 `exclusive2` 死代码仍在，且配置键仍可开启

- 位置：1440-1450（改 `shareMode/flags`）、849-866（`PseudoECThread`）、834-844（`Hook_SetEventHandle`）、181-182 状态变量
- 问题：
  1. CHANGELOG 12.0 已给出实测结论——"轮询模式下该驱动只给 1768 帧(40 ms)，比 shared 还差，CRI 自己的 buffer 校验不过 → 没声音 + 游戏卡住"。这段代码原样保留在 13.0 里。
  2. 更糟的是它现在**必然走不通**：去掉了 `EVENTCALLBACK` 却仍让 CRI 调 `SetEventHandle`，真实 WASAPI 会拒绝 → `g_s1Event` 恒为 NULL → `PseudoECThread` 里 `if (!c || !g_s1Event ...) { Sleep(5); continue; }` 变成永不退出的休眠循环（`g_pseudoEC` 也永不复位）。
  3. 若真进了这个分支，`PseudoECThread` 的 `if/else` 两支都是 `Sleep(0)` → 100% 占满一个核。
- 矛盾点：文件头注释（第 21-22 行）还写着 *"v11.2: EXCLUSIVE-mode experiment code fully removed"*，而代码就在 850 行之后。
- 修复建议：删掉整段（含 `g_exclusive2`、`g_s1Event`、`g_s1PeriodFrames`、`g_pseudoEC`、`g_ecSetEvents`、`PseudoECThread`、`Hook_SetEventHandle` 里的 exclusive2 分支，slot 13 只保留给 fake）。若想留档，读配置时强制清零并打一行 *"exclusive2 was removed in v13"*。

### P1-13 `Fake_Reset` / `Fake_Stop` 不清空环形缓冲

- 位置：1143-1147、1138-1142
- 后果：游戏内重开歌曲 / 切曲 / seek 时，ring 里残留上一首最多 441 帧（10 ms）的音频，会先被播放出来，听感是曲首"咔"一声或半拍串音。
- 修复建议：`Reset`/`Stop` 里调 `RingResyncToLatest(0)`，并考虑让 `FakeDriverThread` 在 stop 时暂停 drain。

### P1-14 `IAudioClock::GetPosition` 永远返回 0（`g_fakePlayed` 从未被写入）

- 位置：962 声明、988 唯一一处读取
- 代码事实：全文没有任何地方写 `g_fakePlayed`（已用 grep 确认）。也就是交给 CRI 的播放时钟是冻结的；`qpc` 参数恒填 0（WASAPI 里 0 表示"无当前时钟"，但 position 又给 0 属于自相矛盾）。
- 影响：目前 CRI 显然没有依赖它（否则音乐位置会卡死），所以是**潜伏 bug**——一旦 CRI 换用 `IAudioClock` 做播放位置（曲末结算、歌词同步、AV 同步都会用），行为会立刻错乱。
- 修复建议：在 `AsioFill` / `WoFill*` / drain 三处按实际消费帧数 `InterlockedExchangeAdd64(&g_fakePlayed, got)`；`GetPosition` 同时填 `QueryPerformanceCounter` 的值；或者干脆返回 `AUDCLNT_S_NO_CURRENT_POSITION` 明确表达"没有位置信息"。

### P1-15 `GetBuffer` 用 `0x88890006`（BUFFER_TOO_LARGE）当背压信号

- 位置：`FakeRC::GetBuffer()` 936-937
- 问题：这个 HRESULT 的官方语义是"请求帧数超过缓冲区大小"，不是"队列满"。用一个错误码表达"稍后再来"，依赖 CRI 内部对错误的容忍度（工程记忆里也提到过"CRI 0 帧容忍修复"，说明这条边界曾经出过事）。
- 修复建议：队列满时改为让 `GetCurrentPadding` 报一个更小的可用值，或在 `GetBuffer` 里等 `g_fake.ev`/短暂自旋（fake client 的 `GetBuffer` 不在实时线程，允许阻塞几毫秒），正常返回 `S_OK`。

---

## P2 — 并发与时序

### P2-1 `RingRead` 是多消费者，但实现假设单消费者

- 位置：295-308
- 事实：`g_rr = r + n` 是普通读改写。潜在读者有三个：`AsioFill`（ASIO 回调线程）、`WoThread`、`FakeDriverThread` 的 drain 分支。
- 真实竞争窗口：后端切换的瞬间——`WoStart` 里 `InterlockedExchange(&g_woState, 2)` 之后新线程已在跑，而 drain 分支的判据是 `g_asioState == 3 || g_woState == 3`（1053），状态 2 期间仍会 drain。两边同时 `RingRead` → 同一批帧被消费两次、`g_rr` 少加一次 → 听到重音或跳帧。
- 修复建议：加一个轻量消费者互斥（`InterlockedCompareExchange` 自旋或 `SRWLOCK`），或者把 `g_rr` 的推进改成 CAS 循环；更简单的做法是——drain 分支在 `g_woState >= 2` 时完全退出。

### P2-2 握手等待用 `Sleep(1)` 但没有 `timeBeginPeriod(1)`

- 位置：`AsioStartThread()` 662-667
- 事实：默认计时器精度约 15.6 ms，"`Sleep(1)` 到最后 2 ms 再自旋"实际上每次 `Sleep` 可能睡 15 ms → 过冲可达 13 ms。日志显示 56.752 决定 12 ms 握手、56.766 才 start（14 ms），基本符合这个偏差。
- 后果：ASIO 起播偏晚，与引擎排空窗口错位 → 起播处更容易出现一次可闻的空隙。
- 修复建议：`PreInit` 里 `timeBeginPeriod(1)`（退出时 `timeEndPeriod`），或者直接用 `CreateWaitableTimerEx` 的高精度定时器。

### P2-3 fake 模式下 `RingResyncToLatest(0)` 会丢掉已产生的 ~10 ms 音频

- 位置：633，配合 645-660 的 handoff 分支
- 事实：fake 模式下 `g_zeroTap` 照常被置 1（日志里 `zeroTap=1` 全程不变、`zeroed=0` 全程不变，说明这段逻辑在 fake 下是空转），但 `RingResyncToLatest(0)` 是实打实执行的——把 CRI 已经混好的最多 441 帧（10 ms）直接丢弃。
- 后果：每次起播都有一次 ~10 ms 的跳帧（听感是曲首极短的"缺一拍"）。同时 `handoffMs` 用的是 `g_s1BufFrames`（fake 下 = 441 = 10 ms + 2 ms），语义错位——fake 模式压根没有引擎队列要排空。
- 与文档冲突：CHANGELOG 13.0 *"With `fake_client = 1` the tap-and-zero handoff is no longer part of the audio path"* —— 代码里仍在跑。
- 修复建议：`g_fakeClient` 时整段 handoff 跳过（不 resync、不置 zeroTap、不等 handoffMs），ASIO 直接 `start()`，让 ring 里已有的帧自然播出去。

### P2-4 在游戏音频初始化线程里同步做 600 ms 自检 + 抢占设备

- 位置：1389 `Sleep(600)`、1529 在 `Hook_Initialize` 里同步 `WoStart()`
- 后果：
  1. `excl` 模式下游戏启动被阻塞约 600 ms（用户观感是"卡一下才开始出声"）；
  2. 独占流在 stream2 创建**之前**就抢占了设备 → stream2 必然 `DEVICE_USE_IN_USE` 失败（README 370 行已列为已知限制，但它其实是启动时序耦合的必然结果，不是设备限制）。
- 修复建议：先把 shared 拉起来出声，自检放到后台线程，通过后再切 exclusive；或者干脆按 README 的建议不再推 exclusive。

### P2-5 `MsNow()` 无失败保护

- 位置：89-93
- `QueryPerformanceFrequency` 若失败（理论上不会），`g_qpcFreq` 为 0 → 除零 → NaN → handoff 等待循环直接退出（`NaN < tEnd` 恒假）→ ASIO 立刻起播、重叠。加一行 `if (!g_qpcFreq.QuadPart) g_qpcFreq.QuadPart = 1;` 即可。

### P2-6 RC tap 槽位的双字段写入不是原子的

- 位置：`Hook_RC_GetBuffer()` 743
- `g_rcSlots[si].ptr` 与 `.frames` 是两个独立写。若 `GetBuffer` 与 `ReleaseBuffer` 不在同一线程（WASAPI 允许），可能读到"新 ptr + 旧 frames"的组合 → 拷贝长度错误 → 越界读或截断。加 `MemoryBarrier()` 并保证 frames 先写、ptr 后写（读者用 ptr 非空做门闩）即可。

### P2-7 `Log()` 不是行原子

- 位置：108-116
- 时间戳 `fprintf` 与正文 `vfprintf` 是两次调用，多线程（游戏线程 / ASIO worker / watchdog / 通知回调）并发时会出现"两行时间戳夹一行正文"的错行日志。改成先 `vsnprintf` 到本地缓冲、再一次 `fwrite`。

---

## P3 — 资源管理

| 编号 | 位置 | 问题 |
|---|---|---|
| P3-1 | `WoInit()` 1349-1361 | 4 个 `return FALSE` 路径都没 `Release()` `g_woClient`。最坏情况：独占 `Initialize` 成功但 `GetBufferSize` 失败 → 客户端仍持有独占流 → 上层回退 shared 时必然 `DEVICE_IN_USE` → 连带 ASIO 也受影响 |
| P3-2 | `WoInit()` 1323-1341 | `CoTaskMemAlloc(wf)` 在 4 个失败分支未释放 |
| P3-3 | `WoStart()` 1378 | `g_woState` 一旦变成 4 就永不复位（`InterlockedCompareExchange(&g_woState, 2, 0)` 只在 0 时生效）→ 该后端永久不可用，也不会重新 fallback |
| P3-4 | 607 / 1667 / 1304 | `CoInitializeEx` 无配对 `CoUninitialize`（ASIO worker、watchdog、WoInit 所在线程） |
| P3-5 | 1817-1821 | `g_notifier` 从不 `UnregisterEndpointNotificationCallback` / `Release`；进程收尾时 COM 仍持有指针 |
| P3-6 | 1766 | `fopen_s(..., "wb")` 每次启动截断日志——排查崩溃时最需要的"上一轮日志"恰好被冲掉。建议改成 append + 按大小轮转（保留 `audio_hook.log.1`） |
| P3-7 | 178-182 | `g_s1Event` / `g_s1PeriodFrames` / `g_pseudoEC` / `g_ecSetEvents` 在 exclusive2 关掉后完全无用但仍占状态，并误导后续维护者 |
| P3-8 | 1159-1160 | `FakeRC` / `FakeClock` 是永不释放的单例。是故意设计，但没有注释说明，静态分析会当泄漏报 |

---

## P4 — 边界与健壮性

- **P4-1**（473）`AsioChooseBlock` 在 `gr < 0`（2 的幂粒度）且 `mn == 0` 时死循环：`b = 0; while (b < block && b < mx) b <<= 1;` —— 永远涨不上去。驱动一旦报 `minBuffSize = 0`，ASIO worker 线程直接卡死。加 `if (mn <= 0) mn = 16;` + 迭代上限。
- **P4-2**（531-537）`getChannelInfo` 返回值未检查。失败时 `g_aci[].type` 保持 0（`ASIOSTInt16MSB`）→ 落进 `WriteSamples` 的 `default:` → 全程静音，而日志只打印了 type 数值，看不出是失败。
- **P4-3**（363-366）`WriteSamples` 的 `default:` 按每样本 4 字节 `memset`。若驱动是 `ASIOSTInt64LSB` / `ASIOSTFloat64LSB`（8 字节/样本），只会清零一半，另一半残留上块数据 → 白噪声。应按格式宽度清零，或在 `AsioPrepare` 里直接拒绝未知格式。
- **P4-4**（1105-1118）`Fake_GetMixFormat` 在 `FakeInit` 之前返回全零 `WAVEFORMATEX`。WASAPI 的标准调用顺序是 `GetMixFormat → IsFormatSupported → Initialize`；本作 CRI 是"自己构造格式直接 Initialize"（日志第 23 行 `flags=0x140000 dur=100000` 可证），所以没炸，但这属于**依赖调用顺序的隐患**：`nSamplesPerSec=0` 传出去，调用方一旦除零就是游戏崩溃。建议 `PreInit` 就把 `g_fake.fmt` 预填成 44100/16/2。
- **P4-5**（1339-1341）`g_woFloat` 只判 `wBitsPerSample == 32`，不看 `WAVEFORMATEXTENSIBLE.SubFormat`。32-bit **整型** PCM 端点会被当 float 解释 → 满量程爆音。建议判 `wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)`。
- **P4-6**（1013-1039）`FakeGetService` 对 `ISimpleAudioVolume` / `IAudioSessionControl` / `IAudioClockAdjustment` 一律 `E_NOINTERFACE`。目前 CRI 不查（日志无相关记录），但游戏内音量滑条 / 静音按钮若走这条链就会失效。至少把这些 riid 单独打点一次。
- **P4-7**（1389-1410）`excl` 自检用 `g_woOut`（写入帧数）而不是 `IAudioClock::GetPosition`（设备时钟）。这正是 `excl_rate_probe` 文档反复强调的 *"written 不算数，device clock 才是真的"*。副作用：ring 因 CRI 供数慢而饥饿时，写入速率也会掉，可能误判并误回退。建议自检期间同时读一次 `IAudioClock` 做交叉验证。
- **P4-8**（638-641）`handoffMs` 用 `g_s1BufFrames` 当"引擎队列深度"，fake 模式下这个是 441（fake 的 staging 大小），与实际语义无关。
- **P4-9**（682 / 1669）监控循环有硬上限：ASIO monitor 86400 次（24h）、watchdog 3600 次（1h）。超时后静默退出，之后任何异常都没有记录。改 `for (;;)` + 独立计数。
- **P4-10**（1575）`ReadConfig` 的 `strchr(p, '=')` 会跨行搜索。某行没有 `=` 时（例如写错成 `asio 1`），`eq` 会指到下一行的 `=`，key 变成一个跨行的怪字符串，`p` 再跳到更后面 → 中间的键被整段跳过。建议先按 `\n` 切行再处理。
- **P4-11**（1099-1104）`Fake_IsFormatSupported` 无条件返回 `S_OK`。对 fake 客户端撒谎是设计选择，但会误导"格式协商"类调用；至少对不支持的位深返回 `S_FALSE`。
- **P4-12**（251 / 262）`NotifyLogger` 里 `t=%.2fs` 打印的其实是毫秒（`MsNow()`）。排查超时问题时单位错一位会非常误导。
- **P4-13**（227 / 1643）设备 ID / 名称用 `CP_ACP` 转换。非中文/非英文本地化的设备名（俄语、日语）会变问号，日志无法定位设备。
- **P4-14**（1736-1742）`DllMain(DLL_PROCESS_DETACH)` 里做 `CreateFile/WriteFile/VirtualQuery/GetModuleFileNameA`，全部在 loader lock 下。虽是诊断用途，但属于官方明确不建议的用法（可能死锁）。建议改成 `DisableThreadLibraryCalls` + 用一个常开的句柄，或改成 hook 游戏退出点再写报告。
- **P4-15**（1813）`Log("... %p", g_orig)` 用 `%p` 打函数指针，在 x64 MSVC 上能工作但严格说是 UB，且有的 CRT 会加 `0x` 有的不加，日志解析会不一致。统一转 `unsigned long long` 打 `0x%llX`。
- **P4-16**（诊断噪音）`DeviceProbeOnce` 在前 20 秒每秒跑一次（每次 3 个 COM 对象 + 全设备枚举），日志里 4 行/秒；同时 `FakeDriverThread` 每 5 秒还会打一行 `[out] cbs=... wo=0`，在 ASIO 模式下这些 WO 计数器恒为 0，没有信息量。建议 probe 降到前 3 秒，`[out]` 行只在 `g_woState != 0` 时打印。

---

## P5 — 可维护性与文档一致性

- **P5-1**（文件头 1-37 行）注释仍写 *"audio_hook.cpp -- PDMMP audio hook (v11.3 build, 2026-10-17, CURRENT SOURCE)"*，并且整段描述的仍是 v11 的 tap-and-zero 主线，还声称 *"v11.2: EXCLUSIVE-mode experiment code fully removed"*。三处都与 v13 实际代码矛盾（`PreInit` 打的是 v13.0；exclusive2 代码在 850 行后）。头注释需要按 v13 重写。
- **P5-2**（README 76 行）*"No code bytes are written and no return values are forged."* 与 fake client 的实现正面冲突：`Fake_*` 系列全都在伪造返回值（`GetBufferSize=441`、`GetCurrentPadding=ring 占用`、`GetStreamLatency=0`）。建议改成"不写游戏代码段；只替换 COM 接口的实现与返回值"。
- **P5-3**（CHANGELOG 13.0 "Fixed" 第 4 条）声称的 excl 回滚修复没有落地，见 P0-2。
- **P5-4**（CHANGELOG 13.0 "Changed" 第 1 条）声称 handoff 已从 fake 路径移除，实际仍在跑，见 P2-3。
- **P5-5**（README 149 行）`exclusive2` 只出现在配置示例的注释里，没进配置表，也没说明它是"能把音频搞挂的开关"。要么删掉这个键，要么在表里写清"已废弃，置 1 会挂"。
- **P5-6**（整文件）1831 行单文件混了 5 个职责：vtable hook、fake client、ASIO 后端、WASAPI 后端、诊断。几乎所有状态都是文件级全局变量（30+ 个），模块之间靠全局变量隐式通信，这也是 P0/P1 里"失败路径漏清理"大量出现的根因。建议至少拆成 `hook.cpp / fake_client.cpp / backend_asio.cpp / backend_wasapi.cpp / diag.cpp`，配置收进一个 `struct Config`。
- **P5-7**（`build.bat`）编译用 `-W3`。上面 P4 里好几条（函数指针 `%p`、忽略返回值、`long`↔`LONG64` 窄化）在 `/W4` 下会直接报出来。建议 `/W4 /WX`（至少 `-W4`），并加 `/permissive-`。
- **P5-8**（测试）没有回归入口。建议加一个极简日志断言脚本：启动后必须出现 `stream1 v13: FAKE client`、`[asio] started`，且稳态 20 秒内 `und=0 drops=0`；否则 CI 失败。这样 P1-3（shared 假成功）、P1-10（静音误判回滚）这类问题能被自动抓住。

---

## 附录 A：三个可以立刻做的验证实验

按以下顺序做，每条都能在 5 分钟内证伪/证实对应结论（**建议先备份 `audio_hook.txt`**）：

1. **验证 P0-1**：`audio_hook.txt` 改成 `fake_client = 1` / `output = shared` / `asio = 0` → 启动游戏。预期：进游戏后第一次出声瞬间崩溃（AV on write at NULL）。若真崩，说明这条是 P0 无疑。
2. **验证 P1-2**：Windows 声音设置里把默认输出设备格式切成 **48 kHz / 24 bit**，配置改回 `fake_client=1` / `output=shared` / `asio=1` → 播一首熟悉的歌。预期：整体速度和音高都偏高（约 +8.8%，接近一个半音）。
3. **验证 P0-3**：`asio_driver = NoSuchDriver`，`output=asio` → 启动游戏并观察日志出现 `[asio] CoCreateInstance ... hr=0x80040154`。预期：游戏无声（回退应该走到 WoStart(0)，但那需要 fake_client=1），随后用任意 DAW 打开该 ASIO 驱动，看是否报"设备已被占用"。

## 附录 B：修复优先级建议

| 顺序 | 条目 | 理由 |
|---|---|---|
| 1 | P0-1、P0-2、P0-3 | 崩溃 + 设备占用，且都是"改几行就能修"的形态 |
| 2 | P1-11、P1-5 | 默认值与文档不符、核心清理逻辑在默认路径上失效——影响所有用户但难被发现 |
| 3 | P1-2、P1-3、P1-7 | 三个后端的"失败/边界"路径完整性问题 |
| 4 | P1-6、P1-9、P1-10 | 并发与递归，属于"偶发崩溃"的来源 |
| 5 | P1-12、P5-4、P5-1 | 删死代码 + 同步文档，避免维护者被误导 |
| 6 | 其余 P2/P3/P4 | 稳定性与可诊断性改进 |
