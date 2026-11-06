# Chunithm(SDHD 25000 / Fitouch2 整合包)输入秒级延迟 — 分析与修复报告

日期:2026-11-05
环境:Windows,`chuni_Fitouch2/SDHD25000_0801_0801_FI2/bin`(chusanApp.exe 32 位
+ amdaemon.exe 64 位 + segatools 系 hook + Fitouch2 控制板)
产物:`nicochuniio/` 源码工程与重建版 `fitouch2.dll`

---

## 1. 问题描述

| 项 | 内容 |
|---|---|
| 现象 | 选歌菜单等界面**偶发**输入响应变成可感知的秒级延迟 |
| 设备 | Fitouch2 平面触摸琴键(32 格,可滑动)+ 手势高度感应区 |
| 对照 | 游戏外用厂商控制面板测同一设备为毫秒级;游戏内 CPU/GPU 未满、帧率正常 |
| 后续补充 | 发作后**无法自愈**,直到重启游戏;琴键与高度感应**同步**变慢 |

## 2. 输入链路(逆向还原结论)

```
Fitouch2 控制板
  ├─ USB CDC 串口(VID_1B4F/PID_8D21,备选 VID_1145/PID_0003|0005,默认 COM1@115200)
  │    每帧 = FF 01 21 + 32字节触摸 + 1字节高度 + 校验和   ← 琴键和高度在同一帧!
  │    主机→板:LED 报文(FF 02 61 + 0x64 前缀 + 96字节 RGB,每帧一次)
  └─ HID 接口(VID_0CA3/PID_0021):64 字节 io4 报表(光束开关位)
        ▼
fitouch2.dll(nicochuniio,MinGW/32 位,加载在游戏进程内)
  ├─ 滑条线程:TIME_CRITICAL,QPC+pause 忙等"0.8ms"限速(实际失效)
  ├─ io4 线程:TIME_CRITICAL,0.5ms,HID OVERLAPPED 非阻塞轮询
  └─ LED 写:由游戏线程经 hook 转发,持 serial_mutex 同步 WriteFile
        ▼
chusanhook_x86.dll(segatools):滑条虚拟 COM 代理 + io4 JVS 代理
        ▼
chusanApp.exe
```

要点:
- **琴键(触摸)与高度感应走同一条串口流、同一个解析线程** → 任何串口侧
  堵塞两者必然同步变慢(与用户观察吻合)。
- amdaemon/hinata.dll 只负责刷卡等,**不在游玩输入路径上**。
- chusanhook 按名解析 10 个 `chuni_io_*` 导出(API 0x0102);DLL 共 61 个导出。
- 配置读 `.\segatools.ini`,段名 `[nico]`/`[io3]`/`[gfx]`(键名见 `src/core/config.c`)。

## 3. 根因

### 3.1 主病灶:LED 写 → 输入死亡的正反馈环(解释全部症状)

1. 游戏每帧经虚拟 COM 发 LED 更新,hook 的 `slider_req_set_led` **在游戏的
   滑条 COM IO 线程上**调用 `fitouch2!chuni_io_slider_set_leds` →
   `f_slider_led_report` → `send_packet` → 同步 `WriteFile`;
2. 原版写超时 = `2×N+1 ms`(N≈101~200 字节)≈ **200~400ms/次**;
3. 板子一旦停止接收(诱因:菜单静置 → 板子静默 → USB 选择性挂起唤醒/
   固件打嗝),4096B 的 TX 缓冲不到 1 秒被每帧 LED 包填满 →
   此后**每次写都吃满超时并失败**;
4. 失败后**无任何恢复动作**(不 close、不 PurgeComm、不重连)——RX 方向
   触摸帧照常流动,连 2 秒看门狗都不会触发 → **状态永久化**;
5. 被拖死的正是服务触摸扫描(auto-scan)的游戏 IO 线程;同时
   fitouch2 的输入读线程抢 `serial_mutex` 只有 5ms 预算,面对 200~400ms
   的持锁写几乎必输 → 后台触摸/高度帧也停止更新。

结果:游戏画面/帧率正常、CPU/GPU 空闲,但**琴键+高度全部**呈现秒级
延迟且无法自愈——与全部观察吻合。

