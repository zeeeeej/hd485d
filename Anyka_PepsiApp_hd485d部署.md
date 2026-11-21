# Anyka：PepsiApp 7.2.32b + hd485d 0.4.2

本次使用 xpl 指定的新串口实板验证。日期为主机时间 2026-11-21；板端 RTC 仍为2000年，本次未校时。

## 源码、版本与适用板子

- App worktree：`/Users/xiangpengle/Documents/ai/hd-camera-baishi/PepsiApp-hd485`，分支 `feat/hd485-anyka`。
- 基线为 PepsiApp `dev` 的 `2c22074` 加主目录已有未提交改动；主目录源码未修改。原部署阶段未提交；本次按 xpl 授权将实现、测试和文档归档。两个仓库尚未配置远端，先完成本地提交。
- App 7.2.32b；hd485d 0.4.2。SDK 7.5.0 / system 7.0.1，开启 `PEPSI_SDK_NO_SN=ON`，SN为空。
- ARM926EJ-S / ARMv5TEJ、uClibc、Linux 4.4.302-cip94；可见 RAM 33,484 KiB。
- APP 分区1536 KiB，CONFIG 1024 KiB，USERDATA 9828 KiB。部署前USERDATA可用7252 KiB。
- 保留原模型 `/usr/sbin/model_0x0A000000_V1.1.0.bin`（440896字节）。不刷系统、分区或模型，不使用SDK7.5.1的新模型库。

| 用途 | 主机串口 | 板端与参数 |
|---|---|---|
| 日志/部署 | `/dev/cu.wchusbserial12410` | `/dev/ttySAK0`，115200、8N1，root/空密码 |
| 业务/HDTools | `/dev/cu.wchusbserial12440` | `/dev/ttySAK2`，230400、8N1，无流控，地址01 |
| 485方向 | — | GPIO5，高电平TX、低电平RX；仅hd485d操作 |

原厂进程是 `/etc/config/fridge_ipc`，同时持有UART、GPIO5、看门狗；第一次切换必须正常停止它。不要强杀硬件宿主。

## 数据流与行为

物理485 → hd485d百事帧保护/维护分流 → Unix socket → PepsiApp原协议解析和业务逻辑。回复反向流动；hd485d统一完成方向控制及排空，再通过TX_DONE通知App。

- App参数 `--hd485-socket /tmp/hd485.sock`，与原 `--serial` 互斥。共享模式不打开物理UART、不操作GPIO5；SDK相机、陀螺仪、看门狗仍归App。
- `--rx-profile pepsi` 保护业务载荷内的H485标记，维护帧不交给业务订阅者。能力值27，要求FILTERED_RX、PEPSI_RX、STREAM_TX以及230400/8N1。
- 使用方式一：有效业务请求取得控制权，回复实际发送完成后释放；主动拍照等异步请求一直保留至应答结束。App运行但无事务时owner=0，允许HDTools维护。
- 事件出队、主动图片传输完成清理沿用原条件，只把“UART写入+排空”替换为匹配ticket的TX_DONE成功。
- 断线/RX_LOST/发送错误不重放、不回退直连，1秒间隔重新连接。跨连接遗留的异步应答会被抑制。守护进程异常退出需要运维重新启动；服务脚本不做无限重启或自动恢复原厂。
- 本板没有兼容产测程序，FF返回失败。99格式仍为App、SO、manifest三个文件，App和SO由current一起切换，hd485d独立部署。
- 每次App启动清空其自身数据目录中的事件、计数和照片，这是既有业务规则；本次使用独立新目录，旧目录和备份保留。

## 构建和交付文件

复用容器 `x-luckfox-pro` 提供的Anycloud GCC5.5工具链，不执行旧工程全量固件/加密构建。App构建脚本使用自身worktree路径，避免误编译PepsiApp主目录。

```sh
cd /Users/xiangpengle/Documents/ai/hd-camera-baishi/PepsiApp-hd485
PEPSI_BUILD_ROOT="$HOME/Documents/tmp/hd-camera-baishi/hd485-integration/arm" \
  bash tools/build_anyko.sh

cd /Users/xiangpengle/Documents/ai/hd-rs485-drive
HD485_CONTAINER=x-luckfox-pro \
CROSS_COMPILE=/workspace_ai/hd_camera/prebuilt/qjy-anyko/arm-anycloud-linux-uclibcgnueabi/bin/arm-anycloud-linux-uclibcgnueabi- \
  sh tools/build-anyka-docker.sh
```

