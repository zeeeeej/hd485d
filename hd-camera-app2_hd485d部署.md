# hd_camera_app2 + hd485d 部署（RV1106）

> **新增 App 双向升级（0.4.54.d / 0.5.54.d）**：未安装 hd485d 的原始 0.4.51.d，先通过原 App 升级功能安装 0.4.54.d 过渡版，再安装包含 hd485d 0.4.3 的 0.5.54.d 配套包。新版可退回原始 0.4.51.d 包，再次升级仍须经过过渡版。0.1～0.4 前缀按直连版本识别，但拒绝历史 0.4.52/0.4.53 联调包；更早固件不因前缀匹配就视为已通过升级验收。
>
> App 源码及交付物在独立 worktree：`/Users/xiangpengle/Documents/tmp/luckfox/worktrees/hd485-camera-app/qjy-rv1106/camera_app/`，包位于 `dist/`，完整说明为 `deploy/HD485_APP_OTA.md`。2026-11-23 已完成原始旧包、过渡版、新版的完整实板往返，当前保留 App 0.5.54.d / UART 1.0.28 / hd485d 0.4.3；验证记录为该 worktree 下 `docs/records/HD485_STACK_OTA_20261123.md`。以下 0.5.53.d 内容保留为历史手动部署说明，不能把其普通 tar 当作新 App OTA 包。

> 2026-11-23 前一轮记录：RV1106 单独升级 hd485d **0.4.3** 时，App 为 0.5.53.d、UART 为 1.0.27；当前状态以上述双向升级记录为准。发送前100us等待与Anyka统一，发送后5ms不变。[实测、摘要与回退](RV1106_发送等待实测.md)。下文原联合包仍包含0.4.1，历史包未覆盖；新的daemon普通tar为dist/hd485-rv1106-0.4.3.tar。

## 2026-11-24 新设备 CamDock 验收

xpl 在此前未安装过 hd485d 的另一台 RV1106 上，通过 CamDock 依次升级 `dynamic_app_0.4.54.d.tar.gz`、`dynamic_app_0.5.54.d.tar.gz`，并确认“验证完毕”。该路径不要求预先手工安装 daemon；过渡版安装兼容管理器，配套版安装 hd485d 0.4.3 和启动配置。

本次设备 RS485 为 `/dev/cu.wchusbserial144420`，日志口为 `/dev/cu.wchusbserial1420`。这是 xpl 的实际操作与验收确认，Agent 本轮没有重新连接设备或重复刷机；升级前具体 App 版本及逐项测试输出未另行提供。原始 0.4.51.d 往返实测、其他历史版本的验证边界维持原记录。

配套实现已归档在 camera_app 的 `feat/hd485-camera-app` 分支，提交 `b2bfaa4`。

详细用户验收记录位于上述 camera_app worktree 的 `docs/records/HD485_CAMDOCK_FIRST_INSTALL_20261124.md`。后续设备仍按“过渡包 → 等待重启并确认业务恢复 → 配套包 → 检查版本、daemon 和业务”执行。

## 1. 版本和适用范围

本次配套版本：**App 0.5.53.d / UART 库 1.0.27 / hd485d 0.4.1**。

- 从本次起，`0.5.x` 表示依赖 hd485d 的 App 分支，用于区别原来直接操作串口的实现。源码基础版本为 `0.5.53`，`OSS_DEBUG=true` 构建后为 `0.5.53.d`。
- `0.4.51` 是此前直接串口版本；历史 `0.4.52.d/0.4.53.d` 是已接入 hd485d 的联调版本。不要只根据历史版本的 `0.4` 判断是否依赖 daemon。
- 此次从 `0.4.53.d` 改为 `0.5.53.d` 只改 App 版本标识，业务与串口优化逻辑相同；不改 UART 和 daemon 的版本。
- 本文针对与已验证板子相同的 **QJY RV1106 V1、ARM/uClibc** 环境。普通 Luckfox 板或其他 RV1106 底板须先核对硬件映射、驱动和相机 SDK，不能仅因芯片相同就直接使用。

