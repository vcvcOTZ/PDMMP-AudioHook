# v13.1 复审报告（针对 AUDIT-v13.0 的 48 条）

复审对象：`src/audio_hook.cpp`（2266 行，build 2026-10-18 10:03，commit `def7cbf`）
对照：`AUDIT-v13.0.md`、`CHANGELOG.md` 13.1、`README.md` v13.1、实跑日志 `mods/Audio Hook/audio_hook.log`（10:04:26 起，v13.1）
复审日期：2026-10-18

---

## 0. 结论

**这轮改得很扎实。** 48 条里 41 条实质落实，4 条部分修复/降级，7 条按文档明确延后（其中 P2-4/P3-4/P3-5/P4-14/P5-6 属于设计取舍，合理）。三个 P0 全部真修，不是"日志改了代码没改"——我逐条比对了 diff 和运行行为。

三项独立验证：

| 验证 | 方法 | 结果 |
|---|---|---|
| "`/W4` 零警告" | 用本机 MSVC 14.51.36231 实编 `-W4 -EHsc -c` | **通过，0 warning** |
| 主路径无回归 | v13.1 实跑日志 30 s | `und=0 drops=0`，taps 44,096 帧/秒，`zeroTap=0`（handoff 已跳过），`[fix] cleared stale session-mute residue (deferred)` 已生效 |
| 重采样正确性 | 复刻 `WoThread + RsPull + 环形缓冲` 做数值模拟 | 32 kHz 端点出问题（见 **N1**） |

**但仍然有 2 个 P1 级别的问题（1 个新引入、1 个是本轮改动让它变严重）**：

1. **N1**：`output=shared` 在 **低于 44.1 kHz** 的端点上，每个缓冲有 27.4% 被填静音 → 音乐以 **72.6% 速度**播放且周期性断裂。代码里 `n = min(free_, avail)` 把「输出帧」和「源帧」两种单位混用了。模拟实测：44.1k/48k/96k 都是 100%，32k 是 **72.6%**。
2. **N6**：`shared` 后端**没有任何设备失效自愈**。`WoThread` 拿到 `AUDCLNT_E_DEVICE_INVALIDATED` 只是 `Sleep(2); continue;`，不记日志、不回滚、不重建 → 中途换耳机/切设备就是永久静音。而它现在是 fake 模式 ASIO 回滚后的**最后一道防线**，这个缺口比 v13.0 时更要紧。

---

## 1. 原 48 条的状态

### P0（3/3 全部修复）

| 编号 | 状态 | 核实点 |
|---|---|---|
| P0-1 ring 空指针 | ✅ | 2200 行无条件 `malloc`；`RingWrite/RingRead/RingAvail` 首行均判空；`g_ring` 失败时连带关掉 fake/asio/out。另加 `ReadConfig` 一致性纠正（1997-2002：`fake_client=1 && asio=0 && output=asio` → 强制 `output=shared` 并记日志） |
| P0-2 excl 回滚 UAF | ✅ | 新增 `WoShutdown()`（1692-1707）：等 `g_woAlive` 归零，超时则只 log+置 state=4 并 **return（真泄漏）**，不再释放。与 CHANGELOG 声明一致了 |
| P0-3 AsioPrepare 泄漏 | ✅ | 所有失败分支收敛到 `fail:`（680-692），统一 `stop → disposeBuffers → Release`；`buffers` 标志避免误 dispose |

### P1（15 条：11 全修、2 部分、2 轻微）

