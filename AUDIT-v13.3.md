# audio_hook.cpp v13.3 源码审查（第三轮）

- 对象：`modding-docs/release/PDMMP-AudioHook/src/audio_hook.cpp`（2450 行，commit `5c0e812`，含 `e908397` v13.3 全部改动）
- 日期：2026-10-18
- 方法：全文通读 + 本机 MSVC 14.51 `/W4` 编译探针 + 与 v13.1 审计遗留项逐条对账
- 前序：AUDIT-v13.0.md（48 条）/ AUDIT-v13.1.md（复审 + N1..N27）

## 0. 结论

| 级别 | 数量 | 说明 |
|---|---|---|
| P1 | 1 | R1 双消费者竞争（ASIO 重试 × shared 兜底同时读环形缓冲） |
| P2 | 3 | R2 非 16bit/非 float 共享格式误渲染；R3 ASIO 探测会逐个 init 驱动（DAW 机器副作用）；R4 独占回退后不再升级 |
| P3 | 8 | 见 §3 |

验证：本机 `/W4` 编译 **0 警告**（EXIT=0）。注意：仓库里的 v13.3 dll 已构建，但 `mods/Audio Hook/audio_hook.log` 最新记录仍是 v13.2（10:27）——**v13.3 尚未实跑验证过**，本文全部为静态结论。

### 修复状态（2026-10-18 复核）

| 编号 | 状态 | 处理 |
|---|---|---|
| R1 双消费者 | ✅ 已修 | `AsioStartThread` 在 `start()` 前 `if (g_woState==3) WoShutdown()`；`WoStart` 开头 `g_asioState==3` 时拒绝并记日志 |
| R2 非 16bit/非 float 噪声 | ✅ 已修 | `WoInit` 拒绝 `!g_woFloat && wBitsPerSample!=16`，落到兜底 |
| R3 探测 init 副作用 | ⏸ 权衡 | 代码注释 + README 已写明"多驱动建议 pin"；8 次上限保留 |
| R4 独占失败不回试 | ⏸ 有意设计 | 注释说明：自检失败是设备属性（<95%），独占非推荐路径 |
| R5 大小写敏感匹配 | ✅ 已修 | 新增 `StrIStr()`，`focusrite` 现能匹配 `Focusrite USB ASIO` |
| R6 单引号不剥 | ✅ 已修 | 双/单引号都剥 |
| R7 kAsioOverload 回调 I/O + 夸大支持 | ✅ 已修 | overload 改原子计数、monitor 线程报告；`kAsioSelectorSupported` 只答真正处理的 selector |
| R8 timeBeginPeriod 返回值 | ✅ 已修 | `g_timerPeriod = (timeBeginPeriod(1)==TIMERR_NOERROR)` |
| R9 Log 无锁 | ⏸ 可接受 | 诊断日志，单行原子写已足够 |
| R10 Teardown 泄漏 | ⏸ 可接受 | 源码注释已承认 "leaking, not crashing" |
| R11 看门狗日志膨胀 | ⏸ 可接受 | 30s 一条，非无限增长 |
| R12 背压依赖 timeBeginPeriod | ✅ 已修 | 加注释固化该依赖 |

## 1. v13.1 遗留项对账（全部关闭）

| 编号 | 状态 | 证据 |
|---|---|---|
| N1 重采样单位错配（32k 端点 72.6% 速） | ✅ 已修 | 1683 行 `if (g_rsStep > 1.0) avail = (UINT32)(avail / g_rsStep);`，正是建议的一行 |
| N6 shared 后端无设备失效自愈 | ✅ 已修 | WoThread 1670-1673 `GetCurrentPadding` 失败→记日志→`g_woFault=1`→退出→state 4；WoWatchdog 2208-2221 前 5 次 3s 快重试、之后 30s 慢重试**不再放弃**（5c0e812 补齐） |
| N22 MSB/Float64 假通过真静音 | ✅ 已修 | `IsTypeRenderable()` 白名单（397-410），不可渲染直接拒 |
| N27 Int24LSB `<<16`→`<<8` | ✅ 已修 | 436 行，注释还引用了审计编号 |
| N2/N21 g_tmp 按首次 block 定尺寸 | ✅ 已修 | 688-694 行按当前 block 重分配 |
| N25 Stop/Reset 不清重采样器 | ✅ 已修 | `RsFlush()`（1534）保留 step 只清缓冲，Stop/Reset 都调 |
| N11 timeBeginPeriod 不配对的 | ✅ 已修 | DllMain DETACH 里 `timeEndPeriod(1)`（2315） |
| N26 "可重试但无触发点" | ✅ 已修 | 5c0e812 注释更正 + 看门狗慢速持续重试 |
| exclusive2 死代码残留 | ✅ 已清 | grep 无残留 |
| N4 WoShutdown 注释 200ms/实 2s | ✅ 注释已对齐 | 1837 行注释写明 2s |

