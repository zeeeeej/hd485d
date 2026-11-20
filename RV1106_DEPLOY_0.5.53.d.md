# RV1106 App 0.5.53.d OEM 部署记录（2026-11-20）

按 xpl 指令，将当前 QJY RV1106 V1 从隔离调试目录切换到正式 OEM 路径。配套 App 0.5.53.d / UART 1.0.27 / hd485d 0.4.1；使用方式一（idle）、qjy 业务过滤、ttyS3 / GPIO35 / 230400 8N1。

## 安装与备份

- 传输：115200 日志串口 ZMODEM；整包 MD5 `52b5df931509122b03f6ab70d83454ed`，包内全部文件校验通过。
- 旧 App / daemon 正常退出，ttyS3 / ttyS4 / watchdog FD 均释放；没有强杀。
- 备份：`/userdata/backup/hd485-before-0.5.53.d-20261120-095136-2780`。备份中的 OEM App 是部署前的 0.4.52.d；部署前实际运行的是隔离目录中的 0.4.53.d，隔离目录保留。
- App：`/oem/usr/bin/hd_camera_app2`，MD5 `ee98591eb344af47509e23e5bb230500`。
- UART：`/oem/usr/lib/libhd_uart_qjy_rv1106.so`，MD5 `aec6444ce1dd9529a8b50171dfbc0ad4`。
- daemon：`/oem/hd485/rv1106/bin/hd485d`，MD5 `507fd6a954d4953878a059eebdeb841c`。
- 开机入口：`/etc/init.d/S21hd485d` → `/etc/init.d/S22hd_camera_app2` → 原 OEM 启动器。
- 保留模型、其他业务库和设备配置；未执行固件/系统分区升级，未提交 Git，原 camera_app 主目录未修改。

## 验证结果

重启前已验证：485 App 版本为 0.5.53.d；50 次心跳全部应答；静态相机转发及内外 CRC；Shell 命令；1 KiB 文件上传/下载 SHA256 一致；交互 PTY；业务抢占 PTY；重新建立维护会话及恢复业务应答。

测试脚本最初未释放下载句柄便打开 PTY，收到 EBUSY；按协议补充 FILE_CLOSE（39）后完整通过，未修改交付程序。

正常停止完整服务后执行一次板端 reboot，boot_id 从 `d4127408-486a-4005-8af1-3958dd5953a9` 变为 `c52b3f40-94c0-4cb8-a158-21c8c09fbe41`。未手动调用启动命令，新版自动启动：

- hd485d PID 472，来自 OEM 路径，仅它打开 ttyS3。
- App PID 923，来自 OEM 路径，UART 从 `/oem/usr/lib` 加载；App 打开 ttyS4 / watchdog，无 LD_PRELOAD。
- App 日志在 2026-11-20 17:53:50 +08:00 出现 `Step6 business components ready`、`entering main loop`。
- daemon `mode=0 owner=0 tx=0 error=0`，安装文件重启后 MD5 不变。
- 本次没有触发开关门检测或重新做大文件吞吐/内存对比，不将历史性能结果算作本轮测量。

## 用户验收结论

2026-11-20，xpl 确认“验证完毕”，本次 **App 0.5.53.d / UART 1.0.27 / hd485d 0.4.1** 部署用户验收完成，新版保留供调试。

此前 Agent 重启后进行 485 自动复测时，串口由 CamDock 占用，因此未完成该次重复检查；此事实保留为自动验证边界。随后以 xpl 的明确确认为本次用户验收结论，不再列为等待串口释放。用户未提供新增逐项结果或测量数据，不据此声称自动复测、性能测试或两板回归重新执行。

## 日常操作

```sh
# 启动整套服务；S22 会先保证 hd485d 就绪。
sh /etc/init.d/S22hd_camera_app2 start
# 状态。
sh /etc/init.d/S21hd485d status
# 正常停止，App 在前，daemon 在后。
sh /oem/hd485/tools/stop-rv1106-stack.sh
```

当前固件 rcK 的循环实际按正序调用 stop（注释称逆序），不要把注释当成保证；计划重启前按上述脚本正常停止整套服务。本轮按此顺序操作，未改系统 rcK。

本地原始验证材料保存在 `~/Documents/tmp/hd-rs485-drive/deploy-0.5.53.d/`，不将可能含设备信息的完整启动日志写入 Git。交付 tar 保持发布时校验值不变。
