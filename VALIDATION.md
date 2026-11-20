# HD485 0.1.1 验证记录

## 2026-11-20：App 0.5.53.d / hd485d 0.4.1 部署验收完成

2026-11-20，xpl 确认“验证完毕”，本次 **App 0.5.53.d / UART 1.0.27 / hd485d 0.4.1** 部署用户验收完成，新版保留供调试。

已完成的 Agent 验证：集成包及内部文件校验、正常停止旧服务、OEM 安装、实际程序与库来源、业务初始化、485 版本/心跳/静态转发、Shell/文件/PTY/抢占恢复，以及板端重启自启动。重启后的重复485自动检查曾因CamDock占用而未执行；用户随后确认验收完成，不补造逐项结果。本轮仅更新验收文档并提交已有实现，没有再次操作板子或重新测量性能。

- [部署及备份记录](RV1106_DEPLOY_0.5.53.d.md)
- [0.4.1 发送优化与历史实测](TX_OPTIMIZATION.md)
- [新板完整部署步骤](hd-camera-app2_hd485d部署.md)


## 0.4.0 维护过滤与 RV1106 接入（2026-11-20）

当前验证见 [RV1106_INTEGRATION.md](RV1106_INTEGRATION.md)。0.4.0 起合法维护帧不再广播给订阅者；下列旧版“维护 RX 原样可见”是历史行为。

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

## 2026-11-19：HDTools 0.1.0 / hd485d 0.2.0

实现独立桌面应用 `HDTools/`，沿用参考 CamDock 的 Compose Desktop 技术与设备文件布局，
使用 H485 会话。新增可选 `--fs-root` 与文件系统扩展操作码，保留原 CLI／SDK 接口。
本轮没有修改外部 CamDock 或 camera_app，没有更换板端正在运行的 0.1.1 服务。

### 已执行验证

- macOS CMake/CTest：wire 测试通过；服务启用 ASan/UBSan，26 项 Python PTY 集成测试通过。
  覆盖旧接口、文件扩展启用／关闭、中文及跨帧长文件名、隐藏项、目录变化、符号链接、
  路径越界、FIFO 拒绝、权限拒绝、错误哈希、覆盖冲突、权限保留、续传和业务抢占。
  日志：`build/integration-0.2.0.log`，详细材料：`build/test-artifacts/`。
- Linux 正式 SOCK_SEQPACKET 路径：ASan/UBSan 构建、CTest 与 24 项集成测试通过。
  容器 Python 3.10 缺少 `hashlib.file_digest`，未执行既有的
  `test_file_upload_download_and_overwrite`、`test_download_resume`；这两项已包含在 Mac 26 项中。
  root 容器中 chmod 权限拒绝断言条件跳过，非 root Mac 路径已覆盖。
  日志：`build/linux-0.2.0-final.log`；隔离目录记录：`build/linux-0.2.0-path.txt`。
- Kotlin/JVM：5 项测试通过。3 项通过 TCP-to-PTY 测试桥连接实际 hd485d，验证能力协商、
  目录及链接属性、命令输出和退出码、二进制上传下载、取消与显式恢复、零字节文件、
  下载身份变化拒绝；另外覆盖帧分片／噪声／CRC、UTF-8 跨块和同请求编号重传。
  报告：`HDTools/build/reports/tests/test/index.html`。
- 实际 Compose UI 截图已检查，文件／终端 1:1 布局、工具栏、列表和命令输出正常：
  `build/hdtools-preview.png`。打包后的 Mac 应用亦运行截图入口成功：
  `build/hdtools-packaged-preview.png`；应用通过本机 `codesign --verify --deep --strict`。
- JDK 17 完成 Mac 应用及 DMG 构建，应用版本 0.1.0，Mac 安装包版本映射 1.1.0。
  构建日志：`build/hdtools-final-build.log`。
- 复用已核验的 camera_app Docker ARM GCC 8.3/uClibc 工具链完成 0.2.0 交叉编译，
  确认 ARM EABI5、hard-float、解释器 `/lib/ld-uClibc.so.0`。包及 SHA-256 位于 `dist/`，
  构建日志 `build/rv1106-0.2.0-build.log`。既有 serial.c pthread cleanup 的
  `-Wclobbered` 警告仍存在，本次新增文件系统代码没有新增交叉编译警告。

### 尚待实机验收

