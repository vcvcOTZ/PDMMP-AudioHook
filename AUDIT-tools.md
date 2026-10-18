# 伴随工具源码审查：excl_rate_probe / asio_list / session_tool

审查对象（v13.1，commit `def7cbf`）：
- `src/excl_rate_probe.cpp`（505 行）
- `src/asio_list.cpp`（471 行）
- `src/session_tool.cpp`（318 行）

对照：`README.md` v13.1（第 252-390 行"Companion tools"）、`src/audio_hook.cpp`（同类算法的另一份实现）
日期：2026-10-18

---

## 0. 结论

三个工具的整体质量不错：命令行/退出码语义清楚、COM 对象基本都能配对释放、`--test` 不再静默挑错驱动、设备名改 UTF-8、`--unmute` 的误匹配问题都真的修了。它们不是玩具脚本，错误分支写得比多数同类工具认真。

**没有 P0。** 有一个 P1，落在 `asio_list`：回调线程与 `disposeBuffers()` 之间没有在飞保护——这正是 `audio_hook.cpp` 里已经用 `g_asioBusy` 修过的同一类问题（那边的 P1-9），这个工具没同步。而 README 第 302 行恰好写着"one bad driver cannot take down the tool"。

**另有一个跨文件发现（比这三份里的任何单条都重要）**：`audio_hook.cpp` 的 `ASIOSTInt24LSB` 写出是**错的**，而 `asio_list.cpp` 里同一件事是对的。两份审计（v13.0 / v13.1）都漏了这条，因为测试用声卡报告的是 `type=18`（`ASIOSTInt32LSB`），那条分支从未跑过。详见第 4 节。

统计：**P1×1、P2×11、P3×12**（含跨文件 1 条）。

---

## 1. excl_rate_probe.cpp

工具定位是"诚实的测量器"，所以这里的问题几乎全是"测量结论可能偏离真值"或"结论无法归因"。

### EP-1（P2）候选格式缺 24-in-32，pro 接口会被误判为"独占不可用"

- 位置：`Cand cand[]` 389-397，`wfx.Samples.wValidBitsPerSample` 425
- 现状：只试 `{44.1, 48} × {16 bit, 24 bit, 32 bit float}`。而专业接口在独占模式下最常见的格式是 **32 位容器 + 24 有效位**（`wBitsPerSample=32, wValidBitsPerSample=24, nBlockAlign=8`），很多驱动只接受这一种。
- 结构性问题：`Cand` 里只有 `bits`，`wValidBitsPerSample` 被直接赋成 `bits`（第 425 行），所以**无法表达 24-in-32**，加候选也表达不了。
- 后果：这类设备六个候选全失败 → 输出 `NO usable exclusive format` → 用户按 README 的"<50% = 驱动不跑时钟"去理解，结论就归错了；而实际上 `output=excl` 可能可用。
- 修复：`Cand` 增加 `int valid;`，候选补 `{44100,32,2,0,24}`、`{48000,32,2,0,24}`，`wValidBitsPerSample = valid ? valid : bits`。

### EP-2（P2）written-rate 回退有系统性偏低偏差，最大可达 20%+

- 位置：`RunOne` 131-136（预填）与 151-176（计量）
- 机制：`Start()` **之前**预填了整整一缓冲，但 `frames` 从 0 开始、`t0` 在 `Start()` 之后。那一缓冲在 `el` 期间确实被播出去了，却没被计入 → `written = frames/el` 偏低 `bufFrames/el`。
- 量化：44.1 kHz / 10 ms 缓冲 / 2 s 测量 → 偏低 2.2%；若驱动给 100 ms 缓冲且 `--seconds 1` → 偏低 **18%**。
- 命中条件：只在 `IAudioClock` 不可用时生效——而这正是文件头第 5 条 fix（"a device without IAudioClock was reported as 0%"）要兜的场景。也就是说修好的那条路径本身就带偏差。
- 修复：`unsigned long long frames = bufFrames;`（预填的算进去），或把 `t0` 挪到预填之后。

### EP-3（P2）"时钟不动"被报成"没有时钟"

- 位置：183-195
- `ck && c1 > c0` 为假就把 `usedClock` 置 0 并打印 `[no usable clock]`，随后用 written 率给百分比。但"时钟存在却不动"（Realtek 那种 0.3%）和"设备根本不暴露 IAudioClock"是两种完全不同的故障，前者正是这个工具存在的理由（README 348 行）。
- 后果：日志把最有诊断价值的情况描述成最无害的情况。
- 修复：`RateResult` 加 `clockPresent` / `clockStalled`，打印 `(clock present but not advancing)`。

### EP-4（P2）`--seconds` 非法值静默变成 1 秒，与 README 直接矛盾

