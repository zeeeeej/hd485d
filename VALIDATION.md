# HD485 0.1.1 验证记录

日期：2026-11-19。已完成 ARM 交叉构建、Linux 正式监听路径测试和 RV1106 部署启动，部署模式为方式一（idle）。外部参考工程源码未修改。Agent 的板端操作经独立日志串口完成；主机侧真实 RS485 终端测试由 xpl 执行并确认通过。

## 用户验收：方式一终端模式通过

2026-11-19，xpl 确认：“终端模式验证了下 是ok的。”本次确认对应已部署的 **hd485d 0.1.1、方式一（idle）、ttyS3、230400 8N1、GPIO35**。

用户提供的操作包括主机执行 `hd485ctl.py exec 'ls -al'`，以及板端 SDK `status`、`listen`。`listen` 原样显示含 `H485` 帧头的二进制 RX 是预期行为，Shell 结果由主机工具解码显示。

本次记录为用户确认的终端模式验收，不新增自动测试结论。用户未提供完整逐项测试清单，文件双向传输／续传、业务控制权交接、方式二、性能及恢复原相机业务仍待专项验证。本轮仅更新文档，版本保持 0.1.1。

## 权限开放后的构建与部署结果

| 检查 | 实际结果 |
|---|---|
| 交叉构建 | x-luckfox-pro 容器，GCC 8.3.0，ARM EABI5 hard-float，uClibc；完整链接和打包成功 |
| 0.1.1 Linux 正式 IPC | CTest 向量测试及 19 项 PTY 集成测试通过，包含真实 Unix SOCK_SEQPACKET 监听／连接及缺少 flock 的兼容路径，19.409 秒 |
| 0.1.1 macOS 针对性回归 | flock 缺失、独占权限、串口故障三项通过；退出时检查锁文件已清理 |
| 包传输 | 通过 115200 日志口 ZMODEM 传入独立调试目录；板端 MD5 与宿主一致 |
| 板端启动 | QJY RV1106 V1，运行内核 5.10.226；0.1.1 在 ttyS3 / GPIO35 上成功初始化 |
| 本地 SDK | 板端查询 `mode=0 owner=0 epoch=0 tx=0 error=0 rx=0`，控制权空闲且无故障 |
| 现场状态 | 原相机应用正常退出，日志确认看门狗 disabled；保留 hd485d 供用户验证，未修改开机配置 |

Linux 容器 Python 为 3.10.12，主机工具的完整 put/get 校验接口要求 Python 3.11+，因此本轮没有在 Linux 重跑 `test_download_resume` 和 `test_file_upload_download_and_overwrite`；两项此前已在 Mac Python 3.12 的 20 项完整套件中通过。Linux 本轮仍覆盖底层上传续传、错误哈希、下载源变化和路径校验。

Linux 原始测试证据已取回 `build/linux-validation-0.1.1/`；板端状态记录为 `build/rv1106-deployment-0.1.1.log`；ARM 构建日志为 `dist/build-rv1106.log`。交付包哈希、目录和恢复命令见 `DEPLOYMENT.md`。

首次 0.1.0 板端启动失败后已自动恢复相机服务。独立探测确认：`flock=-1/ENOSYS`，`fcntl(F_SETLK)=-1/EACCES`，`pthread_create=0`。0.1.1 增加按 UART 设备号命名的原子锁文件，只有 flock 明确不支持时使用兼容路径，并保留 TIOCEXCL；修复后重新部署成功。正常退出清理锁，异常退出残留锁须核验后人工处理。

交叉编译仍有 GCC 8.3 对 pthread cleanup 宏及局部变量的 `-Wclobbered` 警告；原生 GCC 11 对哈希函数数组／指针参数声明有 `-Warray-parameter` 警告。它们未导致构建失败，未将本次构建表述为无警告。

## 初期 macOS 验证记录（0.1.0）

| 检查 | 结果与边界 |
|---|---|
| macOS CMake 构建 | AppleClang 21，C11，启用 AddressSanitizer 和 UndefinedBehaviorSanitizer |
| CRC32／SHA-256／帧解析 | CTest 标准向量、帧截断与损坏检查通过 |
| 端到端集成 | 20 项通过，PTY＋继承 Socket；未报告 ASan／UBSan 错误 |
| 静态分析 | Clang analyzer 对全部服务、SDK、协议及哈希源码未报告问题 |
| Linux/ARM 分支 | 使用 RV1106 ARM/uClibc sysroot，`clang --target=arm-linux-gnueabihf ... -Werror -fsyntax-only` 通过；不等于完成链接或运行 |

