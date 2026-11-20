# RV1106 / Anyka 部署交接：0.3.0

当前两板已启动 0.3.0，具体启动命令见文末「PTY 0.3.0 调试部署」。

xpl 于 2026-11-20 确认验证完毕。本次更新仅整理文档并提交源码，未重新部署、重启或
读取板端运行状态；下述 PID 和状态均为此前部署交接时的记录。

板卡环境、PTY 基线及实测结果见 [BOARD_PTY.md](BOARD_PTY.md)。

## 0.2.1 历史运行状态

2026-11-19 修复 cd 后已部署 **hd485d 0.2.1**，PID **965**（此次交接值），方式一
`idle --allow-shell --fs-root /`。程序、日志及 PID 文件位于：

```text
/userdata/tmp_var/hd-rs485-drive-0.2.1/rv1106/bin/hd485d
/userdata/tmp_var/hd-rs485-drive-0.2.1/hd485d.log
/userdata/tmp_var/hd-rs485-drive-0.2.1/hd485d.pid
```

旧 0.2.0 PID 913 正常退出，目录保留可回退。新包 153600 字节，SHA-256
`d162fee57c73f691bc81c554e7394716f34a041c0615481d76f6ad301f265e93`，板端包 MD5 `3cb50135f997865e29e12660ee6b190d`；
服务 MD5 `523b67e6d226574f4c8af4a2413fbde1`，均与本地一致。

真实 HDTools 会话已验证：`cd /userdata` 后下一条 `pwd` 返回 `/userdata`；相对目录、
`cd -`、切换失败保持原目录、带非零退出码的命令，以及重连重置初始目录均通过。
旧 HDTools 0.1.2 断开重连即可使用服务修复；0.1.3 另增加“工作目录随会话保留”的能力提示。
文件浏览路径仍独立，export／alias 等 Shell 环境不跨命令保留。

仍使用 ttyS3、230400 8N1、GPIO35、`/run/hd485.sock`。未重启，未修改开机配置，
原相机进程未恢复；重启后仍按原配置启动原相机应用。需回退时，核验 PID/exe 后正常
终止 0.2.1，等待 Socket 和锁释放，再使用下述保留的 0.2.0 目录与启动参数。

## 0.2.0 历史运行状态（2026-11-19）

已按 xpl 指示部署并启动 **hd485d 0.2.0**，使用方式一 `idle`，
`--allow-shell --fs-root /`。PID **913**（仅代表此次交接），程序位于：

```text
/userdata/tmp_var/hd-rs485-drive-0.2.0/rv1106/bin/hd485d
```

同级包目录保存 `hd485d.pid`、`hd485d.log` 与 `hd485-rv1106-0.2.0.tar`。
仍使用 `/dev/ttyS3`、230400 8N1、GPIO35、`/run/hd485.sock`；空闲 GPIO35 为 0。
原相机进程未运行。旧 0.1.1（PID 750）已正常退出，目录完整保留用于回退。
没有重启、修改开机配置或替换原相机文件。重启后仍按原配置启动原相机应用。

新版包 143360 字节，SHA-256：
`b6a3b77cc973f976ed6e274f45ca66297a0e34fdddd317a85b2b3081b207eb60`。
板端包 MD5 `b6e6015ed572bb15a87c35e57c2baee2`，服务 MD5
`560f68cc832b08a54c3fb88a28b1819d`，与本地包一致。

HDTools 0.1.1（已加入 60 ms 换向间隔）自身 SerialIO / Session / RemoteDevice 已通过真实 USB-RS485 验证：
版本查询、根目录、中文文件名、2 KiB 二进制文件、零字节文件、上传／下载取消后从
240 字节恢复，以及 SHA-256 一致。SDK 发送业务数据后，旧维护会话失效，重新 HELLO
可继续终端操作；最终 `owner=0`、`error=0`。证据在 `build/rv1106-deployment-0.2.0.log`、
`build/hdtools-live-board-0.1.1.xml`、`build/rv1106-preemption-0.2.0.log`。

启动使用以下显式参数（此板 BusyBox 不提供 pidof，进程占用通过 /proc 核验）：

```sh
cd /userdata/tmp_var/hd-rs485-drive-0.2.0
(trap "" HUP; exec rv1106/bin/hd485d --device /dev/ttyS3 --socket /run/hd485.sock \
  --root /userdata/hd-rs485/files --fs-root / --baud 230400 --de gpio --gpio 35 \
  --mode idle --allow-shell) </dev/null >hd485d.log 2>&1 &
echo $! > hd485d.pid
```

以上启动命令仅供服务未运行时使用。回退时通过日志口读取新版 pid 文件，先核验
`/proc/<PID>/exe`，正常 kill，等待 socket 和 UART 锁释放，再从下述保留的 0.1.1
目录启动旧服务。无需删除任何旧部署目录。