| 编号 | 状态 | 说明 |
|---|---|---|
| P1-1 fake 回滚永久静音 | ✅ | `AsioRollback` 723-730：fake 模式下改走 `WoStart(0)`，并复位 `g_asioStartReq` |
| P1-2 shared 缺采样率守卫 | ⚠️ **部分** | 48k/96k 修好了（线性插值，模拟验证 100%）；**<44.1k 仍错** → **N1** |
| P1-3 shared Start 失败被当成功 | ✅ | `WoStart` 1745-1751 等 `g_woState` 变 3 或 4，1 s 超时 |
| P1-4 `g_woAlive` 未清零 | ✅ | 1549-1550 |
| P1-5 陈旧静音清理失效 | ✅ **日志实证** | 改为遍历本进程会话（489-536）；`audio_hook.log` 第 102 行 `[fix] cleared stale session-mute residue (deferred)` 证明真的清掉了 |
| P1-6 半补丁自递归 | ✅ | `PatchSlots`（169-185）全量预检 + 全装或全回滚；`PatchSlot` 加 `already ours` 短路；`g_rcPatched`/`g_fakePatched` 用 CAS 抢占 |
| P1-7 FakeInit 失败不降级 | ✅ | 1830-1839：清 `g_fake.on` 后回落 `g_orig`（见 **N3** 的一个小疏漏） |
| P1-8 客户端表溢出 | ✅ | 两表都改成环形覆盖（224-249），`n > S1_MAX` 显式记日志 |
| P1-9 disposeBuffers 竞态 | ✅ | `g_asioBusy` 计数（433/446），teardown 等 200 ms，超时跳过 dispose |
| P1-10 SILENT 不转发 | ✅ | 941-954 按 512 帧块写零；`g_tapFrames` 同步增长 |
| P1-11 默认值不符 | ✅ | 63/68 改 1；日志新增 `MODE: v13 takeover` / `MODE: v9 buffer trim only`（实跑日志第 7 行可见） |
| P1-12 exclusive2 死代码 | ✅ | 代码全删（grep 仅剩配置键的忽略日志） |
| P1-13 Stop/Reset 不清 ring | ✅ | 1335 / 1345 |
| P1-14 GetPosition 冻结 | ✅ | `g_fakePlayed` 在 `RingRead` 里累加（343）+ 真 QPC |
| P1-15 错误码当背压 | ⚠️ 可接受 | 1109-1113 先等 5 ms 再抛；仍用 `0x88890006` 兜底，但已不是唯一手段 |

### P2（7 条：5 修、2 部分）

| 编号 | 状态 | 说明 |
|---|---|---|
| P2-1 多消费者 | ⚠️ 已缓解 | 新增 `g_woReading` 闸门（1253-1262），drain 不再抢读；`RingRead` 的 `g_rr = r + n` 仍是普通写，但 ASIO 与 WO 不可能同时为 3（`AsioRollback` 先置 4 再启动 WO），实际不可达 |
| P2-2 Sleep 精度 | ✅ | 2179 `timeBeginPeriod(1)`（小尾巴见 **N11**） |
| P2-3 fake 仍跑 handoff | ✅ | 794 起 `if (!g_fakeClient)` 才走；实跑日志 `zeroTap=0` 全程为 0 证实 |
| P2-4 同步 600 ms 自检 | ⏸ 延后（文档已写） | 但最坏叠加变大了，见 **N17** |
| P2-5 MsNow 除零 | ✅ | 85 + 2161 双保险 |
| P2-6 RC 槽位撕裂 | ✅ | 918-920：先 frames、`MemoryBarrier()`、后 ptr |
| P2-7 Log 行交错 | ✅ | 106-116 单次 `fwrite` |

### P3（8 条：6 修、2 延后）

✅ P3-1（1675-1683 全路径释放）、P3-2（`wf` 统一 `CoTaskMemFree`）、P3-3（1722 允许从 4 重试）、P3-6（追加 + 4 MB 轮转）、P3-7（变量已删）、P3-8（`if (!g_fakeRc)` 明确的进程级单例）
⏸ P3-4（`CoUninitialize`）、P3-5（通知回调未反注册）——已写进 CHANGELOG "Deferred"，可接受

### P4（16 条：14 修、2 延后/无害）

✅ P4-1（577 guard + 32 次上限）、P4-2（653-662 失败即中止）、P4-3（`AsioTypeBytes` 真实宽度清零）、P4-4（2212-2218 预填有效 `WAVEFORMATEX`）、P4-5（1637-1640 查 `SubFormat`）、P4-7（1753-1784 加 `IAudioClock` 交叉校验）、P4-8（handoff 仅非 fake）、P4-9（两个监控都改无上限）、P4-10（1931-1981 逐行解析 + 畸形行告警）、P4-12（`ms` 标签）、P4-15（`0x%llX`）、P4-16（probe 3 s + `[out]` 仅在 WO 运行时打）
⏸ P4-13（`CP_ACP`）、P4-14（DllMain 写报告）——延后，合理
~ P4-6（`FakeGetService` 仍 `E_NOINTERFACE`，但现在会打点）、P4-11（`IsFormatSupported` 仍恒 S_OK）——无害