**推荐正式安装到 OEM 路径，开机先启动 hd485d，再启动 App。** 这样保留原有模型初始化、日志、看门狗和 App OTA 安装路径。隔离调试方式见第 9 节。

## 2. 准备交付物及基础环境

集成包位于 camera_app 工作副本：

```text
dist/hd485-optimized-0.5.53.d.tar
dist/hd485-optimized-0.5.53.d.tar.md5
dist/hd485-optimized-0.5.53.d.tar.sha256
```

包内主要文件：

```text
hd_camera_app2                          App 0.5.53.d
lib/libhd_uart_qjy_rv1106.so             UART 1.0.27
rv1106/bin/hd485d                       daemon 0.4.1
rv1106/bin/hd485-example                状态/订阅工具
rv1106/configs/rv1106-ttyS3.env          串口配置
rv1106/tools/run-rv1106.sh              daemon 启动入口
rv1106/include/、rv1106/lib/             供其他 App 接入的 C SDK
tools/S21hd485d                         daemon 服务脚本
tools/S22hd_camera_app2_hd485d           App 开机依赖包装脚本
tools/stop-rv1106-stack.sh               正常停止 App → daemon
tools/start-rv1106-optimized.sh         隔离调试启动脚本
MD5SUMS                                包内文件校验清单
hd-camera-app2_hd485d部署.md             本文
```

**这是 App＋UART＋daemon 集成包，不是完整固件、基础 dist 包或 OTA 包。** 不能把它直接交给 `install_dist.sh` 或串口升级命令。

### A. 新板已经能正常运行原相机 App

例如同型号、同固件环境已运行 0.4.51：保留现有其他相机库、模型、设备配置，按第 3～8 节部署。

### B. 新板只有 Linux，还没有完整相机应用环境

先部署该板适用的 QJY 固件、相机基础环境和经过确认的完整 `dist_<基础版本>.tar`。使用 camera_app 的 `deploy/install_dist.sh` 时先正常停止已有 App，选择 `--no-start`，随后再安装本集成包：

```sh
# 在基础完整包、它的 .md5 和 install_dist.sh 所在目录执行。
# BASE_VERSION 必须改为实际交付的基础包版本；该基础包不是本文集成包。
BASE_VERSION='实际基础包版本'
sh install_dist.sh "./dist_${BASE_VERSION}.tar" --no-start
```

基础安装详见 camera_app 的 `deploy/FIRST_INSTALL.md`、`deploy/INSTALL_DIST.md`。必须具备：

- `/oem/usr/lib` 中的 Detect、OSS、OTA、Record、fridge SDK、zlib，以及匹配固件的 RKNN、媒体库、mbedtls 3.6.7 和 C/C++ 运行库。
- `/oem/usr/bin/S22hd_camera_app2`、`/etc/profile.d/start.sh` 等原有 OEM 启动基础。本文只替换 `/etc/init.d/S22hd_camera_app2` 的依赖入口，保留 OEM 启动器。
- 适用的正式 RKNN、标签和检测配置，或 `/oem/usr/share/default_model/` 下可供原启动器首次补齐的配套默认模型。模型版本不能从 App 版本推断。
- 正确的 IQ 文件、摄像头/NPU 驱动、静态相机链路及看门狗设备。
- `rkipc` 已停止且不会和 App 同时占用相机。保留原厂 S21appinit/RkEnv，只按基础安装指南处理 rkipc 启动行。

不要把旧板的设备身份缓存、凭据缓存或未完成 OTA 事务当作基础环境复制到新板。

## 3. 确认新板和串口

所有 Shell 安装命令通过**新板日志串口，115200 8N1**执行。485 协议口不是原始 Shell 口。主机设备名按新板实际枚举确认，不直接套用之前板子的 USB 设备名；退出占用相同串口的其他工具。

在板上检查：

