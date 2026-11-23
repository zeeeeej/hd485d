# HD485 0.4.3

0.4.3：RV1106的GPIO发送前等待与Anyka统一，保留100微秒硬件稳定时间；发送后5ms保护保持原样。实测与部署状态见[RV1106发送等待实测](RV1106_发送等待实测.md)。

0.4.2 新增 Anyka PepsiApp 接入所需的 `pepsi` 接收保护及能力位；联合部署与实测结果见 [Anyka 部署说明](Anyka_PepsiApp_hd485d部署.md)，同一App的直连/共享比较见 [Anyka hd485d 对比](Anyka_hd485d对比.md)。

Anyka发送前等待与小应答IPC优化的实测、短帧限制及当前板端状态见[Anyka TX优化验证](Anyka_TX优化验证.md)。

架构图、状态图与数据流见 [ARCHITECTURE.md](ARCHITECTURE.md)；板卡环境、文件大小与内存实测见 [BOARD_PTY.md](BOARD_PTY.md)。

单 RS485 通道的用户态共享服务。包含 C 服务 `hd485d`、C SDK、Python 主机工具和业务示例；原有业务字节不封装，应用通过 SDK 申请唯一控制权，其他应用可以订阅过滤维护帧后的业务接收流。

## 当前状态

HDTools **0.2.2**：串口模式改用红绿点交替指示变化，默认稳定 500 ms 后隐藏切换提示与红绿点并显示实际模式（无后续收发也自动刷新）；新增每连接 RX/TX 实际字节统计、手动清零和可保存的稳定时间设置。xpl 已确认本轮验证完毕。交互终端持续轮询时仍会显示切换指示，说明见 [HDTools 使用说明](HDTools/README.md)。

2026-11-20，xpl 确认“验证完毕”，本次 **App 0.5.53.d / UART 1.0.27 / hd485d 0.4.1** 部署用户验收完成，新版保留供调试。 当前板使用正式 OEM 路径和 S21→S22 自启动，已验证重启。详见[部署指南](hd-camera-app2_hd485d部署.md)及[验收记录](RV1106_DEPLOY_0.5.53.d.md)。

0.4.1：增加有界整帧发送、事件唤醒和分段计时。RV1106 已验证 App 0.4.53.d / UART 1.0.27，512 KiB 下行吞吐提高约 17.1%，心跳中位延迟由 30.5 ms 降至 6.8 ms。完整结果与隔离包启动见 [TX_OPTIMIZATION.md](TX_OPTIMIZATION.md)。

0.4.0：订阅接收流过滤合法 H485 维护帧，增加 QJY 业务帧载荷保护及定长 OTA 裸流接口。
RV1106 camera_app 通过 C SDK 按事务占用串口，交付与验证见
[RV1106_INTEGRATION.md](RV1106_INTEGRATION.md)。下述版本记录保留为历史。


0.3.0 增加可选交互 PTY（`--allow-shell --allow-pty`，方式一），HDTools 0.2.0 增加真正的终端视图。已交叉构建并在 RV1106、Anyka 两板部署；xpl 于 2026-11-20 确认验证完毕。交互实测和资源数据见 BOARD_PTY.md，验证范围及限制见 [VALIDATION.md](VALIDATION.md)。下述 0.2.1 为历史记录。

0.2.1 修复终端 cd：同一维护会话保留工作目录和 OLDPWD，HDTools 0.1.3 根据服务能力显示提示。新会话重置目录，文件浏览根目录和业务串口行为保持兼容。

新增 [HDTools 桌面应用](HDTools/README.md)，支持 macOS／Windows、整个配置文件系统的目录浏览、二进制上传下载及命令终端。板端 0.2.0 增加独立的 `--fs-root`；旧 CLI、SDK 与 `--root` 行为保持兼容。

**历史记录：2026-11-19 部署 0.2.1，方式一，启用 `--allow-shell --fs-root /`。同一真实 HDTools 会话内 cd／pwd、相对目录、cd -、重连重置均通过；此前文件传输与业务抢占验证见记录。0.2.0、0.1.1 目录保留用于回退。当前状态见 [部署交接](DEPLOYMENT.md)。**

以下 0.1.1 内容为此前实板验证记录。