### 3.2 次要因素(按影响排序)

| # | 机制 | 影响 |
|---|---|---|
| a | 滑条/io4 两个 TIME_CRITICAL 线程忙等限速失效(1000 迭代 cap≈几十 µs ≪ 0.8ms),全速热轮询 | 常驻烧 ~2 个核 |
| b | 2 秒帧头看门狗:菜单静置(板子静默)时每 ~2.8s 关端口+枚举+重开+复位板子;静置后首触可能撞上复位周期 | 间歇性额外延迟、诱发 USB 挂起 |
| c | `\\.\pipe\chuni_led` LED 管道:LED 线程**持 led_mutex 阻塞 WriteFile**,游戏线程 INFINITE 等锁 | 外接 LED 工具停滞时全输入冻结(本包默认无消费者,条件性) |
| d | `pack_data` 栈缓冲 ~128B < 转义后 LED 帧(可达 ~198B) | 高亮图案时越界写,潜在踩栈/帧损坏 |
| e | 重连死区默认 800ms | 断线恢复慢 |

### 3.3 已排除的假设

- **接收积压(backlog)**:消费能力(≥1250fps)远超板子上报率;即使异常,
  缓冲窗上限 ~5120B≈140 帧,且有超限丢弃——最多百毫秒级,不可能秒级;
- **amdaemon/网络阻塞**:输入不经过 amdaemon;
- **CPU 饥饿拖垮游戏线程**:会掉帧,与"帧率正常"不符;
- **Timer resolution / Sleep 粒度**:全部时序用 QPC,与系统定时器精度无关。

## 4. 修复动作(时间线)

| 阶段 | 动作 | 产物 |
|---|---|---|
| 1 | 逆向:fitouch2.dll / chusanhook_x86.dll(原版自带 COFF 符号 + DWARF,函数/参数/结构/默认值全量还原) | 分析结论(本文) |
| 2 | 第一版止血:二进制补丁 7 字节(写超时、读阻塞、跳过读锁、HID 等待、看门狗预算) | `../fitouch2.dll.patched`(已由阶段 3 取代,保留可对照) |
| 3 | 源码级重建:`nicochuniio/` 工程,MinGW-w64 i686 GCC 14 编译,逻辑 1:1 复原 | `src/`(10 个 .c)+ `nicochuniio.def` + `build.sh` |
| 4 | 修复落源码:FIX-1~6(见 `FIXES.md`) | 同上 |
| 5 | 新增事件日志:FIX-7,写 `bin\nicochuniio.log` | `src/utils/log.c` |
| 6 | 验证:导出表 61/61 一致(cdecl/i386);协议回环测试 9 项全过(帧解析/转义/校验重同步/命令编码/LED 转义/超限丢弃/0xEE/0xF0);open_port 超时字节反汇编确认 | 测试源码见对话记录,可重复 |

### 修复内容一览(与原版行为的差异)

| 修复 | 位置 | 变更 | 目的 |
|---|---|---|---|
| FIX-1 | open_port | 写超时 `2N+1 → 1ms` | 消除"LED 写堵死→输入永久变慢"主病灶 |
| FIX-2 | open_port | 读阻塞到首字节/2ms | 输入线程内核睡眠,不空转 |
| FIX-3 | data_reader | 读缓冲不取 serial_mutex | 输入读与 LED 写互不阻塞 |
| FIX-4 | process_io4_data / HID layout | 恢复原版非阻塞读取,固定 64 字节 HID 布局与 switches 偏移 29 |
| FIX-5 | serial_read_cmd | 帧头看门狗 2s → 20s | 保留死端口自救,消除静置期每 2.8s 端口复位 |
| FIX-6 | pack_data | 发送缓冲 0x100 + 上限丢弃 | 消除转义溢出踩栈(附带发现的原版缺陷) |
| FIX-7 | 新增 log.c | `nicochuniio.log` 事件日志 | 看门狗/重连/写失败可观测 |

## 5. 部署与回退