按文档明确延后的（不算回归）：独占自检同步阻塞 600ms、CoUninitialize 配对、notifier 注销、CP_ACP 设备名、DllMain 写文件、单文件未拆。

## 2. 新发现问题

### R1【P1】ASIO 重试与 shared 兜底可以同时运行 → 两个消费者分食环形缓冲

**位置**：`AsioStartThread`（837-965）全程不检查 `g_woState`；`WoStart`（1857）全程不检查 `g_asioState`。

**触发链**（完全由已声明的"特性"拼出来）：

1. `output=asio`，ASIO 启动失败 → 863 行兜底 `WoStart(0)`，shared 后端跑起来（`g_woState=3`，WoThread 在消费环形缓冲）；
2. `AsioRollback`/失败路径把 `g_asioStartReq` 归零，注释明写 "a rebuilt stream1 may try ASIO again"（860 行）——**重试是设计行为**；
3. 之后游戏重建 stream1（拔插设备、驱动抢占都会触发，AddS1Client 的环形表注释自己就说了这个场景）→ `Hook_Initialize` 2016-2055：`outMode==0 && g_asioEn` → CAS 成功 → 新的 `AsioStartThread`；
4. `WaitForTapSteady()` 立刻通过（taps 一直在流）→ `AsioPrepare` → `start()` 成功；
5. 此刻 **WoThread 和 AsioFill 都在 `RingRead` 同一个环形缓冲**——每帧只被一个消费者拿到，两边各拿一半，两个输出同时裂音/变速。

`AsioStartThread` 里唯一看 `g_woState` 的地方是 851 行 `WaitForTapSteady` 失败后的兜底判断，正常路径没有。`FakeDriverThread` 的 `g_woReading` 闸门（1329）只挡排干逻辑，挡不住 ASIO 回调。

**修复建议**（两行语义）：
- `AsioStartThread` 在 `AsioPrepare()` 成功之后、`g_asio->start()` 之前：`if (g_woState == 3) WoShutdown();`
- `WoStart` 开头：若 `g_asioState == 3` 且非 ASIO 回滚调用方，直接返回 FALSE（或至少记日志）。

### R2【P2】shared 后端对 24-bit / 32-bit 整数 PCM 混音格式会输出噪声

**位置**：`WoInit` 1771-1776 只区分"float vs 非 float"；`WoFill16`（1590）把 `g_woFloat==0` 的一切格式都当 int16 写。

某些驱动的共享混音格式是 24-bit packed 或 32-bit **整数** PCM（不是 float）。此时 `g_woFloat=0`、`g_woCh` 正确，但 `WoFill16` 以 2 字节/样本步长写 3 或 4 字节/样本的缓冲 → 全量噪声。v13.1 修的正是"32int 误判为 float"的反向，这一侧还没堵。

**修复**：`WoInit` 里加一道闸——`!g_woFloat && wf->wBitsPerSample != 16` → 拒绝并记日志（落到 ASIO/独占兜底），或实现 WoFill24/32。

### R3【P2】`asio_driver=*` 探测会真刀真枪 init 每一个注册驱动

**位置**：`AsioPrepare`（723-768）。

`*` 默认下对最多 8 个注册驱动逐个 `CoCreateInstance + init + getChannels + getBufferSize + getSampleRate + createBuffers`。每步失败都 Release 了（703-715，很好），但 **init() 本身有副作用**：不少驱动 init 时会弹控制面板 UI、短暂抢占设备、或把设备从 DAW 手里抢过来再放开。在装着多个 ASIO 驱动（DAW 用户的常态）的机器上，游戏启动=逐个驱动被插拔一遍。

这不算 bug（是权衡），但建议：探测前先用 `getSampleRate` 之前不 init 不可得——无法纯静态过滤，所以至少在 README 写明"多驱动机器建议 pin `asio_driver=`"，并把 8 次上限降为可配。