已完成 ARM/uClibc 交叉编译，并在 QJY RV1106 V1 板上启动 **0.1.1、方式一（idle）**。板端 SDK 状态查询正常，原相机服务已正常停止，看门狗日志确认关闭。Linux 正式 Socket 路径的 19 项集成测试通过；macOS 历史测试及本次兼容性回归见 [验证记录](VALIDATION.md)。

2026-11-19，xpl 已确认 **方式一终端模式实机验证通过**。文件传输、业务控制权交接及方式二尚未获得本轮用户验收确认。当前运行位置和恢复步骤见 [部署交接](DEPLOYMENT.md)。

首版不提供内核模块调用接口，不修改 `camera_app`，不替换现有业务程序或开机脚本。设计依据见 [plan.md](plan.md)，协议定义见 [PROTOCOL.md](PROTOCOL.md)。

## 构建与本机验证

要求 C11 编译器、CMake 3.16+、pthread；主机工具与集成测试使用 Python 3.11+，无第三方 Python 依赖。

```sh
cmake -S . -B build -DHD485_SANITIZE=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tests/integration.py
```

若执行环境禁止创建监听端点，可运行不需要 bind 的测试后端：

```sh
HD485_TEST_FDS=1 python3 tests/integration.py
```

该模式以 `hd485d-test` 接收继承的已连接 Socket，实际执行相同的协议、仲裁、文件及串口代码；不证明正式监听端点可用。生产 `hd485d` 不接受测试 FD 参数。测试日志与输入输出文件保存在 `build/test-artifacts/`，作为可审阅产物保留。

Linux IPC 使用 `SOCK_SEQPACKET`；macOS 验证后端使用显式帧边界的 `SOCK_STREAM`。这是一项本地平台适配，不改变主机串口协议。

## RV1106 配置与构建

本次指定链路为温控器主机 ↔ RV1106 从机：

| 参数 | 值 |
|---|---|
| 板端端口 | `/dev/ttyS3` |
| 主机 USB-RS485 | `/dev/cu.usbserial-FTAP6OK03` |
| 速率／格式 | `230400 8N1`，业务与空闲维护均相同 |
| DE | GPIO35，高为发送，低为接收 |
| TX 时序 | 拉高，100 μs，完整写入，`tcdrain()`，5 ms，拉低 |
| 独立日志口 | `/dev/cu.wchusbserial144440`，115200 8N1 |

在已核验的 Linux 构建环境执行；现有交叉编译器是 Linux x86-64 ELF，不能直接在 macOS 运行：

```sh
export CROSS_COMPILE=/实际工具链/bin/arm-rockchip830-linux-uclibcgnueabihf-
sh tools/build-rv1106.sh
```

脚本在本工作区生成 `dist/hd485-rv1106-0.4.3.tar` 及 SHA-256 校验文件，不执行部署。包中包含服务、示例、SDK 静态库和头文件、配置与运行脚本。包使用普通 tar，不依赖板端 gzip。

在 Mac 上复用 `camera_app` 的现有 Docker 环境时，可使用隔离快照入口，无需把本工作区挂载到容器，也不会覆盖容器中的另一份 camera_app：

```sh
export HD485_CONTAINER=已核验的构建容器名称
export CROSS_COMPILE=/容器内实际工具链/bin/arm-rockchip830-linux-uclibcgnueabihf-
sh tools/build-rv1106-docker.sh
```

该入口先检查容器状态、挂载和编译器目标，再将本项目源码复制到容器用户的 `~/Documents/tmp/hd-rs485-drive/` 独立目录，调用同一交叉编译脚本，取回 `dist/` 并核验包哈希。宿主快照与日志保存在宿主 `~/Documents/tmp/hd-rs485-drive/`，运行后保留供审阅。它仍需要当前执行环境有 Docker API 访问权限。

## 板端调试与恢复流程

1. 核验串口设备、接线和占用。退出 CamDock 的该串口连接；不要同时运行另一主机模拟器或把第二个主机并接到仍工作的温控器总线。当前程序不负责总线多主仲裁。
2. 通过独立日志口检查板型、运行内核、相机进程和看门狗。使用现有 `/etc/init.d/S22hd_camera_app2 stop` 正常退出，确认 `/dev/ttyS3` 已释放及看门狗已停用。不要以 `kill -9` 代替正常退出。
3. 使用已有受控传输方式将已核验包放入板端 `/userdata/tmp_var/hd-rs485-drive/` 并解包；不覆盖 OEM 程序，不触发 OTA，不刷系统分区。
4. 在解包后的 `rv1106` 目录运行：

