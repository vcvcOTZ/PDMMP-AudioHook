# Audio Hook

> 《初音未来 Project DIVA Mega Mix+》（Steam）的低延迟音频模组。

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Game](https://img.shields.io/badge/game-PDMM%2B-7b5cff)
![Version](https://img.shields.io/badge/version-v13.5-informational)
![License](https://img.shields.io/badge/license-MIT-blue)

把游戏的音频**客户端**（不是代码）整个换掉，让引擎缓冲从链路里消失，再把游戏的 PCM 通过你选择的后端渲染出去：**ASIO**、**WASAPI 独占**或 **WASAPI 共享**。

只打 **vtable** 补丁（COM 数据层）——可执行文件逐字节原封不动，所以游戏的反篡改（Denuvo）不会被触发。

> 语言：[English](README.md) · 简体中文

---

## 目录

- [为什么需要它](#为什么需要它)
- [特性](#特性)
- [实测缓冲大小](#实测缓冲大小)
- [环境要求](#环境要求)
- [安装](#安装)
- [配置](#配置)
- [输出后端](#输出后端)
- [工作原理](#工作原理)
- [故障排查](#故障排查)
- [可移植性](#可移植性)
- [配套工具](#配套工具)
- [从源码构建](#从源码构建)
- [已知限制](#已知限制)
- [更新日志](CHANGELOG.md)
- [许可证](#许可证)

---

## 为什么需要它

PDMM+ 使用 **CRI ADX2** 音频栈，开了两条带超大缓冲的 WASAPI 共享流：

| 流 | 内容 | 格式 | 引擎授予的缓冲 |
|--------|---------|--------|----------------------|
| stream1 | 游戏音频 | 16-bit / 44.1 kHz | **44.5 ms**（请求 10 ms） |
| stream2 | 影片音轨 | 32-bit float / 44.1 kHz | **66.7 ms** |

44.5 ms 是由 `AUDCLNT_STREAMFLAGS_RATEADJUST` 标志造成的。只修剪它顶多压到平台共享模式的下限（多数驱动 22 ms），因为音频仍要穿过引擎缓冲。

v13 走了另一条路：**把 CRI 交给一个由本模组拥有的客户端对象。** 游戏混进我们的内存，引擎缓冲完全不在链路里，音频设备也彻底空出来，交给我们自己控制的后端。

---

## 特性

### 1. 客户端接管 *（始终生效，`fake_client = 1`）*

- 拦截 stream1 的 `IAudioClient::Initialize`，真正的 `Initialize` **从未被调用**。
- CRI 转而与我们的实现对话（`GetBufferSize`、`GetCurrentPadding`、`GetBuffer`、`ReleaseBuffer`、`SetEventHandle`、`GetService`、`Start`/`Stop`/`Reset`、…）。
- 结果：**没有 22 ms 引擎缓冲，也没有占用设备的 WASAPI 流。**
- CRI 由真实输出时钟驱动（`FakePoke()` 在输出回调里运行），生产者和消费者共用同一个时钟，不会漂移。

### 2. 三个输出后端（`output = …`）

| 后端 | 作用 |
|---|---|
| `asio` *（默认）* | 原生 ASIO 驱动。测试硬件上延迟最低、最稳定。 |
| `shared` | 我们自己开一条 WASAPI 共享流。**任何**设备都可用，无需 ASIO 驱动。 |
| `excl` | WASAPI 独占，驱动配合时为 5 ms 周期。带 **600 ms 自检**：设备交付不足实时 95% 时拆掉后端、回退到 `shared`。 |

`asio_driver = *`（默认）时，ASIO 后端按注册表顺序逐个尝试已注册驱动，保留第一个能完整初始化的——日志会写明每次尝试和最终选中的驱动，所以装了多个驱动的机器不会被悄悄路由到断开的或虚拟设备。探测会真的 `init()` 每个候选（上限 8 个），有些驱动在此过程中会弹控制面板或短暂抢占设备，所以装了多个 ASIO 驱动（ASIO4ALL、FlexASIO、虚拟声卡等）的机器，建议先跑一次 `asio_list`，把声卡名字填进 `asio_driver` 以跳过探测。

### 3. 反篡改安全

只打 vtable 槽位（数据层）：

- `IAudioClient` — `[3]` `Initialize`、`[4]`–`[12]`（`GetBufferSize` … `Reset`）、`[13]` `SetEventHandle`、`[14]` `GetService`
- `IAudioRenderClient` — `[3]` `GetBuffer`、`[4]` `ReleaseBuffer`

不写任何代码字节。被替换的是游戏自己的 **COM 实现**（由我们的客户端应答 `GetBufferSize` / `GetBuffer` / …）——这正是引擎缓冲脱离链路的原因——但没有修改任何可执行内存。

### 4. 健壮性

- **vtable 补丁全有或全无。** 客户端接管（9 个槽）和渲染客户端 tap（2 个槽）都作为一组安装；任一槽打不上，已写的槽会被回滚。半补丁表曾导致 `slot 3 = 我们的 hook` 而 "patched" 标志仍为 0，下一次尝试就把自己的函数 hook 到自己头上（栈溢出）。
- **带真实验证的分级回退：** `output = excl` → `shared` → ASIO。每一级现在都会*等待*其线程报告成功，`Start()` 失败不再被当成后端正常。
- **独占自检**（600 ms）同时检查已交付帧和**设备时钟**（`IAudioClock`）；低于实时 95% 就拆流回退。
- **fake 模式有真正的兜底：** 客户端被接管后若 ASIO 挂了，模组会启动 shared 后端（`WoStart(0)`），而不是旧的"游戏音频退回 WASAPI"——没有真实流时那是不可能的。
- **fake 模式无引擎交接：** 跳过 tap-and-zero 流程（重同步 + 23 ms 等待），切歌不再丢弃约 10 ms 已混好的音频。
- **环形缓冲无条件分配**，所有环形访问都有 NULL 守卫，所以 `fake_client = 1` + `asio = 0`（以前必崩）现在能工作，并自动纠正为 `output = shared`。
- **输出侧任意声道数**（2.0 / 5.1 / 7.1 设备），FL/FR 映射，其余声道静音。
- **部分写入：** 只释放环形缓冲里实际持有的帧数，引擎缓冲比 CRI 队列大的设备不会再每周期被补零。
- **有界等待：** tap 稳定等待 30 s 超时；独占回滚等待写线程真正退出（2 s），退不出去就泄漏而不是释放（不使用已释放内存）。
- **死掉的输出流会被重建：** 若 shared 后端的设备消失（拔耳机、默认设备切换、流被抢占），写线程把后端标记为失败而不是无声空转，看门狗会重启它（3 s 退避、最多 5 次、每次记日志）。
- **实时安全回调：** ASIO/WASAPI 回调内无文件 I/O、无内存分配；计数器由监控线程报告。
- **会话静音清理**现在遍历本进程自己的音频会话（`IAudioSessionManager2`），所以 stream1 被接管时也有效——以前会因 `AUDCLNT_E_NOT_INITIALIZED` 失败并静默什么也不做。
- **日志：** 每行一次原子写入、追加模式带轮转（`audio_hook.log.1`），崩溃那次运行仍然可读。

---

## 实测缓冲大小

客观数字，用 44.1 kHz 的 USB ASIO 声卡测得：

| 组件 | 数值 |
|---|---|
| 交给游戏的 CRI staging 缓冲 | 441 帧 = **10.0 ms**（`FAKE_FRAMES`） |
| ASIO 块大小 | 64 帧 = **1.45 ms** |
| ASIO 驱动报告的延迟 | 181 帧 入/出 = **4.1 ms** |
| WASAPI 共享引擎缓冲 | 970 帧 = **22.0 ms** @44.1 kHz（48 kHz 下 1056） |
| WASAPI 独占周期（该驱动授予） | 221 帧 = **5.01 ms** |

`asio` 和 `shared` 后端的稳态验证：25 s 运行中 **交付 44,100 帧/秒、0 欠载、0 丢帧**。

> 独占模式仅列出来供参考——见[已知限制](#已知限制)。它不是推荐路径。

---

## 环境要求

- **初音未来 Project DIVA Mega Mix+**（Steam）。
- **DML（Diva Mod Loader）**——`dinput8.dll` + `mods/<名>/config.toml`。
- *（仅 `output = asio` 需要）* 一个运行在 **44.1 kHz** 的 **ASIO 驱动**。

---

## 安装

1. 确认已装 DML（游戏根目录有 `dinput8.dll`）。
2. 把 `Audio Hook` 文件夹复制进 `mods/`：

   ```
   mods/Audio Hook/
   ├── config.toml
   ├── audio_hook.dll
   └── audio_hook.txt
   ```

3. 在根 `config.toml` 的 `priority` 列表里加上 `"Audio Hook"`：

   ```toml
   priority = ["Audio Hook", "Disable Watermarks", "High Frame Rate", "ExPatch"]
   ```

4. 启动游戏，查看 `mods/Audio Hook/audio_hook.log`——表头应显示 `v13.5` 和 `MODE: v13 takeover`。

---

## 配置

`mods/Audio Hook/audio_hook.txt`：

```toml
fake_client = 1
output      = asio
asio        = 1
asio_driver = *          # "*" = 自动（第一个可用驱动）；或填 asio_list 列出的名字
block       = 64
handoff_ms  = 0
mute_mode   = 1
```

| 键 | 默认 | 含义 |
|-----|---------|---------|
| `fake_client` | `1` | 用我们的客户端替换 CRI 的 stream1 客户端。任何非 ASIO 后端**必需**；否则退回旧的 v11 行为。 |
| `output` | `asio` | `asio` / `shared` / `excl`。 |
| `asio` | `1` | `output = asio` 时启用 ASIO 工作线程。 |
| `asio_driver` | `*` | 对 `asio_list` 列出的名字做大小写无关的子串匹配。`*`（或 `auto`、空）= **第一个能真正初始化的驱动**：逐个尝试已注册驱动，直到某个报告 ≥2 输出、44.1 kHz、可用采样格式。每次探测会调用驱动的 `init()`，可能弹控制面板或短暂抢占设备——装了多个驱动时，填 `asio_list` 给出的确切名字。 |
| `block` | `64` | ASIO 块大小（16…1024）。 |
| `handoff_ms` | `0` | 交接延迟；`0` = 自动。 |
| `mute_mode` | `1` | `1` = tap-and-zero 交接；`0` = 旧版 `SetMute`。 |

---

## 输出后端

### `asio`（推荐）

在 **STA** 工作线程上加载 ASIO 驱动，直接从我们的客户端 tap CRI 的 PCM，中间用一个无锁环形缓冲播放出去。ASIO 回调也驱动 CRI（`FakePoke`），两者永不漂移。

### `shared`（无需 ASIO 驱动）

在默认端点上开一条普通 WASAPI 共享流（混音格式，float32 或 16-bit），写入环形缓冲当前持有的帧。24-bit packed 或 32-bit *整数* PCM 混音格式会被拒绝（日志写 `refusing`，模组回退到 `asio`）——按 int16 写会变成满幅噪声。引擎的约 22 ms 缓冲仍在链路里——那是平台地板，不是客户端能压缩的——但去掉了 CRI 自己的缓冲，且任何设备都可用。

如果端点**不是 44.1 kHz**（48 kHz HDMI / 蓝牙 / 板载很常见），后端会用线性插值把 44.1 kHz 重采样到设备速率（约 1 帧延迟，这个比率下听不出来），而不是把整局游戏放快 8.8%。日志行显示 `resample=yes`。

### `excl`

以 `duration == periodicity`（5 ms）加事件驱动回调打开 `AUDCLNT_SHAREMODE_EXCLUSIVE`。因为独占模式绕过引擎，设备必须空闲——而 stream1 不再开真实流，所以它是空闲的。

难点在驱动质量。目前测过的设备里：

- 一块 USB 声卡——以 221 帧初始化，但**设备时钟只推进到额定的 88.5%**；
- 一块板载 codec——以 480 帧 @48 kHz 初始化，但**设备时钟几乎停在 ~0.3%**。

两者都会饿死队列、让音乐卡顿，所以 v13 启动时测 600 ms，速率低于 95% 就回退到 `shared`。回退之后，本次进程生命周期内独占**不再重试**——看门狗只重建 `shared`——所以想让 `output = excl` 生效需要重启游戏。用 `tools/excl_rate_probe.exe` 检查你的设备：

```
excl_rate_probe --list
excl_rate_probe "Speakers" --seconds 2
```

---

## 工作原理

```
CRI ADX2 混音 ──▶ [我们的 IAudioClient]  (GetBuffer / ReleaseBuffer, 441 帧)
                        │
                        ▼
              [无锁环形缓冲 65536 帧]
                        │
        ┌───────────────┼────────────────┐
        ▼               ▼                ▼
   ASIO 驱动      WASAPI 独占      WASAPI 共享
        │               │                │
        └────── FakePoke() 从输出时钟驱动 CRI ──────┘
```

1. 拦截 stream1 的 `Initialize`；跳过真正的那个，装上我们的 vtable 槽位。
2. `GetService(IAudioRenderClient)` 返回我们的渲染客户端；`Start` 启动驱动线程。
3. 每个输出回调从环形缓冲取数据，队列快空时给 CRI 的事件发信号，让它混下一块。
4. 因为信号来自真实输出时钟，生产和消费被锁在一起——没有时钟漂移（`shared` 后端只在把 44.1 kHz 转成设备速率时才重采样，从不追赶漂移）。
5. 监控线程每秒报告一次回调数 / 欠载 / 队列深度。

---

## 故障排查

| 症状 | 处理 |
|---------|-----|
| 完全无声 | 查日志里的 `[fake] stream1 client replaced` 和 `[asio] started` 或 `[wo] output running`。若某后端失败，模组会记录它回退到哪一级。 |
| 声音卡顿 / 重复 | 只会在 `output = excl` 且驱动很烂时出现；v13 会自检并回退。设 `output = asio`（或 `shared`）。 |
| 禁用模组后无声 | 运行 `session_tool.exe --unmute DivaMegaMix`（旧版会留下持久会话静音）；v11.2+ 会自动清除。 |
| 启动几秒后静默退出 | **游戏自身的 bug**（前台窗口检查 → `WinMain` 提前返回）。启动期间保持窗口聚焦，或装社区补丁 `MegaMixPlus-BackgroundLaunchFix`（`d3d11.dll` 代理）。 |
| 全部禁用 | 在 `audio_hook.dll` 旁边放一个空文件 `audio_hook.off`。 |
| 回到旧行为 | 设 `fake_client = 0` 和 `asio = 0`：纯 v11 WASAPI 缓冲优化。 |

日志：`audio_hook.log`（主）、`audio_hook_detach.log`（detach 诊断）。

---

## 可移植性

- **任何设备、无 ASIO 驱动：** `fake_client = 1`、`output = shared`。
- **ASIO 声卡：** `fake_client = 1`、`output = asio`，`asio_driver` 留 `*`（自动）或填 `asio_list` 报出的名字。声卡必须跑在 44.1 kHz；否则桥接拒绝并回退。
- **独占实验：** `output = excl`——先跑 `excl_rate_probe`；设备报不到 ≈100% 的话，模组反正会回退。
- 多声道输出（5.1/7.1）已处理：FL/FR 映射，其余声道静音。

---

## 配套工具

模组附带三个控制台小工具，在 `tools/` 里（也附在每个 [Release](https://github.com/vcvcOTZ/PDMMP-AudioHook/releases)，或用 `src/build.bat` 构建）。它们都不碰游戏——是只读诊断，除特别说明外可在游戏运行时安全使用。

| 想知道… | 运行 |
|---|---|
| 哪个 ASIO 驱动是哪个、`asio_driver` 该填什么 | `asio_list` |
| 我的设备上 WASAPI 独占到底能不能用 | `excl_rate_probe` |
| 游戏为什么无声（会话静音 / 无峰值） | `session_tool` |

### `asio_list` —— 识别并试听 ASIO 驱动

```
asio_list                          交互菜单（方向键）
asio_list --list                   纯文本列表，无菜单
asio_list --test "USB ASIO"        直接在该驱动上播放测试音
asio_list --test "*"               在第一个已注册驱动上播放
asio_list --help
```

退出码：`0` 已播放 · `1` 无驱动注册 · `2` 无匹配 / 播放失败 · `3` 命令行错误 · `4` 驱动崩溃。

交互菜单列出 `HKLM\SOFTWARE\ASIO` 下注册的每个驱动；**↑/↓** 移动选择、**回车**在其上播放 3 秒 440 Hz 测试音、任意键停止、**Q** 退出。

```
=== ASIO Driver Test  (5 drivers) ===
  Up/Down: select   Enter: play 3 s tone   Q: quit

  [ 1] USB Audio ASIO Driver
  [ 2] Thunderbolt Audio ASIO
  [ 3] Onboard ASIO
  [ 4] Virtual Mixer ASIO       <- highlighted = selected
  [ 5] Guitar Multi-Effects ASIO
```

测试音刻意很轻：幅度 `0.12`、120 ms 淡入，不会轰你的音箱。**测试音从哪个音箱/耳机出来，那个驱动驱动的就是哪个物理输出**——这正是这个工具的全部意义。

把打印出的名字填进 `asio_driver`。匹配是**大小写无关的子串**搜索、取**第一个**命中，所以太短的字符串可能选错驱动（一台机器同时注册了 `Thunderbolt Audio ASIO` 和 `USB Audio ASIO`，写 `Audio` 会选中第一个）。始终复制完整名字。

匹配**不到任何东西是错误**（`exit 2`，会打印已注册的名字）：绝不会被悄悄重定向到第一个驱动——过去那样会让你"验证"错设备。

**加载或初始化**时崩溃的驱动（确实会发生）被 SEH 捕获并报告为 `driver crashed`，所以一个坏驱动不会阻止你测其余驱动。每次测试都关闭自己的流（`stop` → `disposeBuffers` → `Release`）并在释放缓冲前等音频回调离开，所以下一个驱动真的能拿到设备。（在其*自己的回调内部*出错的驱动跑在自己的线程上、不在我们的 SEH 框架内——那仍可能带崩进程；用户态下没有任何办法阻止。）

### `excl_rate_probe` —— 这台设备上独占模式是真的吗？

```
excl_rate_probe --list                       列出渲染端点，默认端点有标记
excl_rate_probe "<设备子串>" [--seconds N] [--shared]
```

默认值：**默认渲染端点**、**每种格式测 2 s**（`--seconds` 接受 1…60；坏值或未知选项是用法错误，不会被静默忽略）。它会尝试每种可行格式（44.1 / 48 kHz × 16 / 24 / 24-in-32 / 32-bit float），每种格式重新激活客户端，只有当**设备时钟真的在推进**才信任该格式——`Initialize()` 返回 `S_OK` 什么也证明不了。

退出码：`0` 测得 ≥ 实时 95% · `3` 低于该值（或都不行）· `1` 设备/COM 错误 · `2` 命令行错误。

```
-- 44100 Hz / 16 bit / 2 ch --
   initialized: buffer 441 fr (10.00 ms) - measuring...
   -> written 43773 fr/s, device clock 43950 fr/s

== SUMMARY 'Speakers (USB Audio)'
   best: 44100 Hz / 16 bit / 2 ch  buffer 441 fr  ->  99.7% of real time
```

| 行 | 含义 |
|---|---|
| `written … fr/s` | 工具真正推进缓冲的帧数 |
| `device clock … fr/s` | `IAudioClock` 的推进，换算成帧——**诚实的数字** |
| `(352800 units/s)` | 时钟自己的 tick 速率；用 `GetFrequency()` 归一化 |
| `not supported (0x88890008)` | 驱动在独占模式下拒绝该格式 |
| `Initialize 0x88890019` | 缓冲大小未对齐——跳过该候选 |
| `(clock not advancing)` | 时钟对象存在但位置从未移动——计 0% |

> 设备时钟**不是**按额定速率计采样数的：曾测到一条共享流报 **352800 units/s**（8× 44.1 kHz）。工具先除以 `GetFrequency()` 再转成百分比，所以 100% 才真正代表实时。若驱动没有可用的 `IAudioClock`，则改用写入速率，行里会注明。

怎么看摘要：

- **≥ 95%** —— 独占真的可用；`output = excl` 会保留它。
- **90–95%** —— 勉强：队列缓慢变空或变满，预期会有漂移/卡顿。
- **< 50%** —— 驱动接受了流但从未启动硬件时钟；该设备上无法用独占。

最后这种情况正是模组对 `excl` 跑 **600 ms 自检**、低于 95% 就回退 `shared` 的原因。`--shared` 在普通共享流上跑同样的测量，是最快证明某个端点还活着的方式。

### `session_tool` —— 会话静音 / 音量 / 峰值检查器

```
session_tool                    列出每个渲染设备的会话
session_tool --list             同上，显式
session_tool --unmute <子串>    对进程名匹配的每个会话清除静音
session_tool --mute <子串>      设置静音（仅诊断）
```

匹配**大小写无关**（`divamegamix` 可用），系统音会话（`pid 0`）从不被碰，模式匹配不到任何会话时退出码为 `2`——所以脚本能区分"已解除静音"和"游戏根本没在跑"。纯文本报告追加在 exe 旁边（`session_tool_report.txt`，4 MB 时轮转到 `session_tool_report.1.txt`），设备和进程名都按 UTF-8 打印，中文/日文名可读且仍可匹配。

```
== device 'Speakers (USB Audio)' [{...}]
    session pid=16672  DivaMegaMix.exe   [Active] muted=no volume=1.00 peak=0.4182
    session pid=13564  msedge.exe        [Inactive] muted=no volume=1.00 peak=0.0000
```

`peak` 是实时的 `IAudioMeterInformation` 值——最快区分"游戏有活动流但输出数字静音"（状态 `Active`、`peak=0.0000`）和"游戏根本没在播任何东西"。

**游戏运行时**再跑——会话只在此时存在。`--unmute DivaMegaMix` 修掉 v11.1 之前可能留下的按应用持久静音（Windows 按可执行文件路径记住该状态，模组卸载后依然存在）。

---

## 从源码构建

- **MSVC**（Visual Studio Build Tools）+ Windows SDK。
- ASIO SDK 头文件打包在 `src/asiosdk/` 里。
- 源码：`src/audio_hook.cpp`、`src/asio_list.cpp`、`src/excl_rate_probe.cpp`。
- `src/build.bat` 通过 `vswhere` 定位工具链并构建 DLL。

```
cl /nologo /utf-8 /LD /MT /O2 /W3 /I asiosdk /Fe:audio_hook.dll /Fo:obj\ audio_hook.cpp /link /MACHINE:X64
```

> 把生成的 `audio_hook.dll` 复制到 `mods/Audio Hook/`。

---

## 已知限制

- **WASAPI 独占依赖驱动。** 目前实测：一块 USB 声卡 **88.5%**、一块板载 codec **0.3%** 的实时速率。因此后端会自检并回退；它不是默认值、不是推荐路径。
- **独占自检跑在游戏的初始化线程上**，所以 `output = excl` 会给首次音频初始化加约 600 ms（并在 stream2 存在前占用设备）。实验可接受；默认不用。
- **影片音频（stream2）** 在 `asio`/`shared` 模式下仍走真实 WASAPI 流。`excl` 模式下设备被独占，stream2 初始化失败（`AUDCLNT_E_DEVICE_IN_USE`）、过场音静音——这对游玩无影响、符合预期。
- **共享模式无法低于引擎缓冲**（多数驱动约 22 ms）。那是平台地板，与本模组无关。
- `shared`/`excl` 只使用默认渲染端点。

---

## 许可证

[MIT](LICENSE) © 2026 vcvcOTZ