```
1. bin\fitouch2.dll  →  改名 fitouch2.dll.orig(备份)
2. 复制 nicochuniio\out\fitouch2.dll 到 bin\
3. segatools.ini 追加(可选但推荐):
     [nico]
     autoReconnect=1
     reconnectInterval=100
4. Windows 电源:禁用"USB 选择性暂停";设备管理器中该路径所有
   USB Hub 取消"允许计算机关闭此设备以节约电源";板子直插主板后置口
回退:把 fitouch2.dll.orig 改回原名即 100% 还原
```

实时日志:`Get-Content .\nicochuniio.log -Wait -Tail 20`

## 6. 验证状态与待办

**已完成(构建侧)**
- 导出表 61/61 与原版一致;i386;导入集合一致且不再依赖 `libgcc_s_dw2-1.dll`;
- 协议层回环测试 9/9 通过(真实 packet_solver.c + 桩环境);
- open_port 超时常量反汇编确认。

**待真机验证(需要游戏与板子)**
- [ ] 菜单静置 10 分钟:chusanApp CPU 占用应明显下降(两个 100% 线程消失);
- [ ] 静置后首触:应立即响应;
- [ ] 游玩 10 分钟:触摸/滑动/高度感应/琴键灯效正常;
- [ ] 观察日志三问:① 有无 `WATCHDOG` 行(有 = 板子静默属实);② 有无
      `serial write FAILED`(原死锁前兆,现应自动 `recovered`);③ 是否出现
      第二个 PID 反复 `open FAILED err=5`(= 串口被其他程序占用的新线索)。

**若替换后仍有发作(按顺序排查)**
1. 日志中 `write FAILED` 是否出现后立即 `recovered` → 若是,板子 RX 侧仍有
   周期性停滞,优先查 USB 供电/ hub / 线材;
2. 仅高度感应变慢而琴键正常 → HID 侧问题,查 VID_0CA3/PID_0021 设备的
   电源管理;
3. 外接 LED 工具是否在跑 → 关闭或 `controllerLedOutputPipe=0`/`cabLedOutputPipe=0`;
4. 以上都不是 → 回退 `.orig` 确认与重建版相关后再深入。

## 6.1 源码重建版首次部署失败的事后分析(2026-11-05)

用户实测:二进制补丁版有效(输入恢复正常),**源码重建版输入完全无响应**。
定位到重建版 `GetSerialPortByVidPid` 中两处手写 Win32 API 参数错误:

1. `SetupDiOpenDevRegKey` 参数顺序错误:HwProfile 传了 `DIREG_DEV`(1)、
   KeyType 传了 `KEY_READ`(0x20019,非法)、samDesired 传了 0(无访问权)。
   正确为 `(…, DICS_FLAG_GLOBAL, 0 /*当前硬件配置*/, DIREG_DEV, KEY_READ)`。
2. `SetupDiGetClassDevsA` 标志写成 `DIGCF_PRESENT|DIGCF_PROFILE`(0xA),
   原版为 `DIGCF_PRESENT|DIGCF_ALLCLASSES`(6),且 ClassGuid=NULL 时
   缺 ALLCLASSES 调用直接失败。

两处叠加 → VID/PID 枚举永远失败 → 回退默认 `\\.\COM1` → 板子不在
COM1 → 无数据、重连也永远失败(现象:输入全死、无法自愈)。
二进制补丁版保留原版正确的枚举代码,故不受影响。

教训:协议层回环测试覆盖不到 Win32 集成层;此类调用必须逐参数对照
原版反汇编核验。已对重建版全部 API 调用做了一轮穷举核对(见
`serial_port.c`/`io4_port.c`,其余均与原版一致),并已在成品反汇编中
确认修复字节。重建版的 `nicochuniio.log` 本可提前暴露此问题
(会显示端口从未成功打开)。

### 6.2 严格静态比对(第二轮,修复后)

对修复后的重建版做了三层比对:

1. **Win32 调用审计**(逐参数对照原版反汇编):穷举核对后新增修复
   三处遗漏——`send_packet` overlapped 超时路径缺 `CancelIo`、
   `chuni_io_slider_stop` 缺 `DeleteCriticalSection(&cs)`、错误码常量
   因 MinGW 头文件把 ERROR_IO_INCOMPLETE/PENDING 定义互换(996/997)
   而编成了 0x3E4(原版检查 0x3E5),已改用字面值;