```sh
cat /proc/version
cat /proc/device-tree/model
ls /dev/ttyS3 /dev/ttyS4 /dev/rknpu /dev/watchdog /dev/ptmx
ls /sys/class/gpio
ls /oem/usr/share/iqfiles/gc2053_CMK-OT2274-V10_28IRC-F20.bin
ls /oem/usr/bin/S22hd_camera_app2 /etc/profile.d/start.sh
ls /oem/usr/lib/libfridge_ipc.so /oem/usr/lib/libhd_detect.so
mount
df -k /oem /userdata
```

确认 `/oem` 可写，空间足够存放集成包、解包和旧文件备份；基础库和模型的空间另外计算。PTY 需要 `/dev/ptmx` 和已挂载的 devpts。检查并处理未完成升级；不通过删除 OTA 目录跳过事务：

```sh
for txn in dynamic_sys_transaction dynamic_app_transaction model_transaction; do
    if [ -e "/userdata/ota/$txn" ]; then
        echo "请先处理未完成事务：$txn"
    fi
done
```

当前配置文件 `rv1106/configs/rv1106-ttyS3.env`：

| 项目 | 值 |
|---|---|
| 对外温控器 485 | `/dev/ttyS3`，由 hd485d 独占 |
| 静态相机串口 | `/dev/ttyS4`，由 App 使用 |
| 参数 | `230400 8N1` |
| RS485 DE | GPIO35，高发送、低接收 |
| 模式 | `idle`（方式一） |
| 接收过滤 | `qjy` |
| 本地 IPC | `/run/hd485.sock` |

不同底板必须先确认接线和 GPIO。App 默认忽略命令行参数；不能靠给 App 临时加 `--baud`/`--ttyS3` 来改变当前接入配置。daemon 串口参数须与 App 构建配置一致。

## 4. 传包和校验

宿主机先用配套 SHA256 校验包（在包所在目录）：

```sh
shasum -a 256 -c hd485-optimized-0.5.53.d.tar.sha256
```

可使用项目已有 OSS＋`http_get` 下载流程，或在确认日志口空闲后用项目 ZMODEM 工具。ZMODEM 示例中的两个路径和串口名需要填写：

```sh
# 宿主机执行，Python 3.11+；先确认 lsof 没有其他占用。
CONSOLE='/dev/cu.新板日志串口'
BUNDLE='/实际camera_app工作副本/dist/hd485-optimized-0.5.53.d.tar'
LUCKFOX_TOOLS='/实际luckfox仓库/tools'
lsof "$CONSOLE"
python3 "$LUCKFOX_TOOLS/qjy_zmodem_send.py" "$BUNDLE" \
  --device "$CONSOLE" \
  --remote-dir /userdata/tmp_var/hd485-install-0.5.53.d --timeout 360
```

传输工具会检查整包 MD5。也可在板上运行 `md5sum 包名` 与宿主 `.md5` 比较。目标 BusyBox 不支持 `md5sum -c` 和 `tar -z`，因此使用普通 tar：

```sh
cd /userdata/tmp_var/hd485-install-0.5.53.d
mkdir unpack
cd unpack
tar -xf ../hd485-optimized-0.5.53.d.tar
# 下列命令在校验失败时返回非零；失败就停止部署。
sh -eu -c '
while read expected file; do
    actual=$(md5sum "$file")
    test "${actual%% *}" = "$expected"
    echo "$file OK"
done <MD5SUMS
'
```

不要重复解压覆盖正在运行的程序。以下步骤均从这次新建的 `unpack` 目录继续。

## 5. 正常停止、备份和安装

先等待当前业务事务结束，然后使用配套脚本正常停止。它扫描实际进程路径，不依赖 pidof，最多等待每个进程 30 秒；App 不退出时会返回失败并保留 daemon，不强杀看门狗进程：

```sh
sh tools/stop-rv1106-stack.sh
```

只有脚本成功、App 已退出且串口无其他使用者后才继续。可以检查打开的关键设备：

```sh
for f in /proc/[0-9]*/fd/*; do
    target=$(readlink "$f" 2>/dev/null || true)
    case "$target" in
        /dev/ttyS3|/dev/ttyS4|/dev/watchdog*) echo "$f -> $target";;
    esac
done
```

