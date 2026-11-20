# RV1106 / Anyka 板卡与 PTY 记录

2026-11-20 RV1106 新增 0.4.0 业务接入，当前实测与资源见 [RV1106_INTEGRATION.md](RV1106_INTEGRATION.md)。Anyka 本轮未更新；下述为 PTY 0.3.0 的历史数据。

此前更新日期：2026-11-20（实机测量日期：2026-11-19）。两板已部署 hd485d 0.3.0，HDTools 0.2.0 支持交互 PTY。
均完成 vi/top、回收、业务抢占与手动取消读取验证，保留新版运行供调试。

xpl 于 2026-11-20 确认验证完毕。资源摘要如下，完整测量口径与场景见后文：

| 项目 | RV1106 | Anyka |
|---|---:|---:|
| hd485d 可执行文件 | 71,524 字节（69.8 KiB） | 58,700 字节（57.3 KiB） |
| 冷启动、无会话 RSS | 936 KiB | 960 KiB |
| 全部测试结束、无会话 RSS（单次） | 964 KiB | 984 KiB |
| hd485d + Shell + vi 编辑 64 KiB 文件的合计 PSS 峰值 | 1,563 KiB | 1,350 KiB |

文件大小不含 SDK、文档、日志及用户文件，也不是文件系统块占用。RSS 包含共享页，
PSS 按比例分摊共享页，两者不能直接相减。无新增板端动态库依赖；测试时原相机业务停止。
最终单次读数及证据见 [VALIDATION.md](VALIDATION.md)。

## 板卡与通信

| 项目 | RV1106 | Anyka |
|---|---|---|
| 硬件 | QJY RV1106 V1 | AK3918AV130 / ARM926EJ-S / ARMv5TEJ |
| 内核 | 5.10.226 | 4.4.302-cip94 |
| 日志口 / 115200 8N1 | /dev/cu.wchusbserial144440 | /dev/cu.wchusbserial1410 |
| 主机 485 口 | /dev/cu.usbserial-FTAP6OK03 | /dev/cu.wchusbserial144420 |
| 板内 UART | /dev/ttyS3 | /dev/ttySAK2 |
| 当前速率 | 230400 8N1 | 原厂进程实测 460800 8N1 |
| DE | GPIO35，高发送低接收 | GPIO5，高发送低接收；实测输出低 |
| PTY | 真实交互 vi/top、窗口设置、Ctrl+C、回收通过 | 真实交互 vi/top、窗口设置、Ctrl+C、回收通过 |
| 命令 | BusyBox sh / vi / top | BusyBox 1.30.1，sh / vi / top / script |
| 现状 | hd485d 0.3.0 / PID 1921，原相机停止 | hd485d 0.3.0 / PID 3926，原厂应用正常停止 |

Anyka 初次检查时原厂进程持有 ttySAK2、GPIO5 和 /dev/watchdog。经 xpl 授权正常停止，
日志确认 watchdog stopped 后接管串口；原厂程序文件和开机配置保持完整。
root 日志口登录已验证，本文不保存登录凭据。

## Anyka 485 参考

用户指定参考：`/Users/xiangpengle/Documents/tmp/hd-camera-baishi`。
参考证据包括 `pepsi-anyko-commit.txt`、`new-board-7131b/check_protocol.py`、
`deploy-0.1.27/app.sh`、`fe-100/sdk/include/fridge_ipc.h`。
构建依赖定位到源工程 `/Users/xiangpengle/Documents/ai/hd-camera-baishi/PepsiApp`，
只读核对了 `src/main.c`、`src/hal_board.c` 和 `tools/pepsi_client.py`。

- 百事客户应用使用 **230400**，原厂当前实际为 **460800**；不能混用。首轮 HD485
  Anyka 调试沿用当前板端 460800。SDK 可选工厂模式默认也是 460800，但客户协议不同。
- 客户应用持有 UART/termios/GPIO5，SDK 普通 init 不接管串口。共享服务集成需要让
  原应用经 SDK 申请控制权，不能两个进程直接打开 UART。
- 参考发送线程临时采用 SCHED_FIFO 优先级 1，拉高 DE，检查 TIOCOUTQ 与
  TIOCSERGETLSR/TIOCSER_TEMT，排空后忙等 2 ms 尾部保护再切回接收；失败也恢复
  GPIO 和调度策略。较大软件队列等待时让出 CPU。