- 位置：253-257、262-263
- 代码：`seconds = atoi(argv[++i]);` 后只做 `if (seconds<1) seconds=1;`。`--seconds abc` → 0 → 1 秒；`--seconds 0` / `--seconds -5` 同样。
- README 312-313 原文：*"`--seconds` accepts 1…60; **a bad value or an unknown option is a usage error, not silently ignored**"*。未知选项确实报错了，取值范围越界却没有。
- 修复：`strtol` + endptr 校验 + 范围检查，越界 `return 2` 并打 Usage。

### EP-5（P3）测量中途设备失效 → 只看到 ~0%

- 位置：158
- `GetCurrentPadding` 失败只 `Sleep(1); continue;`，硬等到计时结束，最后给出 `pct≈0`，看起来和"驱动不跑时钟"一模一样。
- 修复：命中 `AUDCLNT_E_DEVICE_INVALIDATED` 时填 `res->err` 并 break，让上层打印 `!! device invalidated`。

### EP-6（P3）`GetBuffer` / `ReleaseBuffer` 的返回值几乎都被忽略

- 162、164、132-135：`rc->ReleaseBuffer(n, 0);` 不检查。共享模式下失效只会在 `ReleaseBuffer` 上报，忽略后测量会"看起来正常但全是静音"。
- 另：131-136 若 `GetBuffer` 成功但返回 `p0 == NULL`，该缓冲永不 Release → `Start()` 可能返回 `AUDCLNT_E_OUT_OF_ORDER`。边界，但一句话就能补。

### EP-7（P3）`--list` 用名字判断默认设备

- 229 行：`!strcmp(nm, defName)`。两台同名设备会同时被标 `(default)`。应比较 `IMMDevice::GetId()`。

### EP-8（P3）琐碎

- `RunOne` 结束只 `Stop()` 不 `Reset()`（每个候选都重新 Activate，无影响，但和 `audio_hook` 的处理不一致）。
- `--help` 在 `SetConsoleOutputCP` 之前返回（当前全 ASCII，无实际影响）。

---

## 2. asio_list.cpp

### AL-1（P1）回调与 `disposeBuffers()` 之间没有在飞保护（README 的承诺过头了）

- 位置：`cbBufferSwitch` 121-145 + 清理段 324-336
- 机制：`InterlockedExchange(&g_playing, 0)` → `asio->stop()` → `asio->disposeBuffers()` → `g_asio = NULL`。但 ASIO 规范**不保证** `stop()` 返回时 `bufferSwitch` 已经退出（驱动实现差异很大）。若驱动在 `stop()` 后再打一次回调：
  - 走 `!g_playing` 分支，`memset(g_bufs[c].buffers[index], ...)` —— 这块内存刚被 `disposeBuffers()` 释放 → 对已释放内存写入；
  - 这段代码跑在**驱动的回调线程**上，不在 `TestTone` 的 `__try` 作用域内 → AV 直接杀进程。
- 矛盾点：README 301-303 *"Drivers that crash on load … are caught with SEH … one bad driver cannot take down the tool"*。严格说只覆盖"加载时崩"，回调线程崩它兜不住，而"设备被吊销"恰恰是最容易触发后置回调的时刻。
- 参照：`audio_hook.cpp` 的 `AsioTeardown()`（695-710）已经用 `g_asioBusy` 计数 + 200 ms 等待解决了同一件事（AUDIT-v13.0 的 P1-9）。
- 修复：加 `volatile LONG g_busy`，`cbBufferSwitch` 首尾 `InterlockedIncrement/Decrement`；`stop()` 后轮询等归零（≤200 ms），再 `disposeBuffers()`；超时就跳过 dispose（泄漏优于崩溃），并在崩溃时把 `g_bufs[c].buffers[h] = NULL` 置空让回调直接短路。

### AL-2（P2）`asio->Release()` 在 `__try` 之外

- 335 行。`stop()` / `disposeBuffers()` 都被 SEH 包住了，最可能崩的 `Release()` 反而没有。对已经出过问题的驱动（实测有几个 DAW 附带的驱动），Release 里做析构+卸载 DLL，最容易二次崩。
- 修复：把 `Release()` 挪进同一个 `__try`。

### AL-3（P2）`outputReady()` 在 `createBuffers()` 之前试探

- 263 行 `g_postOut = (asio->outputReady() == ASE_OK)`，而 `createBuffers` 在 271 行。ASIO 规范里 `outputReady()` 只在第一次 `bufferSwitch` 之后有意义；不少驱动此时返回 `ASE_NotPresent`（结果是 g_postOut=0，功能退化但无害），少数会直接拒。
- 修复：挪到 `start()` 之后（或第一次回调里）再探测。

### AL-4（P2）同一进程内重复测同一驱动会失败，用户会误判成坏驱动