下面是一次安装命令。在同一个日志终端中执行，并记下输出的备份目录。使用 `sh -eu`，任何失败都会中止后续安装：

```sh
sh -eu <<'INSTALL'
STAGE=/userdata/tmp_var/hd485-install-0.5.53.d/unpack
BACKUP=/userdata/backup/hd485-before-0.5.53.d-$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$BACKUP" /oem/hd485/rv1106 /oem/hd485/tools
cp -p /oem/usr/bin/hd_camera_app2 "$BACKUP/hd_camera_app2"
cp -p /oem/usr/lib/libhd_uart_qjy_rv1106.so "$BACKUP/libhd_uart_qjy_rv1106.so"
cp -p /etc/init.d/S22hd_camera_app2 "$BACKUP/S22hd_camera_app2"
if [ -f /etc/init.d/S21hd485d ]; then
    cp -p /etc/init.d/S21hd485d "$BACKUP/S21hd485d"
fi
# 备份现有 daemon/配置/辅助脚本；空目录也可恢复。
cp -a /oem/hd485 "$BACKUP/hd485"
echo "BACKUP=$BACKUP"

cp -a "$STAGE/rv1106/." /oem/hd485/rv1106/
cp "$STAGE/tools/stop-rv1106-stack.sh" /oem/hd485/tools/
chmod 755 /oem/hd485/tools/stop-rv1106-stack.sh

cp "$STAGE/lib/libhd_uart_qjy_rv1106.so" /oem/usr/lib/.libhd_uart_qjy_rv1106.so.new
chmod 644 /oem/usr/lib/.libhd_uart_qjy_rv1106.so.new
mv /oem/usr/lib/.libhd_uart_qjy_rv1106.so.new /oem/usr/lib/libhd_uart_qjy_rv1106.so
cp "$STAGE/hd_camera_app2" /oem/usr/bin/.hd_camera_app2.new
chmod 755 /oem/usr/bin/.hd_camera_app2.new
mv /oem/usr/bin/.hd_camera_app2.new /oem/usr/bin/hd_camera_app2

cp "$STAGE/tools/S21hd485d" /etc/init.d/.S21hd485d.new
chmod 755 /etc/init.d/.S21hd485d.new
mv /etc/init.d/.S21hd485d.new /etc/init.d/S21hd485d
cp "$STAGE/tools/S22hd_camera_app2_hd485d" /etc/init.d/.S22hd_camera_app2.new
chmod 755 /etc/init.d/.S22hd_camera_app2.new
mv /etc/init.d/.S22hd_camera_app2.new /etc/init.d/S22hd_camera_app2
sync
md5sum /oem/usr/bin/hd_camera_app2 /oem/usr/lib/libhd_uart_qjy_rv1106.so /oem/hd485/rv1106/bin/hd485d
INSTALL
```

本次三个程序文件应为：

| 文件 | MD5 |
|---|---|
| App 0.5.53.d | `ee98591eb344af47509e23e5bb230500` |
| UART 1.0.27 | `aec6444ce1dd9529a8b50171dfbc0ad4` |
| hd485d 0.4.1 | `507fd6a954d4953878a059eebdeb841c` |

如果有不同硬件配置，安装后启动前修改 `/oem/hd485/rv1106/configs/rv1106-ttyS3.env`；不要覆盖正式模型或其他系统库。

## 6. 启动和开机顺序

首次手动启动：

```sh
sh /etc/init.d/S21hd485d start
sh /etc/init.d/S21hd485d status
sh /etc/init.d/S22hd_camera_app2 start
```

也可以直接执行最后一条，新的 S22 入口会先确保 daemon 已就绪，再调用原来的 `/oem/usr/bin/S22hd_camera_app2 start`。原 OEM `start.sh` 继续负责首次模型准备、日志及原有 App OTA 回滚路径。

开机顺序：