## 以下为 0.1.1 历史交接记录

日期：2026-11-19。按 xpl 指示完成交叉编译和板端启动；xpl 随后确认方式一终端模式实机验证通过。文件传输、业务切换及方式二仍待专项验证，详见 `VALIDATION.md`。

## 部署交接时的运行状态

| 项目 | 已核验值 |
|---|---|
| 板型／内核 | QJY RV1106 V1 Board，Linux 5.10.226 |
| 服务版本／模式 | hd485d 0.1.1，方式一 `idle`，已开启 `--allow-shell` |
| PID | 750，仅代表本次交接时的实际进程 |
| 串口／方向 | `/dev/ttyS3`，230400 8N1，GPIO35，交接时为低电平接收 |
| 本地 Socket | `/run/hd485.sock` |
| UART 锁 | `/run/hd485-uart-443.lock`，内容为实际服务 PID |
| 状态查询 | `mode=0 owner=0 epoch=0 tx=0 error=0 rx=0` |
| 相机应用 | 已正常停止，退出码 0；看门狗日志确认 disabled |
| 系统配置 | 未替换 OEM 程序，未修改开机配置；未重启板子 |

程序目录：

```text
/userdata/tmp_var/hd-rs485-drive-0.1.1-20261119/rv1106/
```

同级父目录保存 `hd485d.log`、`hd485d.pid` 和已校验的发布包。文件传输根目录为 `/userdata/hd-rs485/files`。

当前为临时调试运行。板子重启后仍按原开机配置启动相机应用，不会自动启动 hd485d。

## 主机测试入口

在 Mac 项目目录执行；确保 CamDock 或其他工具没有占用该口：

```sh
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 status
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 exec '@version'
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 exec 'cat /proc/version'
```

板端当前 BusyBox 没有 `uname` 命令，因此用 `cat /proc/version` 检查系统信息。首次测试可先执行内建 `@version`，预期输出 `hd485d 0.1.1`。

部署过程中 Agent 只通过独立日志口检查 SDK 状态。随后 xpl 使用 Mac 侧 `/dev/cu.usbserial-FTAP6OK03` 执行终端命令，并确认终端模式通过。

## 日志口检查与恢复原相机应用

通过日志口 `/dev/cu.wchusbserial144440`（115200）进入板端 Shell，可执行：

```sh
cat /userdata/tmp_var/hd-rs485-drive-0.1.1-20261119/hd485d.log
/userdata/tmp_var/hd-rs485-drive-0.1.1-20261119/rv1106/bin/hd485-example /run/hd485.sock status
```

结束测试时，先读取 PID 并核验可执行文件：

```sh
HD485_PID=$(cat /userdata/tmp_var/hd-rs485-drive-0.1.1-20261119/hd485d.pid)
readlink /proc/$HD485_PID/exe
```

确认指向上述 hd485d 后正常终止：

```sh
kill "$HD485_PID"
```

确认该进程已退出、`/run/hd485.sock` 和 `/run/hd485-uart-443.lock` 已清理，再恢复相机服务并检查温控器业务：

```sh
/etc/init.d/S22hd_camera_app2 start
```

不要同时运行原相机应用和 hd485d，也不要在进程仍运行时删除锁文件。

## 发布包与校验

- 本地包：`dist/hd485-rv1106-0.1.1.tar`，133120 字节。
- SHA-256：`f80379c824ff2f878cd1351dc401a9110a0ee749aed266467286f9f011cc94da`。
- 包 MD5：`a06f20132a287ec7d1bea557dcb3ff59`，传输后板端一致。
- 服务 MD5：`0d1285fe485760fddad73232c5d3635d`，板端解包后与本地一致。
- 示例 MD5：`fc74caf517e657657c4dc31d41730952`，板端解包后与本地一致。

使用 camera_app 同一 Linux 容器与 GCC 8.3.0 ARM/uClibc 工具链，确认 ARM EABI5 hard-float、解释器 `/lib/ld-uClibc.so.0`，且已在目标板实际加载运行。首次 0.1.0 启动因 flock 不支持失败，已自动恢复原相机服务；修复为 0.1.1 后重新正常停机、部署成功。


## HDTools / 0.2.0 部署步骤参考

已按下列流程部署 `dist/hd485-rv1106-0.2.0.tar`，旧目录保持完整。
新版启用文件浏览需要 `--fs-root /`。建议采用新的板端目录
`/userdata/tmp_var/hd-rs485-drive-0.2.0/` 保存包与解包结果。