- 历史客户端 5 ms 换向间隔仅供参考；不能直接替换 RV1106 已实测的 60 ms 主机间隔。
- 历史长包存在局部丢失/无响应调查记录，不将其描述为已解决。本轮 HD485 链路基本
  交互已验证，未进行任意长度持续传输或业务峰值验收。

## 编译环境

Linux Docker 容器 `x-luckfox-pro`；宿主
`/Users/xiangpengle/Documents/docker_workspace_ai` 映射 `/workspace_ai`。

- RV1106：`/workspace_ai/luckfox/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-`，GCC 8.3、hard-float/uClibc。
- Anyka 宿主工具链：`/Users/xiangpengle/Documents/docker_workspace_ai/hd_camera/prebuilt/qjy-anyko/arm-anycloud-linux-uclibcgnueabi`。
  容器对应 `/workspace_ai/hd_camera/prebuilt/qjy-anyko/arm-anycloud-linux-uclibcgnueabi`，
  `bin/arm-anycloud-linux-uclibcgnueabi-`，GCC 5.5、ARM926EJ-S、soft-float、uClibc 1.0.31。
  构建进程设置 `LD_LIBRARY_PATH=<工具链根>/lib`，否则 cc1 缺少 libmpfr.so.4。
  posix_openpt/grantpt/unlockpt/ptsname/TIOCSWINSZ 编译链接已通过；不需要更改系统库。

两平台二进制不能互换。外部工程不修改，构建快照和临时证据放 ~/Documents/tmp/hd-rs485-drive。

## 初次检查基线与测量方法（历史）

| 指标 | RV1106 0.2.1 | Anyka |
|---|---|---|
| hd485d 文件 | 66,624 字节（初次检查产物） | 待交叉构建 |
| 交付 tar | 153,600 字节 | 待交叉构建 |
| hd485d RSS | 952 KiB | 尚未部署 |
| 控制台 Shell RSS | 1,152 KiB，其中匿名 228 KiB | 704 KiB |
| Linux MemTotal | 157,280 KiB | 33,484 KiB |
| MemAvailable | 142,564 KiB | 19,520 KiB |
| 测量负载 | 原相机停止 | 原厂 fridge_ipc 在运行，非峰值验收 |

PTY 会话的 **2～4 MiB** 仅为初期预留预算，不是实测或强制上限。
PTY 实现后分别记录构建版本、优化/strip 状态、服务/新增依赖/安装总大小/包大小及哈希，
与同平台同条件基线比较。记录无会话、空闲 Shell、top、vi 编辑固定 64 KiB 文件、
关闭后的服务与子进程 RSS、可获取的 PSS、系统 MemAvailable。各场景采样 60 秒并保留
最大值，RSS 总和不视为新增物理内存。连续开关 20 次检查增长、FD、PTY 和子进程回收。
业务停止的验收与实际业务负载峰值验证分开。实现后的实测结果见下文。

## 部署与启动

两板 0.3.0 的准确目录、启动参数、校验及回退见 [DEPLOYMENT.md](DEPLOYMENT.md)。
Anyka 根和 /usr 为只读 squashfs，/userdata 为可写 jffs2；部署前剩余 7,716 KiB。
两板使用独立版本目录，未刷系统、未修改开机配置。重启仍按原配置启动原业务。

本次顺序：交叉构建并校验 ABI → 日志口传输与校验 → 确认旧进程正常退出及资源释放 →
启动 hd485d → 核对 Socket/日志/能力 → 实机交互和资源测量 → 保留服务供调试。

PTY 启动顺序：hd485d 初始化串口，进入方式一 idle → 用户打开交互终端时创建
PTY/Shell → 主机轮询输入输出 → 主动关闭、业务抢占或断线超时回收会话。
连接期间保留 Shell 环境；断线后显式重连，不恢复旧会话。

## 0.3.0 实现与交付

基线提交：394acb9。新增单 PTY 会话，4 KiB 输入/64 KiB 输出队列，收到明确输出确认后
释放；无会话不保留这两块动态缓冲。HDTools 0.2.0 使用 JediTerm 3.72 处理交互屏幕，
Mac 包版本 1.2.0。断线默认 20 秒后回收；业务抢占立即撤销交互权限，进程异步清理。

| 文件 | 0.2.1 基线（字节） | 0.3.0（字节） | 增量 |
|---|---:|---:|---:|
| RV1106 hd485d | 66,624 | 71,524 | 4,900 |
| Anyka hd485d | 49,636 | 58,700 | 9,064 |

