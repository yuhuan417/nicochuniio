# nicochuniio — Fitouch2 chuniio DLL 源码重建

> 完整的问题分析与修复过程见 **[ANALYSIS.md](ANALYSIS.md)**;
> 与原版二进制的行为差异见 **[FIXES.md](FIXES.md)**。

对 `fitouch2.dll`(Fitouch2 触摸板 chuniio DLL,内部工程名
`nicochuniio`)的源码级重建。原版二进制自带完整 DWARF 调试信息,
本工程按其源文件结构 1:1 还原,并应用了 6 处修复(见 `FIXES.md`)。

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
./build.sh          # 产物: out/fitouch2.dll (i386)
```

Windows 原生:安装 [MSYS2](https://www.msys2.org) 后在 MinGW32 shell 里
用同样的命令行编译(去掉 build.sh 里的包装即可)。

## 部署

1. 备份原 DLL:
   `bin\fitouch2.dll` → `bin\fitouch2.dll.orig`
2. 复制 `out\fitouch2.dll` 到游戏 `bin\` 目录。
3. 启动游戏验证:
   - 任务管理器中 chusanApp 的 CPU 占用应明显下降(原版两个 100% 轮询
     线程变为接近 0);
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

## 未验证 / 注意

- 未在真实 Fitouch 板上联机测试(构建环境为 WSL,无硬件)。首次部署
  建议先在选歌菜单玩 10 分钟确认输入/灯效正常,再进行游玩。
- 若你的 segatools.ini 里有 [nico] 段,本 DLL 与其兼容(键名一致);
  推荐设置 `autoReconnect=1`、`reconnectInterval=100`。