App默认SDK目录为父目录source中的 `fridge_ipc_sdk_v7.5.0_sys7.0.1-json-uart`。可以用`PEPSI_SDK_DIR`和`PEPSI_SDK_NO_SN`显式覆盖，必须确认与目标板模型兼容。

- App产物：worktree的 `dist/PepsiApp-7.2.32b-anyko`；OTA包为同名前缀的 `.tar.gz`。
- daemon包：本仓库 `dist/hd485-anyka-0.4.2.tar`。
- 联合安装包：本仓库 `dist/PepsiApp-7.2.32b_hd485d-0.4.2-anyka.tar`，附SHA-256。普通tar，板端无需gzip。
- 联合包包括App、匹配SO、daemon、示例工具、服务/安装/回退脚本、SHA256SUMS和验证用直连基线。不会替换原厂exe/SO。

## 新板首次安装

下面命令在日志口的root shell执行。先核对分区、模型、串口占用、剩余空间；本包用于上述旧模型板。不要在已有部署目录上直接覆盖解包。

1. 在 `/userdata/bak/pepsi-hd485-before-主机时间/` 保存 `/etc/config/app.sh`、`fridge_ipc`、`libfridge_ipc.so`、`/userdata/fridge_inspector.ini`，以及已有业务数据的普通tar。生成SHA256SUMS并逐项校验。本板备份为：
   `/userdata/bak/pepsi-hd485-before-20261121-134650`。
2. 通过worktree的 `tools/board_console.py --port /dev/cu.wchusbserial12410 --upload 本地联合包 /tmp/pepsi-hd485-deploy.tar` 上传并校验，然后：

```sh
test ! -e /userdata/pepsi-hd485 && \
  tar xf /tmp/pepsi-hd485-deploy.tar -C /userdata
cd /userdata/pepsi-hd485
sha256sum -c SHA256SUMS
ln -s releases/7.2.32b current
```

3. 核对原厂PID的 `/proc/PID/exe` 后使用 `/bin/kill -TERM PID`，等其正常退出，确认日志中看门狗停止，并检查没有其他进程持有ttySAK2/GPIO5/watchdog。停止失败就中止切换。
4. 启动并检查，再通过485验证91版本、96自检、图片和维护功能：

```sh
sh /userdata/pepsi-hd485/shared-service.sh start
sh /userdata/pepsi-hd485/shared-service.sh status
```

5. 将已验证的入口安装为开机启动；参数必须是该板真实备份目录：

```sh
sh /userdata/pepsi-hd485/shared-install.sh \
  /userdata/bak/pepsi-hd485-before-20261121-134650
sh /userdata/pepsi-hd485/shared-service.sh stop
sync
reboot
```

安装脚本保留app.sh的传感器初始化和ISP等待，只替换应用启动行。重启后顺序为传感器/ISP → hd485d → socket检查 → App能力/参数检查 → SDK初始化/锐度-10/自检。

## 完整启动命令与日常操作

推荐使用管理脚本；它校验PID对应的可执行文件、阻止重复启动，并在启动前检查硬件占用。

```sh
sh /userdata/pepsi-hd485/shared-service.sh start
sh /userdata/pepsi-hd485/shared-service.sh status
sh /userdata/pepsi-hd485/shared-service.sh stop
sh /userdata/pepsi-hd485/shared-service.sh restart
```

脚本的核心启动参数如下。手动调试前先停止服务，不要与脚本同时启动第二份进程：

```sh
/userdata/pepsi-hd485/hd485/bin/hd485d \
  --device /dev/ttySAK2 --socket /tmp/hd485.sock \
  --root /userdata/pepsi-hd485/files --baud 230400 \
  --de gpio-anyka --gpio 5 --lock-dir /tmp \
  --mode idle --rx-profile pepsi --allow-shell --allow-pty --fs-root /
```

待socket就绪后，另一个shell启动App：

```sh
release=$(cd /userdata/pepsi-hd485/current && pwd -P)
LD_LIBRARY_PATH="$release" "$release/app" \
  --data /userdata/pepsi-hd485 --hd485-socket /tmp/hd485.sock \
  --address 1 --opening-direction 1 --gyro-sign 1 \
  --factory-script /userdata/pepsi-hd485/unsupported-factory
```