- 位置：`RunTui` 的 Enter 分支 → `TestTone`
- 很多 ASIO 驱动只允许每个进程 `init()` 一次；第二次 `CoCreateInstance` 能成功但 `init()` 返回 false，工具报 `failed: init() returned false`。
- 在交互菜单里连测几个驱动、再回头测第一个，就会看到"刚刚明明响过，现在坏了"。
- 修复：在状态栏注明"同一驱动在一次运行内只能测一次"，或者干脆缓存结果；更彻底的做法是测完提示重启工具。

### AL-5（P3）`--test ""` 会静默选第一个驱动

- 441 行：`if (!test_drv[0] || test_drv[0] == '*') pick = 0;` —— 空串本该"匹配不到任何东西"，却回退到第一个。这正是文件头第 1 条 fix 声称已经消灭的行为（`"*"` 是显式写法）。
- 修复：只有 `"*"` 才 pick 0，空串走"no driver matches"。

### AL-6（P3）TUI 的列宽按字节算，中日文驱动名会错位

- 354-356：`%-44s` 补的是 UTF-8 **字节**数，而终端按显示宽度排版。`フォーカスライト` 这类名字会把状态行顶到奇怪的位置。
- 修复：按 `wcwidth` 或简单规则（CJK 记 2 列）手算填充。

### AL-7（P3）`EnableVT()` 失败未处理

- 339-345：老控制台不支持 VT 时，`\x1b[H` / `\x1b[7m` 会原样打出来。至少检查返回值并在失败时退回"每轮清屏重绘"。

### AL-8（P3）`FrameBytes(ASIOSTInt16MSB)` 等返回 0 → 明确拒绝

- 行为和 `audio_hook.cpp` 相反，这里是**对的**：不支持的格式直接报 `unsupported sample type` 而不启动。`audio_hook` 则让 MSB / Float64 通过校验然后 memset 成静音（AUDIT-v13.1 的 N22）。这条不是缺陷，是提醒两边不一致。

---

## 3. session_tool.cpp

### ST-1（P2）报告文件用 `"wb"`，把上一份冲掉

- 位置：106-115
- `fopen_s(&g_report, path, "wb")`。用户往往是"出问题后跑一次"，第二次跑就把上次（可能包含崩溃前的状态）覆盖了——和 `audio_hook` 里刚立下的规矩（追加 + 4 MB 轮转，理由就是"崩掉那次的日志才值得看"）自相矛盾。
- 修复：`"ab"` + 一次运行一个分隔头，或者写 `session_tool_report.txt` / `.1`。

### ST-2（P2）进程名用 ANSI API，非 ASCII 可执行名会让 `--unmute` 失配

- 位置：95 `QueryFullProcessImageNameA(h, 0, out, &n)`
- 设备名已经改成 UTF-8 了（文件头还把 CP_ACP 当成要修的 bug 批评了一通），但**进程名**还停在 ANSI。中文/日文 exe 名会输出 `?????`，`ContainsNoCase` 匹配不上，工具报"没有匹配的会话"。
- 修复：`QueryFullProcessImageNameW` + 已有的 `WideToUtf8`。

### ST-3（P2）报告写在 exe 目录，失败完全静默

- 位置：106-115
- 注释的理由是"Program Files 对非管理员只读，所以不要写 CWD"——但代码写的是 **exe 所在目录**，如果 exe 就在 Program Files，两个位置都不可写；`fopen_s` 失败后 `g_report` 为 NULL，`Out` 静静地只打屏，没有任何提示。
- 修复：失败时回退到 `%LOCALAPPDATA%` 或 CWD，并打一行 `(report not written: err)`。

### ST-4（P3）`matched`/`acted` 与退出码的组合会掩盖"半静音"

- 304-316：一个进程有两个会话、只有一个能 QI 到 `ISimpleAudioVolume` 时，`acted=1` → 退出 0，但另一个会话仍是静音。汇总行能看出来（"2 matched, 1 changed"），退出码却是乐观的。
- 修复：`acted < matched` 时返回 2（或引入 4 = 部分成功）。

### ST-5（P3）`-1.00` 占位值没有说明

- 260、267：`lvl = -1.0f` / `peak = -1.0f` 表示"取不到"，但打印出来和正常值长得一样（`volume=-1.00 peak=-1.0000`）。建议打 `n/a`。

### ST-6（P3）汇总用枚举数而非成功遍历数

- 312 行 `=== %u device(s) listed ===` 用的是 `devCnt`；某个 `Item()` 失败时实际列出的更少。

---

## 4. 跨文件发现（重要）：`ASIOSTInt24LSB` 在 audio_hook 里是错的，在 asio_list 里是对的

同一件事、两套实现、结果不同：

**`asio_list.cpp` 109-115（正确）**
```cpp
int32_t v = (int32_t)(sin(...) * a * 8388607.0);
o[i*3] = v; o[i*3+1] = v>>8; o[i*3+2] = v>>16;      // 24 位值直接取低 3 字节
```