### P5（8 条：6 修、1 延后、1 未做）

✅ P5-1（头注释按 v13 重写）、P5-2（README 76 行改成"不写代码段、只替换 COM 实现"）、P5-3/P5-4（CHANGELOG 与代码对齐了）、P5-5（README 配置示例里删掉 `exclusive2`）、P5-7（`/W4` 实测零警告）
⏸ P5-6（单文件拆分）
✗ P5-8（无回归断言脚本；新增 `test_rs.cpp` 是个开始，但它没覆盖到 N1）

---

## 2. v13.1 新发现

### N1（P1）`output=shared` 在 <44.1 kHz 端点上：27% 静音 + 音乐降到 72.6% 速

- 位置：`WoThread` 1567-1568 + `RsPull` 1464-1496
- 根因：`n = min(free_, avail)` 里 `free_` 是**输出帧**（设备采样率），`avail` 是**源帧**（44.1 kHz）。
  - `step < 1`（48k/96k）：请求 `n` 个输出帧只需 `n*step` 源帧，`avail >= n` 足够 → **没事**
  - `step > 1`（32k 等 <44.1k）：请求 `n` 个输出帧需要 `n*step` 源帧，`avail >= n` **不够** → `RsPull` 中途断供、`WoFill16` 补零
- 量化：每轮 `avail=441`（`FakeRC::GetBuffer` 上限就是 441，所以环形缓冲永远不超过 441 帧），请求 441 个输出帧只产得出 `441/1.378 = 320` 个 → 每缓冲 27.4% 是静音，音乐按 72.6% 速度播。
- 模拟验证（复刻同样的 `min(free_, avail)` 与 441 帧闸门）：

  ```
   44100 Hz  step=1.0000  music/total = 100.0%
   48000 Hz  step=0.9187  music/total = 100.0%
   96000 Hz  step=0.4594  music/total = 100.0%
   32000 Hz  step=1.3781  music/total =  72.6%   <-- 应为 100%
  ```

- 为什么单测没抓到：`test_rs.cpp` 用 `MockRead` 造了**无限供数的斜坡**，只验证插值数学（帧数、误差），不验证 `WoThread` 的取数上限；而且它是算法的**复制粘贴**版本，不 `#include` 真实实现，天然覆盖不到调用侧。
- 修复（一行）：

  ```cpp
  UINT32 avail = RingAvail();
  if (g_rsStep > 1.0) avail = (UINT32)(avail / g_rsStep);   /* 源帧 -> 可产出的输出帧 */
  UINT32 n = free_ < avail ? free_ : avail;
  ```

- 另建议给 `test_rs.cpp` 加一个"环形缓冲 + FAKE_FRAMES 上限"的用例，或直接让它 `#include` 一份抽出的小头文件，避免测试与实现漂移。

### N6（P1）`shared` 后端设备失效后永久静音，且不留任何日志

- 位置：`WoThread` 1556-1581，尤其 1562：
  ```cpp
  if (FAILED(g_woClient->GetCurrentPadding(&pad))) { Sleep(2); continue; }
  ```
- 现象：切默认设备、拔插耳机、独占被抢占 → `GetCurrentPadding` 返回 `AUDCLNT_E_DEVICE_INVALIDATED` → 循环里无限 `Sleep(2)`，`g_woState` 仍是 3，`g_woReading` 仍为 1（drain 也被挡住）→ 永久静音，日志无一行。
- 为什么现在更严重：v13.0 里 shared 只是三个后端之一；v13.1 起它是 **fake 模式 ASIO 回滚的兜底**（`AsioRollback` → `WoStart(0)`）。兜底本身没有自愈能力，等于把"偶发失败"变成"必然静音"。
- 修复建议：`WoThread` 里对 `AUDCLNT_E_DEVICE_INVALIDATED`（以及连续 N 次 `GetBuffer` 失败）置 `g_woState=4` 并退出，让上层 `WoStart` 能重建（`WoStart` 已支持从 4 重试）；同时在 `WatchdogThread` 里加一条：`g_woState==4 && g_fakeClient` → 重建 shared。顺带 `if (w != WAIT_OBJECT_0) continue;` 也应在连续超时十几次后记一行日志。