```text
原厂 S21appinit（保留）
  → S21hd485d：独占 ttyS3，提供 /run/hd485.sock
  → S22hd_camera_app2：确认 daemon 状态，再调用原 OEM 启动器
  → hd_camera_app2：连接 socket；独立使用 ttyS4 和 watchdog
```

脚本没有依赖 systemd。以上自启动要求目标固件确实按原 QJY 流程执行 `/etc/init.d/S* start`。daemon 服务脚本不是自动拉起监控器；故障时查明原因，再按下面顺序重启整套服务。

日常停止/重启：

```sh
# 停 App，正常关闭 watchdog，再停 daemon。
sh /etc/init.d/S22hd_camera_app2 stop
sh /etc/init.d/S21hd485d stop

# 重新启动整套服务。
sh /etc/init.d/S22hd_camera_app2 start

# 只重启 App，daemon 保持工作。
sh /etc/init.d/S22hd_camera_app2 restart
```

daemon 服务脚本在 App 仍运行时拒绝 stop/restart，避免误断业务依赖；没有 daemon 的情况下也要求先正常停 App，再启动整套服务。

## 7. 新板验收

1. `S21hd485d status` 成功，输出 `mode=0`、`error=0`。App 按事务申请控制权，业务空闲时 `owner=0` 是正常状态。
2. App 日志出现 `Step6 business components ready` / `entering main loop`，485 查询 App 版本应为 **0.5.53.d**；PID 存在不代表全部业务初始化完成。
3. 查看实际运行程序与库：

```sh
for f in /proc/[0-9]*/exe; do
    target=$(readlink "$f" 2>/dev/null || true)
    case "$target" in
        */hd_camera_app2|*/hd485d) echo "$f -> $target";;
    esac
done
# App 应来自 /oem/usr/bin；daemon 应来自 /oem/hd485/rv1106/bin。
# 用实际 App PID 检查库来源：
APP_PID='实际App进程号'
awk '/libhd_uart_qjy_rv1106.so/ {print $NF}' "/proc/$APP_PID/maps"
```

4. 业务心跳、静态相机查询、开关门及检测正常；ttyS3 只能由 daemon 打开，ttyS4/watchdog 由 App 使用。
5. 主机给出维护窗口，用 HDTools 验证命令、文件上传下载和 PTY；业务到来时应抢占维护。持续心跳会反复抢占终端，维护主机需要安排发送时机。
6. 在新板允许的维护时段重启一次，重新验证 daemon→App 的开机顺序和业务。本轮没有替新板执行重启或验收，这一步必须在实际新板完成。

常用状态和日志：

```sh
/oem/hd485/rv1106/bin/hd485-example /run/hd485.sock status
tail -n 40 /userdata/hd-rs485/hd485d.log
# 下列文件可能包含业务配置，不要整份外传。
tail -n 40 /userdata/hd_camera_app2/logs/console.log
```

## 8. 常见问题和回退

- **App 无法启动/连接 socket**：先确认 S21 服务成功、socket 为 `/run/hd485.sock`，不要只替换 App 而漏掉 daemon 或新 UART 库。
- **终端正常但业务不通**：核对 230400/8N1、GPIO35、线路和 qjy 配置，确认库来自 `/oem/usr/lib`，不是残留的隔离 `LD_LIBRARY_PATH`。
- **资源/模型缺失**：返回第 2 节补齐基础环境，不通过禁用业务初始化检查硬启动。
- **socket/锁文件已存在**：先查记录 PID、`/proc/PID/exe`、UART FD 和实际运行服务。只有证明确为崩溃遗留且无活服务/串口所有者，才清理相应 socket 和锁文件；脚本不自动删除它们。
- **停止超时**：保留现场，检查业务是否仍在发大包、App 是否已关闭 watchdog；不要直接 kill -9 后继续覆盖文件。

回退到部署前状态时，使用第 5 节记录的那个备份目录：