**`audio_hook.cpp` 407-413（错误）**
```cpp
int32_t v = ((int32_t)src[i*2+ch]) << 16;           // 左对齐到 32 位
o[i*3] = (unsigned char)v;
o[i*3+1] = (unsigned char)(v >> 8);
o[i*3+2] = (unsigned char)(v >> 16);                // 只取到 v 的低 3 字节
```

因为 `(s<<16) & 0xFFFFFF == (s & 0xFF) << 16`，**16 位样本只剩低 8 位**，而且被顶到最高字节。实测映射：

```
 src     正确(24bit)   audio_hook   asio_list
 32767     8388352      16711680     1.000x   <- 2 倍满量程，削顶
 16384     4194304             0     1.000x   <- 完全静音
  8192     2097152             0     1.000x
   256       65536             0     1.000x
   257       65792         65536     1.000x
```

听感就是"音量极低 + 严重失真/削顶"。正确写法是 `int32_t v = (int32_t)src[i*2+ch] << 8;`（16 位→24 位左对齐），或直接照 `asio_list` 那样先换算到 24 位量程。

- 影响面：仅当 `g_aci[].type == ASIOSTInt24LSB`（值 17）时命中。作者本机日志是 `out ch0 type=18`（`ASIOSTInt32LSB`），所以从未触发，两份审计也都漏了。
- 建议：顺手在 `AsioTypeBytes` 校验处补一条 unit test 或断言，或者把 `WriteSamples`/`WriteTone` 合成一个共享实现——见下面 R-1。

---

## 5. 横切建议

### R-1（P2）四份重复实现已经开始漂移

`audio_hook.cpp` 与 `asio_list.cpp` 各自实现了一套：ASIO block 尺寸取整、采样格式→字节宽度、样本写入。已经出现的分歧：

| 项 | audio_hook | asio_list |
|---|---|---|
| `Int24LSB` 写入 | **错**（只留低 8 位） | 对 |
| `minBuffSize == 0` | 有 `mn<=0 → 16` 保护 | 无（靠 `block<=0 → 512` 兜） |
| `Int16MSB` / `Float64` | 通过校验后**输出静音** | 明确拒绝 |
| 无 `outputReady` 支持 | 记日志后用 0 | 同（但探测时机不同） |

建议抽一个 `asio_common.h`（`AsioBlockSize()`、`AsioTypeBytes()`、`WritePcm()`），两个 .cpp 共用。这样 R-1 里那张表的分歧就没法再出现，`Int24` 那种事也不必靠人眼比对发现。

### R-2（P3）三个工具都有 `ContainsNoCase` / `WideToUtf8` 的私有一份

语义一致，风险低；但如果以后要支持通配符或正则，会三处改三遍。同 R-1 一并收进 `common.h`。

### R-3（P2）三个工具都没有自动化回归

`excl_rate_probe` 的结论依赖真实设备，可以只用 `--list` + 非法参数做冒烟测试（退出码 2/0）；`session_tool` 可以对着自己的会话做 `--mute/--unmute` 往返；`asio_list --test "*"` 需要响度，不适合 CI。至少把"参数解析 → 退出码"这一层做成断言，能挡住 EP-4 这类回归。

---

## 6. 修复优先级

| 顺序 | 条目 | 理由 | 成本 |
|---|---|---|---|
| 1 | **第 4 节 Int24** | 真实的音频缺陷，只是本机没触发；一行 | 1 行 |
| 2 | **AL-1** | 能把进程打死，且 README 明说不该发生；port 一次 `g_asioBusy` | ~20 行 |
| 3 | **EP-1** | 会让"独占能不能用"的结论系统性偏悲观 | ~10 行 |
| 4 | **EP-2 / EP-3** | 测量器的读数偏差与归因错误 | 各几行 |
| 5 | **EP-4 / ST-1 / ST-2 / AL-2 / AL-3** | 文档与行为不一致、可无损改善 | 各 1-5 行 |
| 6 | R-1 / AL-4 / ST-3 / EP-5 | 结构性改善 | 中等 |
| 7 | 其余 P3 | 打磨 | 低 |

---

## 7. 一句话总结

三个工具的错误分支写得比多数同类工具认真，问题都不在主路径：`asio_list` 缺一个回调在飞保护（而隔壁 `audio_hook` 已经有了），`excl_rate_probe` 的候选格式表和 written 回退会让"独占可用性"的结论偏悲观，`session_tool` 的报告写法和进程名编码还没跟上它自己给设备名定下的 UTF-8 标准。真正要尽快改的是第 4 节那条 **24-bit 写入**——它不在这次审查的三个文件里，但它是两份实现分叉后留下的实伤。
