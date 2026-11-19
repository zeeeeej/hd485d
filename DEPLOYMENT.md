# RV1106 部署交接：0.1.1

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