- 新版 RV1106 服务部署、HDTools 真实 USB-RS485 连接、闪存上的文件传输及业务抢占。
- Windows 原生 EXE 构建、安装、界面和 COM 串口验证。仓库提供构建入口，未声称在 Mac
  完成 Windows 实机验证；Windows 上 PTY 联调测试跳过，仅运行平台无关测试。
- 当前数据块仍为 240 字节，64 MiB 是容量上限，不代表已完成 64 MiB 实板性能测试。

升级及回退见 `DEPLOYMENT.md`。所有本轮生成的可执行文件、安装包、测试证据均被 Git 忽略。

## 2026-11-19：板端部署与 HDTools 0.1.1 真实链路验收

xpl 明确要求部署，释放 Tabby 日志串口后执行。通过日志口核验板型、旧进程及空间，
将校验通过的 0.2.0 包经 ZMODEM 传至新的 `/userdata/tmp_var/hd-rs485-drive-0.2.0/`。
板端 MD5 与本地一致，旧 0.1.1 PID 750 正常退出，socket／UART 锁正常释放。
新版 PID 913，`idle --allow-shell --fs-root /`，旧部署目录保留。板子没有重启，
原相机进程未运行；本轮未修改 OEM、开机配置及原相机文件。

使用 **HDTools 自身的 SerialIO / Session / RemoteDevice**，从 Mac 的
`/dev/cu.usbserial-FTAP6OK03`、230400 8N1、地址 1 验证：

- 查询 `@version`，结果 `hd485d 0.2.0`；浏览 `/` 和读取 `/userdata` 属性。
- 在本次新建的 verification 目录上传带中文和空格名字的 2048 字节二进制文件。
- 上传、下载均在 240 字节处主动取消，然后显式恢复，偏移准确，下载内容逐字节一致。
- 文件 SHA-256：`10fc3c51a152e90e5b90319b601d92ccf37290ef53c35ff92507687d8a911a08`。
- 零字节文件上传下载正常，目录刷新可见新文件。
- 通过板端 SDK 发出 `hdtools-business-test` 原始业务数据，主机收到完全一致的字节；
  旧维护会话失效，重新 HELLO 后版本命令正常，最终 `owner=0`、`error=0`、`epoch=2`。

连续请求实测暴露换向等待问题：应答后等待 0/20 ms，4 次 STATUS 各发生一次重试；
等待 60/120/250 ms，各 3 次 STATUS 均只发送一次。HDTools **0.1.1** 在会话层增加
有效应答后的 60 ms 间隔，覆盖成功和错误应答；新增对应回归用例。板端二进制保持 0.2.0。
6 项 JVM 测试通过，并重建 Mac 应用、DMG、校验文件，包内程序截图启动及 codesign 检查通过。
同一整组真实链路验收在修正前耗时 89.267 秒，修正后 7.612 秒，全部通过；此时间包含
目录、命令、文件及取消恢复步骤，不是单独的传输吞吐基准。

证据：`build/rv1106-deployment-0.2.0.log`、`build/rv1106-0.2.0-transfer.log`、
`build/rv1106-preemption-0.2.0.log`、`build/hdtools-live-board-0.1.1.xml`、
`build/hdtools-0.1.1-jvm-tests.xml`、`build/hdtools-0.1.1-build.log`。
实机测试 Kotlin 源码与临时 Gradle init 文件保留在
`~/Documents/tmp/hd-rs485-drive/live-hdtools-0.2.0/`，不会进入默认测试或触发自动连接。
本次文件保留于板端新部署目录下的 `verification-20261119` 和 `verification-20261119-fast`。

本节更新前一节“尚待实机验收”的状态：真实 485 基本功能、小文件与一次业务抢占恢复
已经通过；Windows COM、64 MiB 大文件吞吐及长期稳定性仍未验收。运行与回退见 DEPLOYMENT.md。

## 2026-11-19：HDTools 0.1.2 串口读写模式

按 xpl 确认，读模式指发送请求后自动等待应答，非手动纯监听模式。
新增连接栏下的串口模式、读取倒计时、当前尝试次数及“取消读取”按钮。
会话层用单请求占用标志拒绝并发写入；分片、错误 CRC、非当前请求应答不能提前
退出读取。完整有效应答或每次超时退出读模式，之后才允许重试；手动取消使当前任务
结束，阻止重试和 finally 中的文件关闭请求。显式开始新操作后恢复发送。
“取消任务”与“取消读取”分别保留，取消接收不承诺终止板端已开始的命令。

