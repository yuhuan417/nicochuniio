# FIXES.md — 与原版 fitouch2.dll 的行为差异

本工程是对 Fitouch2 `nicochuniio`(chuniio DLL)的源码级重建。
除以下 7 项修改外,所有逻辑、结构体、默认值、怪癖均按原版反汇编结果
1:1 复原(原版带 DWARF 调试信息,函数/参数/结构布局全部还原)。

## FIX-1 写超时 2N+1ms → 1ms(主病灶修复)
`open_port()`:`WriteTotalTimeoutMultiplier 2 → 0`。

原版:游戏线程每帧经 `chuni_io_slider_set_leds` → `WriteFile` 写约
101~200 字节的 LED 报文,写超时 2×N+1 ≈ 200~400ms。一旦板子停止接收
(TX 缓冲 4096B 被填满),每次写都吃满超时并失败,而且失败后没有任何
恢复逻辑 → 游戏的滑条 IO 线程被永久拖死,输入秒级变慢且无法自愈。
改为 1ms 后:堵死时写立即失败跳过,输入完全不受影响;板子恢复后
LED 自动恢复。

## FIX-2 读阻塞 2ms(消灭 CPU 空转)
`open_port()`:`ReadTotalTimeoutConstant 0 → 2`。

原版读是完全非阻塞的,滑条线程/io4 线程各自 100% 空转占满 2 个核
(忙等循环的 1000 次迭代上限远小于 0.8ms 目标,限速从未生效)。
改为"阻塞到首字节到达或 2ms"后,线程在内核里睡眠、数据到达即刻
唤醒——CPU 占用降到接近 0,输入延迟不变甚至更好。

## FIX-3 输入读不再抢 serial_mutex
`data_reader()`:补充读缓冲时不再获取 `serial_mutex`(原版最多等 5ms)。

串口读/写在驱动层是全双工的,原版却用一把锁把"输入读"和"游戏线程的
LED 写"串在一起。`read_pos/read_len` 只有滑条线程访问,开/关端口也在
同一线程,无竞争。这样 LED 写和输入读互不阻塞。

## FIX-4 已回退:恢复原版 AIR/HID 读取路径
`process_io4_data()`恢复为原版状态机:ReadFile 返回
`ERROR_IO_PENDING`(997) 时保留挂起状态,后续以
`GetOverlappedResult(..., bWait=FALSE)`轮询,仅在
`ERROR_IO_INCOMPLETE`(996) 时继续等待。AIR JVS 路径也直接读取
`io4_received_data.switches`,与原版相同。

原版 HID 结构使用 1 字节对齐,`switches` 位于字节偏移 29。重建版现已
显式固定为 64 字节布局,并用编译期断言检查尺寸和字段偏移。

## FIX-5 帧头看门狗 2s → 20s
`serial_read_cmd()`: FSM 总预算 2000ms → 20000ms。

预算超时返回 0xFF,上层会关端口重连——原版这是"静默看门狗"。但在
菜单静置(板上报表流停止)时,它导致每 ~2.8 秒一次完整的关端口/
设备枚举/重开循环。配合 FIX-2 改为 20s:保留死端口恢复能力,不再
频繁重置端口。

## FIX-6 pack_data 缓冲安全化(附带发现的潜在栈溢出)
原版 `pack_data` 把转义后的发送帧写入栈上约 128 字节的缓冲区,而
LED 帧(96 字节 RGB)在大量 0xFF/0xFD 字节时转义后可达 ~198 字节,
越界写会覆盖循环计数器与栈上保存的寄存器。原版靠 `send_packet` 的
0x80 上限事后丢弃溢出帧,但部分路径存在踩栈风险。

重建版:`send_packet` 保持 0x80 上限不变(兼容性),`pack_data`
缓冲扩为 0x100,超限帧干净丢弃,无未定义行为。可观察结果与原版
一致(超大 LED 帧不发送)。

## FIX-7 事件日志(重建版新增)
新增 `src/utils/log.c`:把关键状态转换写入 DLL 同目录的
`nicochuniio.log`(每行带本地时间与 PID)。记录的事件:

| 事件 | 含义 |
|---|---|
| `WATCHDOG: no frame header for >20000 ms` | **看门狗触发**,即将关端口重连 |
| `board sent error frame (0xFF)` | 板子主动报错帧 |
| `serial open state lost (comm error/closed)` | RX 出错/句柄失效被 100ms 巡检发现 |
| `serial open FAILED: ... err=N` | 开端口失败(err=5 通常是端口被占用) |
| `serial port opened / closed` | 端口开关 |
| `reconnect attempt #N` / `reconnect OK` | 重连节流与结果 |
| `serial write FAILED ... board RX wedged?` | **写失败(死锁前兆,见 FIX-1)** |
| `serial write recovered` | 写恢复 |
| `io4 HID opened/closed` | 高度感应 HID 开关 |
| `config: ...` | 启动时实际生效的配置 |

正常游玩时日志几乎无输出;仅在状态转换时各写一行。文件可随时删除。

## 与原版一致的 AIR 路径
- `[io3] ir=0xAA` 使用 IO4 HID;64 字节报表布局、switches 偏移 29、
  997/996 错误码状态机、共享报告数据和 beam 位映射均保持一致。

## 未修改的部分
- 板间协议、命令集(0x01 滑条帧/0x05 空气/0x0EE 重同步/0xF0 硬件信息/
  0x10 复位/2 LED/3,4 报表开关/6 空气报表)、转义(0xFF→FD FE,
  0xFD→FD FC)、校验和(0xFF 种子)完全一致,已通过回环测试验证。
- 配置键/段名([nico]/[io3]/[gfx])、全部默认值、VID/PID 候选链、
  3 秒枚举缓存、光束映射表、线程优先级(TIME_CRITICAL)、
  SetThreadExecutionState 周期、chuniio API 版本 0x0102 全部一致。
- 61 个导出符号与原版完全一致(函数为 cdecl,数据符号照常导出)。