1. 通过已确认的日志口检查当前进程、看门狗和 ttyS3 占用，核对旧 hd485d PID 与 exe。
2. 将新包传至新目录，核验哈希，使用 `tar -xf` 解包。保留原 0.1.1 目录。
3. 正常终止已核对身份的旧 hd485d，等待 socket／设备锁释放。
4. 从新目录的 `rv1106/` 执行 `sh tools/run-rv1106.sh --allow-shell --fs-root /`。
5. HDTools 连接指定 485 口，执行 `@version`；从 `/` 浏览到 `/userdata`，在独立测试子目录
   验证小文件上传、下载 SHA-256、一轮取消恢复，再验证业务抢占后重新连接。
6. 若需回退，先正常退出新版服务，确认资源释放，再从保留的 0.1.1 包运行原启动命令
   `sh tools/run-rv1106.sh --allow-shell`。不修改原相机程序、OEM 分区或开机脚本。

HDTools 未提供创建目录按钮；测试子目录可由操作者在终端使用 `mkdir` 创建。
真实串口与小文件功能已验证；大文件吞吐、长期稳定性及 Windows COM 口仍待专项验收。

## PTY 0.3.0 调试部署（2026-11-19）

本节为新增交互终端部署；前面的 0.2.1/0.2.0 保留为历史回退记录。
xpl 已确认两板部署、测量并保留新版供调试。未修改开机配置或原厂程序文件。

### RV1106

- 新目录 `/userdata/tmp_var/hd-rs485-drive-0.3.0`，最终 PID 1921。
- 旧 PID 965 在确认 owner=0 后正常退出，旧 0.2.1 目录保留。
- UART ttyS3 / 230400 / GPIO35，IPC `/run/hd485.sock`。

从已解包目录启动（先核验 UART/Socket 无其他所有者）：

```sh
cd /userdata/tmp_var/hd-rs485-drive-0.3.0
(trap '' HUP; exec ./rv1106/bin/hd485d \
  --device /dev/ttyS3 --socket /run/hd485.sock \
  --root /userdata/hd-rs485/files --fs-root / \
  --baud 230400 --de gpio --gpio 35 --mode idle \
  --allow-shell --allow-pty) </dev/null >hd485d.log 2>&1 &
echo $! >hd485d.pid
./rv1106/bin/hd485-example /run/hd485.sock status
```

### Anyka

- 新目录 `/userdata/hd-rs485-drive-0.3.0`，最终 PID 3926。
- 原厂 PID 426 收到 SIGTERM 后正常退出；日志明确出现 `watchdog stopped`、PWM disabled
  和摄像头反初始化完成。boot_id 未变，没有重启。原厂文件及 `/etc/config/app.sh` 未修改。
- 用同一工具链编译的 0.2.1 基线放在同目录 `hd485d-baseline-0.2.1`；测量后正常停止。
- UART ttySAK2 / **460800** / GPIO5，IPC `/tmp/hd485.sock`，锁目录 `/tmp`。
  百事参考应用的 230400 不适用于当前调试配置。
- 此板不提供 nohup，使用以下经过验证的启动方式。

```sh
cd /userdata/hd-rs485-drive-0.3.0
(trap '' HUP; exec sh ./anyka/tools/run-anyka.sh \
  --fs-root / --allow-shell --allow-pty) </dev/null >hd485d.log 2>&1 &
echo $! >hd485d.pid
./anyka/bin/hd485-example /tmp/hd485.sock status
```

run-anyka.sh 检查 UART 和 watchdog 占用，不负责停止原厂业务或自动关闭看门狗。
程序成功初始化后进入方式一；连接 HDTools 本身不创建 PTY，点击“打开交互终端”后
才创建 Shell。退出、抢占、断线超时后回收。HDTools 需要 0.2.0（Mac 包 1.2.0）。

### 停止与恢复

先读取 hd485d.pid，再用 `/proc/<PID>/exe` 确认指向本次目录，发送 SIGTERM，等待进程、
Socket 和锁退出。禁止不核对 PID 就复制历史 kill 命令；禁止删除仍有所有者的锁或 Socket。
RV1106 可按历史参数启动保留的 0.2.1。
Anyka 如需恢复原厂业务，在新服务完全退出后按原有目录和环境启动：

```sh
(trap '' HUP; exec env LD_LIBRARY_PATH=/etc/config \
  /etc/config/fridge_ipc /userdata/fridge_inspector.ini) </dev/null >/tmp/fridge-restored.log 2>&1 &
```

上面的原厂恢复命令本轮未执行（按要求保留 PTY 调试服务）。重启会按原开机配置恢复原厂业务，
不会自动启动本次 hd485d。资源数据、完整测试范围和产物指纹见 BOARD_PTY.md、VALIDATION.md。
