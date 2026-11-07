# nicochuniio — Fitouch2 chuniio DLL 重建

> 完整的问题分析与修复过程见 **[ANALYSIS.md](ANALYSIS.md)**;
> 与原版二进制的行为差异见 **[FIXES.md](FIXES.md)**。

这是 Fitouch2 触摸板 `fitouch2.dll` 的社区源码重建工程(内部工程名
`nicochuniio`)，不是原厂源码或官方发行版。重建实现参考了原 DLL 的调试
信息和运行行为，并在尽量保持协议、导出和配置兼容的前提下修复了输入
延迟、CPU 空转、串口写阻塞和缓冲区安全问题。完整差异见 [FIXES.md](FIXES.md)，
问题分析见 [ANALYSIS.md](ANALYSIS.md)。

## 优化点

- **串口写失败快速返回**：将写超时从随报文长度增长的约 `2*N+1 ms`
  改为固定约 `1 ms`。板端接收阻塞时跳过该次 LED 写入，避免游戏 IO
  线程被拖住；串口恢复后后续 LED 写入可继续工作。
- **消除轮询空转**：读操作在数据到达或 2 ms 超时后返回，替代完全非阻塞
  的忙等循环，避免滑条和 IO4 线程持续占用 CPU。
- **读写并行**：输入读缓冲不再与 LED 串口写共享互斥锁，降低慢写对输入
  读取的影响。
- **减少静置时重连**：帧头看门狗由 2 秒延长到 20 秒，避免菜单静置期间
  反复关闭、枚举和重开设备，同时保留端口失效后的重连恢复。
- **修复 AIR HID 等待读取**：保留原版 overlapped I/O 完成状态机；默认构建
  使用阻塞等待 AIR HID 报告的变体，另提供非阻塞轮询变体用于对照。
- **提高帧打包内存安全性**：扩大发送打包临时缓冲区并保留原有发送上限，
  防止转义后的 LED 数据写越界。
- **增加故障诊断日志**：只在端口、重连、看门狗等状态变化时写日志，方便
  定位串口被占用或板端停止接收等问题。

这里的“优化”描述来自代码与故障分析；CPU 占用和输入延迟的具体改善幅度
尚未在真实 Fitouch 硬件上做量化基准。兼容性测试和已知限制见下文。

## 目录结构(与原版一致)

```
src/
  core/chuniio.c          chuniio API 入口 + 滑条/io4 工作线程
  core/config.c           .\segatools.ini 配置读取
  core/input_packet.c     串口流/HID 流消费逻辑
  feat/cursor.c           全屏模式鼠标指针归位
  hardware/serial_port.c  CDC 串口传输(枚举/开关/缓存)
  hardware/io4_port.c     HID 传输(io4 高度感应)
  protocol/packet_solver.c  组帧/解帧/转义/校验/FSM
  protocol/slider_protocol.c 板端命令构造
  utils/queue.c           环形队列(兼容导出)
  dllmain.c
nicochuniio.def           导出表(61 个符号,与原版一致)
build.sh                  构建脚本
```

## 构建

WSL/Debian:
```bash
sudo apt-get install gcc-mingw-w64-i686
./build.sh          # 产物: out/fitouch2-air-polling.dll 和
                    #       out/fitouch2-air-blocking.dll (i386)
```

`fitouch2-air-polling.dll` 按原版方式非阻塞轮询 AIR HID 报告；
`fitouch2-air-blocking.dll` 在报告尚未到达时阻塞等待。默认的
`fitouch2.dll` 与阻塞版相同；需要对照测试时可改用非阻塞轮询版。

Windows 原生:安装 [MSYS2](https://www.msys2.org) 后在 MinGW32 shell 里
用同样的命令行编译(去掉 build.sh 里的包装即可)。

## 部署

1. 备份原 DLL:
   `bin\fitouch2.dll` → `bin\fitouch2.dll.orig`
2. 复制 `out\fitouch2.dll` 到游戏 `bin\` 目录。
3. 启动游戏验证:
   - 任务管理器中 chusanApp 的 CPU 占用应明显下降;
   - 菜单静置后再触摸,输入应立即响应(无秒级延迟);
   - 调试输出("使用IO4模式模拟AIR"等)与原版相同。

如出现任何异常,把 `fitouch2.dll.orig` 改回 `fitouch2.dll` 即可完全
还原。

## 日志

DLL 会在自身目录写 `nicochuniio.log`(正常游玩几乎无输出)。实时查看:

```powershell
Get-Content .\nicochuniio.log -Wait -Tail 20
```

关键行:
- `WATCHDOG: no frame header for >20000 ms` — 看门狗触发(20 秒没收到任何帧头,自动关端口重连)
- `serial write FAILED ... board RX wedged?` — 板子不收数据了(原版死锁的前兆,现在会自动恢复)
- 若日志里出现**两个不同的 PID** 且其一反复 `serial open FAILED err=5`,说明有别的程序占用了串口。

## 已验证

- 导出表:61/61 符号与原版一致,cdecl 调用约定,i386 架构。
- 协议层回环测试(在 Linux 上以桩函数编译 packet_solver.c):
  滑条帧解析、转义往返、校验错误重同步、命令帧编码、LED 帧转义、
  超限帧丢弃、0xEE/0xF0 命令 — 全部通过。
- open_port 超时配置与 FIX-1/2 一致(反汇编确认)。
- 构建脚本可生成 i386 DLL；AIR HID 轮询与阻塞两种变体均可构建。

## 未验证 / 注意

- 未在真实 Fitouch 板上联机测试(构建环境为 WSL,无硬件)。首次部署
  建议先在选歌菜单玩 10 分钟确认输入/灯效正常,再进行游玩。
- 若你的 segatools.ini 里有 [nico] 段,本 DLL 与其兼容(键名一致);
  推荐设置 `autoReconnect=1`、`reconnectInterval=100`。
