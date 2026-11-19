# HD485 0.1.1

单 RS485 通道的用户态共享服务。包含 C 服务 `hd485d`、C SDK、Python 主机工具和业务示例；原有业务字节不封装，应用通过 SDK 申请唯一控制权，其他应用可以订阅原始接收流。

## 当前状态

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

脚本在本工作区生成 `dist/hd485-rv1106-0.1.1.tar` 及 SHA-256 校验文件，不执行部署。包中包含服务、示例、SDK 静态库和头文件、配置与运行脚本。包使用普通 tar，不依赖板端 gzip。

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

`echo` 示例在独占控制期间原样回送收到的数据，只适用于隔离的回环测试；在共存模式中会回送维护帧，不作为真实业务协议实现。

`listen` 只订阅并原样打印 RX，不解析维护协议。看到 `H485`、命令文本及不可打印字符是二进制请求帧的直接显示；Shell 执行结果经 TX 返回主机，应在 Mac 的 `hd485ctl.py exec` 窗口查看。只读监听不会申请控制权，因此不阻止方式一终端工作。

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
- 串口收到的字节广播到所有订阅者，包括共存模式的维护请求；不会把 `read` 边界当作业务帧。慢读者丢最旧完整接收块，通过 `HD485_RX_LOST` 和接收序号报告；不会丢掉已部分发往 IPC 的消息尾部。
- SDK 每个句柄只供一个线程使用，跨线程需外部串行化。SDK 在 RPC 期间可缓存 RX／事件；调用 `poll()` 前检查 `hd485_pending()`。`hd485_next_event()` 为非阻塞，`hd485_read()` 使用连接配置的超时。
- 参数设置仅控制者可用；TX 尚未完成时返回 `EBUSY`，调用方等待完成后重试。参数失败不更新逻辑配置。释放控制权恢复启动配置，并增加配置／会话代次。
- 串口错误进入故障状态，拒绝后续发送并报告事件；需修复设备后重启服务。故障不会伪装成成功交权。

0.1.1 增加按 UART 设备号命名的原子锁文件，默认位于 `/run`；测试可用 `--lock-dir` 指定已有目录。所有生产实例应使用同一锁目录。板端实际缺少 `flock` 支持时，以此锁配合 `TIOCEXCL` 保持服务间互斥；其他 flock 错误仍拒绝启动。正常退出删除本实例锁文件，异常断电／强杀留下的锁不自动删除，需核验记录 PID 和串口占用后再处理。

## 两种模式及限制

`idle` 是默认模式：业务持有控制权时不解析维护帧；申请控制权会取消维护命令并禁止后续维护发送。文件临时数据保留，新维护会话通过 BEGIN 恢复。

`coexist` 始终旁路识别维护协议，维护服务是唯一业务控制者之外的内部发送来源；业务写入优先，并取消正在运行的维护命令。主机必须保证业务间隙和应答时间，业务解析器必须容忍维护帧。帧头和 CRC 无法证明与任意裸流绝不碰撞，因此不宣称这种模式对任意业务无干扰。

维护只在主机请求后应答，每帧最多 256 字节载荷；默认应答窗口 500 ms，主机默认等待 800 ms。过期且尚未提交硬件的应答被丢弃，主机以相同请求编号重试。已经提交 UART 的字节无法撤回；Linux 调度与驱动排空延迟不构成硬实时保证。

命令默认超时 30 秒，stdout/stderr 各最多保存 128 KiB，超出后继续排空并标记截断。取消进程组不能撤销已产生的副作用，也不能保证清理主动脱离该进程组的后台程序；首版只面向前台命令。

文件上限 64 MiB，分块 240 字节，根目录默认 `/userdata/hd-rs485/files`。中间子目录必须已存在；拒绝绝对路径、`.`、`..`、空路径段及符号链接。失败的部分文件保留，不自动删除证据；身份不匹配的续传返回冲突。存储读写和 fsync 仍可能受文件系统延迟影响，本服务不提供硬实时调度保证。

## 后续集成边界

当前 `camera_app` 仍直接操作串口与 GPIO，不能与 `hd485d` 同时运行。接入真实业务时需将其 UART 层改成 SDK 客户端，保留原始协议解析，移除该客户端的物理 UART／DE 操作，再验证方式二维护帧的兼容性。该变更位于外部工作区，本次未执行。