20 项集成测试覆盖：

- 独占冲突、只读越权、FIFO 等待与超时、控制者断连释放。
- 多读者收到相同原始字节、慢读者队列溢出和隔离、发送完成事件。
- 参数变更／失败／释放后恢复，串口断开故障报告。
- 空闲模式业务期间禁用维护、业务抢占命令、共存模式保留原始维护 RX。
- 命令 stdout/stderr／退出码、注册命令、输出上限、超时、重复 EXEC 去重。
- 噪声、CRC 损坏、分片请求、8192 字节 TX 完整性。
- 文件双向 SHA-256、覆盖控制、上传与下载续传、错误哈希拒绝提交。
- 路径穿越、符号链接拒绝、下载源变化、拒绝硬链接暂存文件后不能继续写入。
- 实际 C SDK 示例的状态查询和发送／释放链路。

0.1.0 完整通过的原始证据目录为 `build/test-artifacts/1795060247286006000/`，20 项测试用时 19.428 秒；目录中每个用例保留服务日志和传输文件。0.1.1 macOS 三项针对性回归证据为 `build/test-artifacts/1795061409855999000/`。

## 测试中修复的问题

- 发送线程在完成确认与下一次提交之间可能错过唤醒：将等待条件改成完成结果是否已消费，避免下一笔 busy 状态覆盖确认。
- 上传 BEGIN 失败后可能残留打开的文件句柄：失败路径统一清理，新增硬链接暂存文件的回归测试。
- 新维护会话重建时关闭旧文件句柄，保留可恢复的磁盘文件，避免复用失效下载快照。
- 串口初始化失败时，仅撤销本实例成功取得的 `TIOCEXCL` 状态。
- 测试中不以固定 50 ms 等待假设子进程已经输出，改成协议规定的轮询完成条件。

## 初期权限阻塞记录（现已解除）与未执行项

| 项目 | 初期状态 |
|---|---|
| 正式监听端点集成 | 沙箱下 `bind(AF_UNIX)` 返回 `Operation not permitted`；继承 Socket 测试只覆盖连接后的处理逻辑 |
| RV1106 完整构建 | Docker API 连接 `~/.docker/run/docker.sock` 被拒绝；现有 GCC 为 Linux ELF，无法直接在 macOS 执行 |
| 板端只读检查与部署 | 打开 `/dev/cu.wchusbserial144440` 即返回 `Operation not permitted`，未设置串口参数或发送命令 |
| 真实 RS485 验证 | 尚未执行 GPIO35、真实 tcdrain、换向延迟、文件传输和业务恢复验证 |

权限开放后，正式监听测试、ARM ELF 构建和板端启动均已完成；随后 xpl 确认方式一终端模式通过。真实 RS485 性能／丢包／交接延迟及双向传输尚未获得用户验收确认。

设计阶段曾观察到 CamDock 占用 `/dev/cu.usbserial-FTAP6OK03`，后续 lsof 未显示占用；设备占用会变化，实际调试仍须重新核验，不沿用历史 PID。

## 后续用户验收

1. 方式一终端模式已确认通过；继续按 `DEPLOYMENT.md` 验证文件传输及业务控制权交接。
2. 根据测试结果记录真实链路延迟、吞吐和错误情况；方式二仍需独立的主机调度条件验证。
3. 用户结束测试后恢复原相机服务并验证温控器查询，随后再决定真实业务 SDK 集成。

## 对照 camera_app 构建环境

已读取外部 `camera_app/build_all.sh`、APP／UART 的 CMake 配置及 `AGENTS.local.md`。它们通过 `CROSS_COMPILE` 选择 `arm-rockchip830-linux-uclibcgnueabihf` 工具链，在 Mac 上通过 Linux Docker 容器执行；该 GCC 的宿主格式确认为 Linux x86-64 ELF。

本项目新增 `tools/build-rv1106-docker.sh`，复用上述环境但使用独立源码快照。权限开放后已核验并使用容器 `x-luckfox-pro` 及工具链前缀 `/workspace_ai/luckfox/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-`。0.1.1 构建快照位于容器 `/root/Documents/tmp/hd-rs485-drive/build.IIxuEh`，宿主构建证据位于 `~/Documents/tmp/hd-rs485-drive/build.UuLWF4`；未覆盖容器中的另一份 camera_app。