2. **GetHidDevicePathByVidPid 逐行比对**(此前唯一未逐行核对的函数):
   与原版语义一致(cache 3s → HidD_GetHidGuid → 设备接口枚举 →
   detail(cbSize=5) → 探测打开 → HidD_GetAttributes → "VID_%04X" 匹配);
3. **协议常量全量核对**:帧结构/命令字/转义/校验和种子/超时/预算/
   优先级/队列深度/缓存 TTL 全部与原版一致。

残留差异仅为:文档化的 6 处修复、log.c 新增、编译器版本代码gen差异
(GCC 14 vs 8.1:寄存器分配/内联策略)。字节级一致对 fitouch2 不可达
(原始源码文本不存在,重建非逐字誊抄)。

### 6.3 chusanhook 字节级比对结论

使用原版编译器(Ubuntu 24.04 mingw GCC 13.2.0-win32,官方 .deb 解包,
producer 字符串完全一致)重编 vanilla beb54c5 后逐函数字节比对:
ImageBase 一致(0x6a4c0000)、导出一致,但读卡/LED 栈的大量静态函数
在原版中独立存在、在 vanilla 构建中被 GCC 内联进 dispatch(符号表
可证),且原版 sg-nfc.c 比上游多约 800 行——**证明厂商源码对这些文件
做了分发表结构重构(配合 hinata),属私有代码,chusanhook 的字节级
一致不可达**。详见 `upstream/segatools-patches/README.md`。

## 7. chusanhook 审计与源码重建(第二轮)

### 7.1 来源考证
原版 chusanhook_x86/x64 为 **TeamTofuShop/segatools**(Tendokyu)构建,
基点提交 `beb54c5`(2026-08-10)。证据:45+ 个源文件与该提交逐函数
**行号完全一致**;编译环境为 Ubuntu 24.04 发行版工具链
(GCC 13-win32, mingw-w64 11.0.1-3build1, meson)。
厂商私有改动集中于读卡器栈:`aime-dll.c`(+7 行)、`sg-nfc.c`(约 +100 行,
含 mifare/felica 轮询)、`sg-reader.c`(约 +340 行)、`aimeio.c`(约 +200 行),
以及 `misc/nusec/ewf/clock/amvideo` 等平台文件的零散插入。

### 7.2 审计发现(对照上游源码逐条核实)

| 发现 | 影响 | 状态 |
|---|---|---|
| **H1** `games/chuniio/pipeimpl.c`:LED 管道线程持 `pipe_write_mutex` 做阻塞 `WriteFile`,游戏线程 `led_pipe_update` 以 INFINITE 等锁;LED 客户端停滞 → 调 LED 的线程全部冻结 | 条件性(需外接 LED 客户端) | **本包中休眠**:加载 fitouch2 后 hook 自身 LED 管道不初始化(led_output_init 仅键盘模式调用),led15093 hook 也因 fitouch2 导出 led_init/led_set_leds 而被跳过 |
| **H2** 管道名 `\\.\pipe\chuni_led` 固定,同一 hook 套件运行于游戏与 amdaemon 两个进程,后创建者 `CreateNamedPipeW` 失败 → `continue` 热循环烧一个核 | 条件性(同 H1 前提) | 同上休眠 |
| **H3** `aime_dll_init`:aimeio DLL 加载失败 = 致命 ExitProcess。本包 `[aimeio] path=hinata.dll` 为 x64,x86 游戏进程必然加载失败 | **启动阻断** | 厂商已打补丁(+7 行降级到内置读卡器)——这解释了原版能启动、且 hinata 实际运行于 amdaemon 的 x64 hook 中 |
| 滑条/io4 IRP 处理(`slider_handle_irp*`/`io4_*`) | 无缺陷:锁内只做解码/分发,慢操作(fitouch2 写串口)已被我们的 FIX-1 限制在 ~1ms | 干净 |
| amdaemon 侧 chu2to3 代理会派生**第二个 fitouch2 实例** | 经分析该实例只服务 io4 jvs_poll(HID)与 LED,不打开 CDC 串口,与游戏侧实例无端口冲突 | 无碍 |