```sh
sh tools/run-rv1106.sh --allow-shell
# HDTools 浏览整个设备文件系统：
sh tools/run-rv1106.sh --allow-shell --fs-root /
# 或显式启用由主机保证时机的共存模式：
sh tools/run-rv1106.sh --mode coexist --allow-shell
```

运行脚本会拒绝相机应用仍在运行、串口仍有打开者或 Socket 路径已存在的情况，不替用户停止进程或删除 Socket。首次运行会创建 `/userdata/hd-rs485/files`；上传续传文件保留在这个目录中。

5. 在板端其他终端测试 SDK：

```sh
bin/hd485-example /run/hd485.sock status
bin/hd485-example /run/hd485.sock listen
bin/hd485-example /run/hd485.sock send 'business-test'
```

`echo` 示例在独占控制期间原样回送过滤后的业务 RX，只适用于隔离回环测试；合法维护帧不会回送。

`listen` 原样打印过滤后的业务 RX，不解析业务载荷。业务协议的二进制内容仍可能显示为不可打印字符；合法 H485 维护帧不会转发，损坏或未完成候选的回退规则见 PROTOCOL.md。Shell 结果通过维护 TX 返回主机。

`send 'business-test'` 会申请控制权，原样发送 13 字节（不加协议头或换行），等待发送结束后释放控制权并退出；方式一随后恢复维护终端。若已有其他业务控制者则返回 `EBUSY`。该示例不等待主机业务回复；主机普通串口工具可查看原始字节，`hd485ctl.py` 不把它当作维护应答。

6. Mac 主机运行：

```sh
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 status
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 exec '@version'
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 exec 'uname -a'
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 put ./input.bin input.bin
python3 tools/hd485ctl.py --port /dev/cu.usbserial-FTAP6OK03 get input.bin ./download.bin
```

7. 测试结束，正常终止 `hd485d`，确认 DE 回到接收，再启动原相机服务并验证温控器查询恢复。本次已完成构建、传包、停原相机服务及启动 hd485d；终端模式已经用户确认通过，服务保留供后续测试。

`--allow-shell` 允许以服务进程身份执行 `/bin/sh -c`，默认关闭。`@version` 是无需开放任意 Shell 的内建命令；可在服务初始化时通过 `maint_register_command()` 注册有界、非阻塞的自定义命令处理器。文件根目录限制只约束文件协议，不构成 Shell 沙箱。

## 应用接口与运行语义

接口定义在 [include/hd485.h](include/hd485.h)。使用 `libhd485.a` 时还需链接 `libhd485_wire.a`；CMake 项目可直接链接 `hd485` target。

- `acquire(wait_ms=0)` 在其他业务控制者或等待者存在时立即返回 `EBUSY`。空闲维护正在 TX 时允许最多 2 秒交接；正数为 FIFO 等待超时，最大 1 小时。
- 控制权属于连接而非 PID；断开连接自动释放，但已接受的 TX 会先结束。只读订阅不阻止维护。默认 16 个客户端，单客户端输出队列 64 KiB。
- `write` 每次接受 1～8192 字节，返回入队字节数；`HD485_TX_DONE` 的 `a` 为 ticket，`b` 为发送错误码。待发送业务队列为 64 KiB，另有最多一个在途帧；满时返回 `EAGAIN`，应用自行重试。
- 串口接收经分类后仅将业务字节广播到所有订阅者；完整合法 H485 维护帧不广播；不会把 `read` 边界当作业务帧。慢读者丢最旧完整接收块，通过 `HD485_RX_LOST` 和接收序号报告；不会丢掉已部分发往 IPC 的消息尾部。
- SDK 每个句柄只供一个线程使用，跨线程需外部串行化。SDK 在 RPC 期间可缓存 RX／事件；调用 `poll()` 前检查 `hd485_pending()`。`hd485_next_event()` 为非阻塞，`hd485_read()` 使用连接配置的超时。
- 参数设置仅控制者可用；TX 尚未完成时返回 `EBUSY`，调用方等待完成后重试。参数失败不更新逻辑配置。释放控制权恢复启动配置，并增加配置／会话代次。
- 串口错误进入故障状态，拒绝后续发送并报告事件；需修复设备后重启服务。故障不会伪装成成功交权。