均为 CMake Release 产物，未额外执行 strip，不包含调试符号构建；不是磁盘块占用。
Anyka 基线由提交 394acb9 使用相同工具链与 CPU/soft-float 选项重新构建。PTY 使用已有
libc 接口，不新增板端 libutil、终端模拟器或 SSH 依赖；JediTerm 只安装在主机 HDTools。

### 内存实测（KiB）

各场景持续采样 60 秒，约 47～50 个样本；下表是样本最大值。RSS 分列展示，合计列
使用各进程 PSS 之和。PSS 会受共享库的其他使用者影响，不等同于绝对新增物理内存；
内核 PTY 缓冲也不在进程 PSS 内。

| 板卡/场景 | hd485d RSS | Shell RSS | 前台程序 RSS | 合计 PSS 峰值 |
|---|---:|---:|---:|---:|
| RV1106 旧版 0.2.1 | 952 | — | — | 591 |
| RV1106 新版冷启动无会话 | 936 | — | — | 575 |
| RV1106 Shell | 956 | 1,384 | — | 1,270 |
| RV1106 top | 956 | 1,384 | 812 | 1,424 |
| RV1106 vi / 64 KiB 文件 | 956 | 1,384 | 948 | 1,563 |
| RV1106 关闭会话后 | 956 | — | — | 595 |
| Anyka 旧版 0.2.1 | 952 | — | — | 952 |
| Anyka 新版冷启动无会话 | 960 | — | — | 960 |
| Anyka Shell | 980 | 700 | — | 1,156 |
| Anyka top | 980 | 700 | 576 | 1,256 |
| Anyka vi / 64 KiB 文件 | 980 | 700 | 648 | 1,350 |
| Anyka 关闭会话后 | 980 | — | — | 980 |

系统 MemAvailable 最小值（KiB）另行记录，不用它直接计算 PTY 增量：

| 场景 | RV1106 | Anyka |
|---|---:|---:|
| 无会话 | 142,352 | 20,372 |
| Shell | 141,792 | 20,248 |
| top | 141,656 | 20,152 |
| vi / 64 KiB | 141,556 | 20,064 |
| 关闭后 | 142,220 | 20,328 |

上表采用最终边界修复后重新启动和测量的数据；RV1106 冷启动 RSS 936 KiB，完成
交互后为 956 KiB，不能把冷启动较低值解释为优化收益。关闭后分配器保留部分页是正常现象，需要结合循环
测试判断泄漏。两板均为原相机停止后的调试状态，未取得相机/编码/推理并行峰值验收，
不把这组结果当作所有命令的内存上限。

部署和准确启动命令见 DEPLOYMENT.md。Anyka 没有 nohup，使用 trap/exec；原厂正常
退出时已确认 watchdog stopped。主机 Python 工具增加 macOS IOSSIOSPEED 支持，
以使用系统 termios 未定义常量的 460800/921600；HDTools 使用自己的串口库。

### 会话回收结果

两板均完成连续 20 次打开/关闭、21 秒无请求超时（配置为 20 秒）。
回收后 PTY 数量为 0，服务 FD 均回到 11；未残留本次 Shell/vi/top。
RV1106 20 次循环后服务 RSS 仍为 956 KiB，Anyka 为 980 KiB，未观察到持续增长。
最终样本在 build/pty-{rv1106,anyka}-audit-final-release.json；旧版基线另存
对应 baseline JSON。先前一轮 audit-complete.json 保留作为历史测量，不与最终值混用。RV1106 第一轮仪器脚本仅采到了服务本身，随后改用
/proc/*/stat 枚举子进程并重测，表格采用完整进程树的最终数据。

### 真实交互与互斥

使用 HDTools 自身的 SerialIO、Session、PtyConnection 和 JediTerm 组件，分别连接两板，
验证 vi 编辑保存、top 全屏刷新、Ctrl+C 和退出；界面截图保存于 build/pty-live-*.png。
SDK 取得控制权后旧 PTY 失效，业务字节正确发送；释放后显式重连可继续执行命令。
两板手动取消读取后，观察 400 ms 内发送计数不再增加；等待 20 秒租约到期后由板端回收。
完整测试范围、证据与产物校验见 [VALIDATION.md](VALIDATION.md)。