脚本后台运行时的PID、SDK输出及daemon日志放在 `/tmp/pepsi-hd485/`；App自身诊断经异步队列发往日志UART。安装方向参数来自 `/userdata/pepsi-hd485/board.env`。进程状态中owner=0表示空闲，非零表示某个客户端正在持有控制权。

HDTools选择485口、230400、地址1，沿用默认60ms收发间隔。主机应依次安排业务与维护请求，不在等待业务应答时并发发送维护帧。命令行工具现在也默认60ms，可以显式设置：

```sh
python3 tools/hd485ctl.py --port /dev/cu.wchusbserial12440 \
  --baud 230400 --turnaround-ms 60 exec 'pwd'
```

## 回退

```sh
sh /userdata/pepsi-hd485/shared-rollback.sh \
  /userdata/bak/pepsi-hd485-before-20261121-134650
```

脚本先正常停止App，再停止daemon，验证原厂exe/SO仍与备份一致，恢复app.sh和原INI，并启动原厂程序。保留新版目录及备份，便于再次安装。原厂串口参数与百事版不同，不要以230400下的百事查询结果判断原厂是否启动。

## 实测资源与性能

以下保留首次部署时的不同版本对比。同一 App 7.2.32b 分别直连/共享的最新受控比较见 [Anyka hd485d 对比](Anyka_hd485d对比.md)，不要混用两组样本。

以下是同板、同SDK7.5.0、同230400配置的短时实测；RSS会重复计算共享库，合计占用优先看PSS。空闲CPU采样约5秒，包含既有诊断/传感器工作，不代表长期均值。

| 指标 | 直连7.1.31b | 共享7.2.32b App | hd485d0.4.2 |
|---|---:|---:|---:|
| 可执行文件字节数 | 108484 | 112604 | 64028 |
| 预热后空闲RSS，KiB | 2092 | 2116 | 1012 |
| 预热后空闲PSS，KiB | 2092 | 1884 | 780 |
| 空闲CPU，占单核 | 约3.10% | 约3.95% | 约0.83% |
| 传图后RSS/PSS，KiB | 未取得完整成功样本 | 2220 / 1988 | 1016 / 784 |

共享版合计空闲PSS为2664 KiB，比直连增加572 KiB（约0.56MiB）；App二进制增加4120字节。匹配SDK SO为145356字节。核心部署文件合计346293字节（约338KiB，不含直连验证程序、备份、日志、照片和文件系统开销）。

- 各100次91查询：直连中位21.66ms、P95 23.21ms；共享中位31.51ms、P95 34.63ms。此轮共享增加约9.85ms中位延迟。
- 共享版3张图片分别73887、73885、73626字节，1024字节请求分包，均通过板端/主机SHA256比对。排除首包拍照和日志口取摘要时间，纯图片载荷分别12.15、12.19、12.28KiB/s；首包含拍照约225～277ms。
- 直连基线在1024和256字节请求分包时均观察到短帧超时；一个256分包应答预期407字节、只收到350字节。故没有可用于完整图片吞吐百分比比较的直连成功样本。不能把这个故障归因于socket，也不把有限共享成功样本表述为长期可靠性结论。
- 共享图片阶段短时CPU约App7.45%、daemon20.80%；包含Anyka原有排空等待，直连缺少对应完整样本，不能据此计算CPU回归幅度。

| 文件 | SHA-256 |
|---|---|
| App7.2.32b | `de7d57e9adf2f4f02be9a4085859faf7a42e9c909f6e260257d8b8bbb1e6e483` |
| hd485d0.4.2 | `e2675d7a3c0af0abf1a0663f5018daa6ba3f2b6e7f80aaaac2c12a0793900808` |
| SDK7.5.0 SO | `341b31520c067842d1ed9df30c35a36139303ed1a0c3ed3c6a6e4d3d23a76926` |

## 验证记录