### N22（P2）"拒绝静音运行"的守卫漏了 MSB / Float64 格式

- 位置：`AsioTypeBytes` 371-387 与 `WriteSamples` 389-426 的集合不一致：
  - `AsioTypeBytes` 认可：`Int16MSB`、`Int24MSB`、`Int32MSB*`、`Float32MSB`、`Float64LSB/MSB`
  - `WriteSamples` 只实现：`Int16LSB`、`Int24LSB`、`Int32LSB*`、`Float32LSB`
- 后果：上面这 6 个格式能通过 `AsioPrepare` 的 `!AsioTypeBytes(...)` 检查（655-662），然后在 `default:` 分支被 **memset 成全零** → 全程静音，而对应用户看到的日志是"驱动的采样格式正常"。
- 修复：加一个 `IsTypeRenderable(long t)`（只列 switch 里真正支持的那几个），`AsioPrepare` 用它做门槛；或补上 MSB/64-bit 的转换（MSB 就是字节序翻转，实现成本很小）。

### N3（P2）takeover 失败的降级路径丢掉了 RATEADJUST 修剪

- 位置：1827-1839
- `fake = 1` 时没有执行 `flags = 0x00040000u`（那行在 `else` 分支里）。所以 `FakeInit` 失败回落真实 `Initialize` 时，传进去的仍是 `0x00140000` → 拿到 44.5 ms 而不是 22.0 ms。功能上不算错，但"降级"降了两级。
- 修复：回落前 `flags = 0x00040000u;`

### N2 / N21（P3）ASIO staging 缓冲按首次 block 定尺寸，重试时可能溢出

- 位置：670 `if (!g_tmp) g_tmp = malloc(block*2*sizeof(short));`
- `AsioRollback` 现在会复位 `g_asioStartReq`，注释也写着"允许之后重试"。一旦真的重试且驱动这次给出更大的 block，`RingRead(g_tmp, blk)` 就越界写。
- 修复：`if (!g_tmp || g_tmpBlock != block) { free(g_tmp); g_tmp = malloc(...); g_tmpBlock = block; }` 失败则 `goto fail`。

### N4（P3）`WoShutdown` 的注释写 200 ms，实际是 2 s

- 1695 `for (int i = 0; i < 400 && g_woAlive; i++) Sleep(5);   /* blocks <= 200 ms */` —— 400×5 ms = 2000 ms。日志那行写的 2 s 才对。这个函数在 `Hook_Initialize` 的同步路径上，差 10 倍的注释会误导后续调优。

### N5（P3）`WoInit` 没校验 `rate > 0`

- 1667 `g_rsStep = 44100.0 / (double)rate;` 若 `nSamplesPerSec == 0`（畸形 mix format）→ `step = inf` → `RsPull` 里 `while (g_rsFrac >= 1.0)` 会把 ring 一路抽干再退出（不挂死，但白跑）。加 `if (rate < 8000 || rate > 384000) goto done;` 即可。

### N9（P3）重采样时 `GetCurrentPadding` 少报排队量

- `Fake_GetCurrentPadding` 返回 `RingAvail()`，但 `RsPull` 会把整个 ring 搬进 `g_rsBuf`（最多 441 帧）→ CRI 看到的"待播"会比实际少最多 10 ms。因为 `GetBuffer` 用的是同一个 `RingAvail` 闸门，自洽、不会溢出（ring 64 K），只是 CRI 自己的 in-flight 记账偏乐观。留个注释说明即可。

### N11（P3）`timeBeginPeriod(1)` 没有配对 `timeEndPeriod(1)`

- 只影响进程退出前这段时间；但它会抬高**全系统**定时器精度，属于有副作用的选择。建议在 `audio_hook.off` 提前返回的分支里跳过，或在 `DllMain(DLL_PROCESS_DETACH)` 里 `timeEndPeriod(1)`。

### N12（P3）`Log` 截断时整行静默丢弃

- `_snprintf_s(..., _TRUNCATE)` 截断时返回 -1，`if (n > 0)` 直接不写。当前 `body[1024]` + 前缀 24 字符 < `line[1200]`，够用，但只要有人把 `body` 加长就会悄悄丢日志。建议 `n < 0` 时改为写入截断后的 `strlen(line)` 字节。