### 7.3 chusanhook 源码重建(完成)
- 上游源码:`upstream/segatools`(git, Tendokyu),固定 worktree:
  `upstream/segatools-pinned` = beb54c5 + 全部复原补丁;
- 补丁:`upstream/segatools-patches/nicochuniio-chusanhook-fixes.patch`
  (305→~590 行,7 个源文件,含全部厂商差异复原):
  - H1 管道锁重构、H2 防热旋、H3 aimeio 降级(H3 为开机必需);
  - 厂商读卡栈扩展:sg_nfc_poll_mifare(第三段 mifare 轮询回退)、
    sg_reader_nfc_get_mifare_block(mifare 块读转发)、
    aime_dll 结构体扩展(get_mifare_block @+0x10、felica_transact @+0x2c)、
    aime.c 去除 warning dprintf;
- 构建:原版编译器(GCC 13.2-win32)meson 交叉编译,导出 43/43 一致;
  产物 `upstream/segatools-pinned/out/chusanhook_x86.dll` / `chusanhook_x64.dll`;
- **已知唯一残留差异**:d3d9.c 为上游精简版(468 行)而非厂商的
  旧版完整 COM 包装(约 2200 行,来自已下架的原始 acetace/segatools,
  不在任何公开 git 历史中)。对 fullscreen 模式(windowed=0)影响最小;
  如果出现渲染异常,保留原版 chusanhook 二进制即可。

### 7.4 部署组合建议
| 组件 | 推荐 | 备选 |
|---|---|---|
| fitouch2.dll | 源码重建版(枚举修复+全部增强) | 二进制补丁版(已验证有效) |
| chusanhook_x86/x64 | 源码重建版(读卡栈复原+全部修复) | 原版二进制(厂商私有 d3d9.c) |
| segatools.ini | 加 [nico] 段(reconnectInterval=100) | — |
| Windows 电源 | 禁用 USB 选择性暂停 + Hub 节电 | — |

**最简验证步骤**:备份原版 → 替换全部重建版 → 测开机/刷卡/输入/灯效 →
任何异常时逐个切回原版定位。

## 7.1 chusanhook 厂商差异复原(进行中,2026-11-05 第二轮修正)

此前"字节级一致不可达"的结论**撤回**——那是被"原型行 vs 定义行"
污染的比对结果。精确对齐后,厂商差异的真实规模:

- **全部平台/hook 文件与 beb54c5 逐行一致**(config/epay/misc/nusec/
  ewf/clock/amvideo/reg/path/dllmain/slider/io4/chuniio/pipeimpl/
  chu2to3 等约 40 个文件零差异;此前的 +386/+766 等全是拿原型行
  比定义行的假阳性);
- 真实差异集中在**读卡栈**,且规模有限:
  - `sg-nfc.c`:统一 +52 行偏移 + 新函数 `sg_nfc_poll_mifare`(236B)
    —— 已反汇编复原(见 `segatools-patches/vendor-sg-nfc-mifare-draft.c`):
    厂商把轮询路径改为从 aimeio 后端直接读取 32 字节实体卡 mifare
    数据(hinata 提供真实卡镜像),写入 IC 模拟器,替代上游的
    aime_card_populate 合成卡;轮询应答仍用固定 UID;
  - `sg-reader.c`:新函数 `sg_reader_nfc_get_mifare_block`(52B,把
    mifare 块读转发给 aimeio 后端)+ 若干 ±5~+17 偏移;
  - `aimeio.c` ±5 行、`aime.c` ±1 行、`aime-dll.c` +7 行(降级补丁已移植);
  - `dprintf.c` = 上游 4864288 修复前的版本(用上游历史提交即可);
  - `d3d9.c`:整个文件来自另一个 segatools 世代(约 2200 行完整
    IDirect3DDevice9 包装;上游历史从未超过 468 行)——需要从
    Tendokyu 的旧版 fork(如原版 acetace 系)取源;
  - 其余内联差异机制已查明:厂商改动使处理函数变为多调用点,
    同编译器(-O2)下不再被内联,故原版符号表中保持独立。