- 本地App：10项CTest（ASan/UBSan），包含事件发送失败保留、照片保留、OTA切换、真实socket的TX_DONE顺序与失败、跨连接拍照应答抑制。板端HAL桩验证共享模式即使GPIO访问失败仍能初始化SDK且不打开GPIO。
- hd485d：39项完整集成测试通过，另新增主机成功/错误应答后的收发间隔测试通过；4项流式发送测试通过。百事测试覆盖头部逐位置拆分、维护与业务粘包、业务内嵌维护帧及残帧超时恢复。
- 实板：版本、相机/陀螺仪自检、参数查询、FE查询、FF拒绝、3张完整图片、32KiB维护文件往返、普通命令/cd、PTY/top/Ctrl-C/resize/手动关闭、vi保存后读取、20秒空闲超时退出通过。
- 业务监听：普通命令、文件、PTY期间订阅文件0字节；随后91请求收到完整82字节且摘要与发送请求相同。
- 异步拍照：处理中owner非零，应答结束后owner=0。
- 单独重启daemon：App PID860未变，daemon852→1092；App自动重连后版本查询成功，没有接管物理UART。
- 开机启动和原厂回退：最终实测结果见本文末尾补充。

实际柜门运动、真实人脸场景、长期压力和完整物理485 OTA升降级未在本轮验证。OTA准备/提交逻辑已做自动化回归；本轮不改99协议，也不把自动化回归称为实板OTA验收。

原始构建日志、测量JSON、照片及串口记录在 `~/Documents/tmp/hd-camera-baishi/hd485-integration/`，包含baseline.patch、measure-*、maintenance-results.json、pty-extra.json与备份/启动日志。

### 最终启动与回退验收

已先安装新开机入口，再运行回退脚本：原厂程序PID1177正常启动，app.sh、原厂exe/SO与备份逐字节相同，hd485 socket消失。随后正常停止原厂，重新启动新版、安装入口、正常停止两服务并执行整机重启。

重启后实测：

- App PID **511**，hd485d PID **501**（仅代表此次启动）；当前为新组合供调试。
- `/proc/501/fd/9`为ttySAK2，fd11为GPIO5；App只持有watchdog，未持有物理485/GPIO。
- App映射的SO来自`/userdata/pepsi-hd485/releases/7.2.32b/`，所有部署清单摘要通过。
- 50次91查询均返回7.2.32b、SDK7.5.0/system7.0.1；96返回status=1、camera=1、Gyro=1。
- 新CLI以60ms收发间隔执行`printf BOOT_SHARED_OK`成功；所有测试进程已退出，两主机串口已释放。
- USERDATA可用5740 KiB，CONFIG可用472 KiB；包含保留的旧目录、备份和测试产物。
- 原PepsiApp主目录的未提交补丁SHA256前后均为`a79e7f96334ea2dbd6b894c5914e6c0b3ff2973567e3151b9d64ca1759293b10`。

联合tar大小471040字节，SHA256为`43605568f3ff180e4869cb84bd7c7d75f731254348a2b848d2b79d36c4211036`。维护测试记录中的异常退出来自测试监听进程清理时的日志shell退出；维护功能断言已全部完成，随后已用`/bin/kill`独立验证清理且保持日志shell存活。管理脚本使用`/bin/kill`，并已通过上述回退与重启流程。


### 后续同版本对比与恢复状态（2026-11-21）

同一App7.2.32b直连/共享补测已完成，结果见[性能对比](Anyka_hd485d对比.md)。共享增加约0.55MiB PSS、62.5KiB必需daemon文件，查询增加约10.2ms；成功图片吞吐约低8.9%（93）/12.0%（9C）。共享93出现一次1499字节应答只收到1496字节的真实短帧，仍待排查；这不覆盖或撤销前文已完成的单项功能验证，也不代表稳定性完全通过。

补测结束已恢复正式共享服务，App1318、hd485d1310，业务版本及相机/陀螺仪自检正常，owner=0，开机配置不变。此时USERDATA可用5448KiB，包含新增比较夹具；二进制版本和摘要保持。端口已释放。上述511/501仍作为此前整机重启验收记录保留。本次更新文档并提交源码，不重建或覆盖已部署的包。


### 发送优化实验后的状态（2026-11-21）

后续已测清usleep(100)延迟并实现小帧WRITE提交优化，详见[优化验证](Anyka_TX优化验证.md)。性能改善已实测，但优化版仍有一次主动图短帧，故完成维护回归后恢复本部署文档的原共享二进制：App de7d57e9…、daemon e2675d7a…。当前App1621/daemon1613，自检正常，开机入口不变。优化产物和原版备份留在validation目录，未替换正式发布包；xpl随后确认验证完毕并授权提交本轮源码与文档，归档不改变上述板端状态。