验证结果：

- 11 项 JVM 回归通过（原 6 项 + 新 5 项串口状态测试）。覆盖完整帧判定、超时每次
  退出读模式、并发写入拒绝、手动取消快速退出、取消后禁止重试／清理写入、显式恢复、
  错误应答及关闭后的状态保持。
- 真实 RV1106：使用 HDTools 串口实现，在 STATUS 请求进入读模式时取消，确认当前
  等待结束、额外清理请求被拒绝；显式恢复后 STATUS 和 `@version` 正常。
  使用已部署的 hd485d 0.2.0，仅作状态／版本读取，无需重新部署板端。
- 读取中控件的 Compose 渲染及完整应用截图已检查；Mac 应用、DMG 生成成功，
  包内程序启动截图及 codesign 校验通过。

证据：`build/hdtools-0.1.2-test.log`、`build/hdtools-0.1.2-integration.xml`、
`build/hdtools-0.1.2-serial-mode.xml`、`build/hdtools-0.1.2-live-read.xml`、
`build/hdtools-0.1.2-read-controls.png`、`build/hdtools-0.1.2-packaged-preview.png`。
Mac 包：`HDTools/build/compose/binaries/main/dmg/HDTools-1.1.2.dmg`，另附 SHA-256。
Windows 构建与 COM 实机验收仍未执行。本轮没有 Git 提交。

## 2026-11-19：修复终端 cd（hd485d 0.2.1 / HDTools 0.1.3）

根因：原 EXEC 每次启动独立 Shell，子进程的 cd 不影响下一条命令。新增会话目录描述符，
每个 Shell 子进程从该目录启动，通过独立 FD 9 回传正常退出时的物理／逻辑路径及 OLDPWD。
目录元数据不混入 stdout/stderr；保留原退出码。Mac /bin/sh 会重置继承的 OLDPWD，且
pwd -P 可能修改 PWD，分别以显式参数恢复 OLDPWD和子 Shell
探测物理路径处理。重连／业务抢占重置目录，取消／超时不提交变更。

- macOS ASan/UBSan：CTest 与 29 项集成测试通过；新增 cwd 测试覆盖绝对／相对路径、
  空格／引号／换行名字、cd -、逻辑链接、实际相对路径写文件、非零退出状态、失败 cd、
  取消／超时及新会话恢复。
- Linux ASan/UBSan 正式 Socket：27 项通过；容器 Python 3.10 不执行两个依赖
  file_digest 的旧 CLI 用例，这两项已在 Mac 覆盖。目录变化测试改为显式改变 mtime，
  避免文件系统时间粒度导致原测试偶发未触发变更检查；生产目录接口未修改。
- HDTools：11 项 JVM 回归通过，实际服务互通测试增加同一会话 cd 后 pwd 校验；
  根据 FS_INFO 新增能力位显示是否保留目录。Mac 0.1.3 / DMG 1.1.3 已打包、截图启动和
  codesign 检查通过。
- RV1106：交叉编译包经 ZMODEM 传至新目录，MD5 核验一致。停止旧 0.2.0，启动
  0.2.1（PID 965），mode=idle、owner=0、error=0。未重启或修改原相机／开机配置。
- 使用 HDTools 的真实串口会话实机复现用户步骤：初始 pwd 为新部署目录；cd /userdata
  后 pwd=/userdata；cd tmp_var 后 pwd=/userdata/tmp_var；cd - 回到 /userdata；
  无效 cd 返回非零且保持原目录；`cd /userdata/tmp_var; printf ok; exit 7` 的输出与
  退出码准确且目录保留；重新连接后回到新部署目录。全部通过。

证据：`build/integration-0.2.1.log`、`build/linux-0.2.1-final.log`、
`build/hdtools-0.1.3-test.log`、`build/rv1106-0.2.1-build.log`、
`build/rv1106-0.2.1-transfer.log`、`build/rv1106-deployment-0.2.1.log`、
`build/hdtools-live-cwd-0.2.1.xml`。实机 Kotlin 测试源保留于
`~/Documents/tmp/hd-rs485-drive/live-cwd-0.2.1/`，不进入默认测试。

这是工作目录保持，不是完整交互式 Shell；export、函数和 alias 不跨请求保留。
显式覆盖内部 FD 9／EXIT trap 或 exec 替换 Shell 可能无法回报目录，详见 PROTOCOL.md。
Windows 仍未实机验收。本轮未提交 Git。