### R4【P2】独占自检失败回退 shared 后，永远不会再试独占

**位置**：`WoStart` 1863 行 `if (st == 3) return TRUE;`。

`output=excl` → 自检失败 → 回退 shared（state=3）。之后即便设备条件恢复（自检失败可能只是瞬时），`Hook_Initialize` 重建 stream1 时再调 `WoStart(1)`，看到 state==3 直接返回 TRUE——**shared 永久锁死，独占不再有机会**。看门狗（2220）也只重建 shared。

量级：自检失败主因是硬件速率 <95%，属"硬件不行"，所以多数情况下不回试也对；但对"插拔后换了台支持独占的设备"这个场景，行为与配置不符。建议：WoWatchdog 在 `g_outMode==1` 时慢速阶段偶发重试独占（比如每 10 次 shared 重试插 1 次 excl）。

## 3. P3 级（不阻塞，记录在案）

| # | 位置 | 问题 |
|---|---|---|
| R5 | 748 行 | `strstr(nm, g_asioMatch)` 大小写敏感；用户写 `focusrite` 匹配不到 `Focusrite USB ASIO`。README 未说明 |
| R6 | 2109 行 | `asio_driver` 值只剥双引号，不剥单引号；且注册表 description 走 ANSI，非 ASCII 驱动名在日志里是乱码（CP_ACP 遗留，文档已声明延后） |
| R7 | 492-501 行 | `kAsioSelectorSupported` 对 `kAsioResetRequest` 答 1，但真收到时 default 分支 return 0 不处理——驱动可能因此行为异常；另 `kAsioOverload` 在实时回调线程里 `Log()`，与 466 行自己的"回调里禁止文件 I/O"注释冲突 |
| R8 | 2374-2375 行 | `g_timerPeriod=1` 先于 `timeBeginPeriod(1)` 赋值；调用失败时 detach 会白调一次 `timeEndPeriod`（无害，但不严谨） |
| R9 | 104-121 行 | `Log()` 的 `fwrite` 无锁；游戏线程/ASIO 回调/看门狗三方同时写时单行已不拆（1200 字节缓冲），但两行之间仍可穿插——诊断日志可接受 |
| R10 | 770-785 行 | `AsioTeardown` 200ms 等待后若回调仍在飞，跳过 `disposeBuffers` 但仍 `Release()`——在飞回调再触回调函数（静态函数，安全）但驱动对象引用计数语义模糊；已在注释中承认"leaking, not crashing"，可接受 |
| R11 | 2214-2220 行 | 看门狗在 WoShutdown 超时泄漏路径（线程永久卡死）下会每 30s 记一行失败日志直到进程结束——日志膨胀，无上限 |
| R12 | 1185 行 | `FakeRC::GetBuffer` 背压等待 `Sleep(1)`×5 依赖 `timeBeginPeriod(1)` 已生效；若未来有人把 timer 调用挪走，这里退化成 15.6ms×5=78ms 的混音线程卡顿。建议加一行注释固化这个依赖 |

## 4. 顺带确认无问题的点（防重复报告）

- 环形缓冲全程 `g_ring` 非空守卫（P0-1 的历史教训，2384 行无条件分配 + 失败时禁 fake/asio，稳）；
- `WoStart` 的 CAS 认领 + `!g_woAlive` 门，看门狗/回滚/重建三方并发不会双初始化；
- `WoInit` 失败路径释放全部 COM 对象（1817-1825），独占半初始化残留已清；
- `FakeRC` 背压 + `RingAvail()` 上限 441，环形缓冲不会溢出；
- `PatchSlots` 全成功或全回滚，vtable 不会半补丁；
- v13.3 的 `Hook_RC_ReleaseBuffer` SILENT 分支写零帧进环形缓冲，tap 路径不再饿死于静音段。

## 5. 一句话总结

v13.3 把 v13.1 复审的遗留项**全部关闭**，编译干净，改动质量好。剩下唯一够格 P1 的是 R1：两条"各自正确"的容错链（ASIO 重试 + shared 兜底）在 stream1 重建时会叠加成双消费者，建议优先修。R2（位深闸门）其次。R3/R4 属行为权衡，改不改取决于你对多驱动机器和独占优先级的取舍。