- 复原方法论已闭环:原版编译器(GCC 13.2-win32,官方 .deb 解包)
  + 逐函数反汇编对照 + 字节级验收(注意先掩去重定位与相对位移)。

**aime_dll 绑定表已完全解码**(原版 .rdata 主表 0x6a59f860 ×17 +
v1.01 扩展表 0x6a5a0cf8 ×1):厂商的 aimeio API = 上游**删除
`nfc_poll`、新增 `nfc_get_mifare_block`**(+0x10,v1.01 扩展绑定,
API ≤0x100 时置 NULL);`nfc_get_aime_id` 上移到 +0x08 且签名扩为
读 32 字节实体卡镜像。复原的两个新函数 C 代码见
`segatools-patches/vendor-sg-nfc-mifare-draft.c`(含完整反汇编
对照注释)。剩余:调用点细节 + aime.c/aimeio.c ±行 + d3d9.c
旧版溯源 + 字节验收循环。

## 附录 A:fitouch2.dll 关键函数地址(原版 VA,ImageBase 0x68040000)

| VA | 函数 | 备注 |
|---|---|---|
| 0x680414c0 | chuni_io_get_api_version | 返回 0x0102 |
| 0x680414c6 | chuni_io_jvs_init | 建两把 mutex + timeBeginPeriod(1) |
| 0x68041625 | chuni_io_jvs_poll | 模式选择(ir=0xAA→io4 HID) |
| 0x6804187e / 0x680418b5 | slider_init / slider_start | 起滑条线程,TIME_CRITICAL |
| 0x68041aae | chuni_io_slider_thread_proc | 0.8ms 忙等 + 每圈一次 serial_read_cmd |
| 0x68041d25 | chuni_io_io4_thread_proc | 0.5ms,HID 非阻塞轮询 |
| 0x68041f7f | io4_JVS_FSM | 500ms 限速开 HID + 起 io4 线程 |
| 0x68043de0 / 0x68043eed | data_reader / serial_read_cmd | 1KB 环形缓冲 / FSM+校验和+看门狗 |
| 0x68043c74 / 0x68043a6e | pack_data / send_packet | 转义+校验和 / mutex+WriteFile |
| 0x68042dd8 | open_port | COMMTIMEOUTS 配置点(补丁落点) |
| 0x68044317 | report_init | Sleep(5)+复位+使能(重连即复位板子) |
| 0x6804425c | f_slider_led_report | LED 报文(cmd 2,len 0x61) |
| 0x6804414c | output_packet | 命令分发 + Sleep(1)/Sleep(10) |

关键全局:`serial_mutex@af5c`、`timeouts@af64`、`comPort@af78`、
`hPort@6008`、`AIR_status@a080`、`LED_status@a0c0`、
`io4_received_data@ae00`、`chuni_io_cfg@aec0`(+96 = reconnectInterval)。

## 附录 B:nicochuniio/ 交付清单

```
ANALYSIS.md            本报告
FIXES.md               与原版的 6 处差异详述
README.md              构建/部署/回退/日志说明
build.sh               构建脚本(i686-w64-mingw32-gcc)
nicochuniio.def        61 个导出
out/fitouch2.dll       重建产物
src/
  core/chuniio.c           API 入口 + 滑条/io4 线程
  core/config.c            配置加载
  core/input_packet.c      串口流/HID 流消费
  feat/cursor.c            指针归位
  hardware/serial_port.c   CDC 串口
  hardware/io4_port.c      HID
  protocol/packet_solver.c 组帧/解帧/FSM
  protocol/slider_protocol.c 命令构造
  utils/queue.c            环形队列(兼容导出)
  utils/log.c              事件日志(新增)
  dllmain.c
  nicochuniio.h
```

相关文件(工程外):
- `../fitouch2.dll.patched`:阶段 2 的二进制补丁版(7 字节差异,已弃用但可对照);
- 原 DLL:`chuni_Fitouch2/SDHD25000_0801/SDHD25000_0801_FI2/bin/fitouch2.dll`(未改动)。