## 2026-11-19：PTY 0.3.0 / HDTools 0.2.0 双板验收

先按授权提交基线 394acb9，再实现单 PTY、输入偏移/输出确认、有界缓冲、超时及
业务抢占回收；HDTools 使用 JediTerm 3.72 提供交互屏幕。本节记录提交前的实现与验证结果，后续确认见文末。

### 自动验证

- macOS：CTest 通过，33 项 Python 集成通过（包含 4 项新增 PTY 用例）。
- Linux：CTest 通过，31 项 Python 集成通过；Python 3.10 不执行两个依赖
  hashlib.file_digest 的旧 CLI 用例，这两项在 Mac 已通过。初次并行运行共享 Socket
  产生冲突，清理测试进程后串行重跑通过；build/pty-linux.log 是初次失败记录，不能
  将其作为通过证据。最终修复后的串行运行 31 项耗时 47.010 秒，记录在 build/pty-linux-full-final.log。
- HDTools：13 项 JVM 用例通过（8 项集成、5 项串口状态）。包含真实服务互通及
  JediTerm 屏幕解析，覆盖 PTY、普通命令/文件互斥和原读写状态回归。
- Mac 应用和 DMG 1.2.0 构建成功、codesign 校验通过。Windows 未实机验收。

证据：build/pty-full-mac-final.log、build/pty-linux-full-final.log、build/pty-desktop-test-final.log、
build/pty-final-TEST-com.hdtools.{IntegrationTest,SerialModeTest}.xml。

### 两板实测

- RV1106：ttyS3 / 230400 / GPIO35；Anyka：ttySAK2 / 460800 / GPIO5。
  两板均运行方式一 idle，启用 --allow-shell --allow-pty --fs-root /。
- 通过真实 RS485 打开 Shell，验证交互输入、窗口设置、Ctrl+C、top、vi 保存并回读
  64 KiB 文件的修改。使用 HDTools 的 SerialIO/Session/PtyConnection/JediTerm 组件
  再次验证真实屏幕，2 项通过；保存 vi/top 截图。该测试覆盖实际终端组件，未冒充
  全部 Compose 按钮的人工点击验收。
- 各内存场景持续 60 秒采样，服务/Shell/前台应用分列 RSS，并计算 PSS 合计。
  连续开关 20 次及 21 秒断线超时后，PTY 数为 0、服务 FD 为 11，没有残留测试子进程。
  RV1106 稳定 RSS 956 KiB，Anyka 980 KiB；未观察到循环持续增长。
- SDK 发送 PTY_PREEMPT_CHECK，旧维护会话失效；业务释放后显式 HELLO 重连正常，
  两板最终 owner=0、error=0。没有自动抢回业务控制权。
- 使用真实 HDTools 串口会话，发送请求进入读模式后手动取消；2 项通过，确认收到
  ReadCancelled 且 400 ms 观察期内无重试/轮询/清理写入。板端随后按租约超时回收。
- Anyka 原厂应用正常退出，日志确认 watchdog stopped；两板 boot_id 未变，保留
  新服务运行供调试。未改原程序或开机配置。已停止原相机，尚未进行并行业务峰值测试。

证据：build/pty-{rv1106,anyka}-audit-final-release.{json,log}、
build/pty-{rv1106,anyka}-preemption-final.log、build/pty-desktop-live.xml、
build/pty-desktop-live-cancel.xml、build/pty-live-{rv1106,anyka}-{vi,top}.png。
完整资源表见 BOARD_PTY.md，启动/回退见 DEPLOYMENT.md。
实机 Kotlin 源码及 Gradle init 保存在 ~/Documents/tmp/hd-rs485-drive/pty-live/，
不会让默认测试自动连接串口；板端采样和抢占脚本在 tools/pty_*audit.py，需显式指定设备。

### 交付指纹

Release 构建，未额外 strip；PTY 不新增板端动态库依赖。以下为实际运行的服务：

| 平台 | hd485d 字节 | SHA-256 |
|---|---:|---|
| RV1106 | 71,524 | 281d51fa6094217d2cc44ba5849d1213dd34bdf24ea116d7876ff195f6c57f92 |
| Anyka | 58,700 | 5bcd9524b45e3a599d940ff6e80de008685458417fedda5b01c3bcd9c4c9494e |