```sh
# 先正常停止完整服务；失败则不要继续。
sh /oem/hd485/tools/stop-rv1106-stack.sh

sh -eu <<'ROLLBACK'
BACKUP='/userdata/backup/填写本次实际备份目录'
test -f "$BACKUP/hd_camera_app2"
test -f "$BACKUP/libhd_uart_qjy_rv1106.so"
test -f "$BACKUP/S22hd_camera_app2"
cp -p "$BACKUP/hd_camera_app2" /oem/usr/bin/hd_camera_app2
cp -p "$BACKUP/libhd_uart_qjy_rv1106.so" /oem/usr/lib/libhd_uart_qjy_rv1106.so
cp -p "$BACKUP/S22hd_camera_app2" /etc/init.d/S22hd_camera_app2
if [ -f "$BACKUP/S21hd485d" ]; then
    cp -p "$BACKUP/S21hd485d" /etc/init.d/S21hd485d
else
    rm -f /etc/init.d/S21hd485d
fi
# 保留当前文件供排查，再恢复原有 daemon 文件/配置。
mv /oem/hd485 "/oem/hd485-disabled-$$"
cp -a "$BACKUP/hd485" /oem/hd485
sync
ROLLBACK
```

然后按**回退目标的原启动方式**启动：目标为 0.4.51 直连版时必须保持 daemon 停止，再执行原 S22；若目标本身依赖 hd485d，必须先恢复并启动它配套的 daemon。不要只换回 App、留下不同 UART 库。

后续 App OTA 应投放同一 0.5.x 依赖分支的匹配包，并确保 daemon 已预装；现有 App OTA 不会自动安装 daemon 或修改 S21/S22。跨回直连分支应按上述完整回退流程处理。

## 9. 可选：只做隔离调试

已具备可用相机环境、正式模型和数据目录，且暂时不改 OEM/自启动时，可以把整个集成包解压到 `/userdata/hd485-debug/0.5.53.d/`，正常停旧服务后执行：

```sh
sh /userdata/hd485-debug/0.5.53.d/tools/start-rv1106-optimized.sh \
   /userdata/hd485-debug/0.5.53.d
```

其工作目录为 `/userdata/hd_camera_app2`，库优先从隔离包 `lib/` 加载，日志在隔离包的 `app.log` / `daemon.log`。它不提供首次模型初始化，也不接入开机自启动。停止用同包 `tools/stop-rv1106-stack.sh`。

隔离运行路径与原 App OTA 的 OEM 安装路径不同，不能把这当作正式 OTA 部署。需要长期运行/正常升级时，采用第 5～6 节的 OEM 部署方式。

## 10. 本次验证边界

0.5.53.d 已按既有 ARM/uClibc、SSL=2、OSS_DEBUG=true 环境重新编译，核对了二进制内的实际版本标记和配套包哈希。UART、daemon 二进制与此前优化实测版本一致。

此前 0.4.53.d 的业务、内存和吞吐记录保留原名，不把它们改写成一次新的 0.5.53.d 实板测量。新增服务脚本有 Shell 检查及 Linux PTY/模拟进程检查；Docker Rosetta 环境的 `/proc/PID/exe` 使用测试适配来模拟真实 Linux 可执行路径，适配不进入交付包。新板 OEM 安装、开机及重启验收仍按第 7 节执行。本轮没有更新当前调试板或修改其自启动配置。

## 11. 当前板部署记录（2026-11-20）

第 10 节记录打包时的验证边界。随后已按 xpl 指令完成当前 QJY RV1106 V1 的 OEM 安装，并通过一次实际重启验证自启动。App 0.5.53.d / UART 1.0.27 / hd485d 0.4.1 已保留运行。备份、实际路径和逐项验证见[本次部署记录](RV1106_DEPLOY_0.5.53.d.md)；本次未重新打包交付 tar。

2026-11-20，xpl 确认“验证完毕”，本次 **App 0.5.53.d / UART 1.0.27 / hd485d 0.4.1** 部署用户验收完成，新版保留供调试。 自动验证与用户确认的边界见上述部署记录；当前板无需继续等待 Agent 重复检查串口。
