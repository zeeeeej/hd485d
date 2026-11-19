# RV1106 / Anyka 板卡与 PTY 记录

检查日期：2026-11-19。当前 hd485d 0.2.1 / HDTools 0.1.3 尚无交互 PTY。
下列实测、历史参考和未来要求分开记录，不以编译成功代替实机验收。

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
| PTY | /dev/ptmx、devpts、内核计数接口存在 | 实际 /dev/pts/0、stdin/stdout TTY、24×80 窗口设置、top -b -n 1 及释放通过 |
| 命令 | BusyBox sh / vi / top | BusyBox 1.30.1，sh / vi / top / script |
| 现状 | hd485d 0.2.1 已部署，原相机停止 | 原厂 fridge_ipc 在运行，尚未部署 hd485d |

Anyka 当前原厂进程持有 ttySAK2、GPIO5 和 /dev/watchdog。接管前必须核验正常停止及
看门狗关闭，不能强杀后直接接管。root 日志口登录已验证，本文不保存登录凭据。
485 USB 节点已枚举且当次无占用；本轮未发送业务帧。

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
- 历史长包存在局部丢失/无响应调查记录，不将其描述为已解决。HD485 Anyka 链路待验。

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

## 无 PTY 基线与测量要求

| 指标 | RV1106 0.2.1 | Anyka |
|---|---|---|
| hd485d 文件 | 66,624 字节（当前产物） | 待交叉构建 |
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
业务停止的验收与实际业务负载峰值验证分开。以上新增指标均为 **待实现后实测**。

## 部署与启动

当前 RV1106 0.2.1 的准确目录、参数、校验与回退见 [DEPLOYMENT.md](DEPLOYMENT.md)。
Anyka 根和 /usr 为只读 squashfs；/userdata 为可写 jffs2，本次剩余 7,716 KiB。
Anyka 尚无 hd485d 可执行部署命令，不能照搬 RV1106 的 ttyS3/GPIO35/运行目录。

后续部署顺序：交叉构建并校验 ABI → 独立版本目录传输及校验 → 确认 UART 占用和业务
正常退出/看门狗关闭 → 启动 hd485d → 核对 Socket/日志/能力 → HDTools 验证 → 记录回退。
板端实际写入目标和业务影响在部署前说明并按用户规则确认。不刷系统，不改开机配置。

未来 PTY 启动顺序：hd485d 初始化串口，进入方式一 idle → 用户打开交互终端时才创建
PTY/Shell → 轮询输入输出 → 主动关闭、业务抢占或断线超时回收会话。
连接期间可保留 Shell 环境；断线后不恢复旧会话。具体启动参数及实测在实现后补齐。