### N17（P3）启动期同步阻塞的最坏叠加变大了

- 现在最坏路径：`WoStart(1)` 里 1 s 等线程 + 600 ms 自检 + `WoShutdown` 2 s + `WoStart(0)` 再 1 s ≈ **4.6 s**，全在游戏音频初始化线程上。P2-4 已延后，但建议至少把 `WoStart` 的等待从 1 s 降到 300 ms（线程报到是毫秒级的），或者干脆异步化。

### N18（P3）线程套间混用

- `AsioStartThread` 是 STA；`AsioRollback` 在这个 STA 线程上调 `WoStart` → `WoInit` 里 `CoInitializeEx(MTA)` 返回 `RPC_E_CHANGED_MODE` 被忽略 → `MMDevice` / `IAudioClient` 代理建在 STA 上，随后 `WoThread`（未 `CoInitialize`，隐式 MTA）直接用这些指针。`mmdevapi` 的实现是自由线程的，所以实测没问题，但这是"靠实现细节成立"的用法。彻底的做法是把 `WoInit` 移到 `WoThread` 里做，或对 DAW 场景统一 `CoInitializeEx(MTA)`。

### N25（P3）`Fake_Stop` / `Fake_Reset` 没重置重采样器状态

- 两者都 `RingResyncToLatest(0)` 清了 ring，但 `g_rsBuf` / `g_rsCur` / `g_rsFrac` 里的残留帧还在（最多 441 帧），切歌/seek 后仍可能先播出一小段旧音频。加一行 `RsReset()`（注意 `RsReset` 会把 `g_rsStep` 打回 1.0，所以要么拆成两个函数，要么保存/恢复 step）。

### N26（P3）CHANGELOG 说"允许重试"，但没有重试的触发点

- `AsioRollback` 复位了 `g_asioStartReq`，可整个工程里没有第二处调用 `AsioStartThread`。要么在 `WatchdogThread` 里加"`g_asioState==4 && g_fakeClient && g_woState!=3` → 重建"，要么把这句话从注释里删掉，别让人误以为有自愈。

---

## 3. 修复优先级

| 顺序 | 条目 | 为什么先修 | 成本 |
|---|---|---|---|
| 1 | **N1** | 少量用户（32k 端点）会听到"变慢 + 卡顿"，症状诡异且难归因；一行代码 | 1 行 |
| 2 | **N6** | shared 是兜底后端，失效即永久静音且无日志；也是"换耳机就哑"这类报障的根因 | ~20 行 |
| 3 | **N22** | 守卫写着"refusing to run silent"却对 6 种格式静音，属于承诺未兑现 | ~10 行 |
| 4 | **N3** / N4 / N5 | 都是一两行的准确性修正 | 各 1~3 行 |
| 5 | N2/N21、N25、N26 | 消除"以后加功能就踩雷"的隐患 | 各几行 |
| 6 | N11、N12、N17、N18、N9 | 稳健性/可维护性 | 视情况 |

---

## 4. 建议补的两个测试

1. **重采样 × 环形缓冲上限**：把 `FAKE_FRAMES` 闸门也搬进 `test_rs.cpp`，断言 32k/48k/96k 三种 step 下 `music/total == 100%`。现在的用例在有无限源数据时必然通过，正好漏掉 N1。
2. **后端失效回归**：一个可脚本化的断言——日志必须出现 `MODE: v13 takeover`、`[asio] started` 或 `[wo] output running`，且稳态 20 s 内 `und=0 drops=0`；再加一条负向用例（把默认设备切走后 5 s 内必须出现一行 rollback/failure 日志）。当前 N6 的静音在这种脚本下会立刻暴露。

---

## 5. 一句话总结

v13.1 把 v13.0 里"失败路径会崩/会哑"的问题基本扫干净了，主路径实测无回归、`/W4` 干净，是一次高质量修复。**剩下两个 P1 都不是主路径问题**：一个是新加的重采样在 <44.1k 端点上的单位错配（N1），一个是老问题——shared 后端一直没有设备失效自愈（N6），而它现在承担了兜底职责，所以该补上了。