0.1.1 增加按 UART 设备号命名的原子锁文件，默认位于 `/run`；测试可用 `--lock-dir` 指定已有目录。所有生产实例应使用同一锁目录。板端实际缺少 `flock` 支持时，以此锁配合 `TIOCEXCL` 保持服务间互斥；其他 flock 错误仍拒绝启动。正常退出删除本实例锁文件，异常断电／强杀留下的锁不自动删除，需核验记录 PID 和串口占用后再处理。

## 两种模式及限制

`idle` 是默认模式：业务持有控制权时不执行维护请求，合法维护帧仍被过滤；申请控制权会取消维护命令并禁止后续维护发送。文件临时数据保留，新维护会话通过 BEGIN 恢复。

`coexist` 始终旁路识别维护协议，维护服务是唯一业务控制者之外的内部发送来源；业务写入优先，并取消正在运行的维护命令。主机必须保证业务间隙和应答时间，合法维护帧不交给业务解析器。帧头和 CRC 无法证明与任意裸流绝不碰撞，因此不宣称这种模式对任意业务无干扰。

维护只在主机请求后应答，每帧最多 256 字节载荷；默认应答窗口 500 ms，主机默认等待 800 ms。过期且尚未提交硬件的应答被丢弃，主机以相同请求编号重试。已经提交 UART 的字节无法撤回；Linux 调度与驱动排空延迟不构成硬实时保证。

命令默认超时 30 秒，stdout/stderr 各最多保存 128 KiB，超出后继续排空并标记截断。取消进程组不能撤销已产生的副作用，也不能保证清理主动脱离该进程组的后台程序；首版只面向前台命令。

文件上限 64 MiB，分块 240 字节，根目录默认 `/userdata/hd-rs485/files`。中间子目录必须已存在；拒绝绝对路径、`.`、`..`、空路径段及符号链接。失败的部分文件保留，不自动删除证据；身份不匹配的续传返回冲突。存储读写和 fsync 仍可能受文件系统延迟影响，本服务不提供硬实时调度保证。

## RV1106 业务接入

`camera_app` 0.4.52 / UART 1.0.26 已增加共享后端，ttyS3 使用 SDK 事务控制权，ttyS4 保持原实现。服务必须启用 `--rx-profile qjy`，防止误识别业务载荷内的 H485 字节。启动顺序、独立部署、测试与回退见 [RV1106_INTEGRATION.md](RV1106_INTEGRATION.md)。

## Anyka 构建与 PTY 启动

使用 `tools/build-anyka-docker.sh`，参数为 `HD485_CONTAINER` 和 Anycloud 的
`CROSS_COMPILE`。脚本自动为 GCC 5.5 设置工具链自己的宿主库搜索路径，输出
`dist/hd485-anyka-0.4.0.tar`（脚本目标版本；本轮未更新 Anyka 实板，板端仍为 0.3.0）。板端先正常停止原 UART 所有者并确认看门狗关闭，
从解包的 anyka 目录运行 `sh tools/run-anyka.sh --fs-root / --allow-shell --allow-pty`。
当前配置为 ttySAK2 / 460800 / GPIO5，具体后台启动、回退及资源数据见
[DEPLOYMENT.md](DEPLOYMENT.md)、[BOARD_PTY.md](BOARD_PTY.md)。

## RV1106 发送优化（0.4.1）

UART 1.0.27 / App 0.4.53.d 增加整帧流水发送：IPC 块仍为 8 KiB，串口使用固定 32 KiB 环形缓冲，满时背压，整帧末尾才排空和换向。线程用通知管道/条件变量唤醒，不使用忙循环。旧 WRITE 和旧服务保持兼容。协议、超时及取消语义见 [PROTOCOL.md](PROTOCOL.md#041-整帧发送与事件唤醒)。

分段计时开关为 `HD485_TX_TRACE=1`，只用于定位，正式吞吐/CPU 测量关闭。隔离包启动入口为 `sh tools/start-rv1106-optimized.sh <解包目录>`，启动前先正常停止旧 App 和 daemon；不修改 OEM 或开机脚本。

0.5.53.d 起使用 0.5.x 标识依赖 hd485d 的 App 分支；新板安装、自启动、正常停止及回退见 [hd-camera-app2_hd485d部署.md](hd-camera-app2_hd485d部署.md)。历史 0.4.53.d 性能记录保持原名。