最终边界检查补充：关闭/超时释放输出后，迟到的 IO 确认被拒绝，避免长度下溢。
该用例已加入既有关闭/超时测试；Mac 33 项及 Linux 31 项均重新通过。
两板随后更新最终二进制，重新进行场景测量与会话回收验收。
真实串口也验证了关闭后的迟到确认返回 EINVAL，服务仍能执行 @version；
证据 build/pty-live-late-ack.log。

最终交付包已补入本次板卡/部署文档和 Python macOS 460800 支持，服务二进制不变。
安装总字节是包内普通文件长度合计（含 SDK 静态库、示例、工具和文档），不是文件系统块占用。

| 包 | 安装总字节 | tar 字节 | tar SHA-256 |
|---|---:|---:|---|
| rv1106 | 166,247 | 184,320 | acc191fe8681372e6d568f7a13520b62c843837f06adaf121c880eed2e230cef |
| anyka | 155,813 | 174,080 | 273f77d2be7c514ec44589d3f0dfc51c330e04980c80fac13fb0aa7c9c7515c1 |

Mac DMG：HDTools/build/compose/binaries/main/dmg/HDTools-1.2.0.dmg，75,651,992 字节，
SHA-256：cac4f0b196a774c158eea6f32d6792d41840834f24a96db7b3a2c2e508ba74a8。
每个包附 .sha256；详细验证记录位于源码仓库，板端包附 BOARD_PTY.md / DEPLOYMENT.md。


### 最终交接状态

两板均保留最终 0.3.0 服务：RV1106 PID 1921，Anyka PID 3926；owner=0、error=0，
PTY=0、服务 FD=11。boot_id 与最初检查一致，未发生重启。最终包与运行二进制的板端
MD5 均与主机一致，更新文档时仅解包文档/工具，没有覆盖运行中的可执行文件。
两板版本目录另存 resource-results.txt；原始采样和完整证据保留在主机 build/。

在完成后续 GUI、抢占、迟到确认及手动取消测试后，最终单次 RSS 为 RV1106 964 KiB、
Anyka 984 KiB（PSS 分别 603/984 KiB）。这与 60 秒分场景采样的关闭值属于不同阶段，
完整记录于 build/pty-final-handoff.log；没有把单次变化直接解释为泄漏或内存上限。

最终包 MD5：RV1106 fca897cd396929471d3a148dde4252da；
Anyka b7dcfbd9912b56930df57e690b20b66d。传输日志为
build/pty-{rv1106,anyka}-final-transfer.log；退出所有测试程序后，主机四个指定串口
及对应 tty 别名均无占用，可直接用 HDTools 连接。该次实机交接时尚未提交 PTY 改动。


## 2026-11-20：用户验证确认与源码提交

xpl 确认“验证完毕”，授权更新文档并提交 Git。本次提交包含 hd485d 0.3.0 PTY、
Anyka 平台适配、HDTools 0.2.0 交互终端，以及协议、部署、资源和架构记录。
新增 ARCHITECTURE.md，补齐架构图、控制状态图、PTY 时序图及源码入口；
BOARD_PTY.md 增加内存/磁盘摘要，修正文档中的 HDTools 应用与安装包版本映射。

用户未补充逐项测试清单；技术验收范围沿用前述记录，不扩展为 Windows COM、
相机并行峰值、任意大文件吞吐或长期稳定性均已验收。
本次仅更新文档并核对既有测试证据；生产代码保持双板最终验收时的版本。
已有结果：Mac 33 项、Linux 31 项集成，13 项 JVM，以及双板真实终端、取消读取、
业务抢占、20 次回收和超时测试通过。未重新连接板卡或改变部署状态。

交付包哈希仍对应 2026-11-19 已部署的产物；本次文档更新未重新打包或上传。
构建产物、安装包与原始测试证据继续按 .gitignore 留在本地；本次仅本地提交，不执行 push。

## 0.4.1 TX 优化回归

新增 `tests/stream_tx.py`：8182/8183/8192/8193/65536/524298 字节、流式缓冲背压、并发 STATUS、不完整帧取消/超时、权限/长度校验、owner 断线和后续旧 WRITE。`tests/stream_board.c` 仅供显式隔离板测试，abort 会发出合成测试字节，不能在真实业务事务中调用。

详细实现、实板测量和交付边界见 [TX_OPTIMIZATION.md](TX_OPTIMIZATION.md)。
