# RV1106 业务接入 hd485d 0.4.0

日期：2026-11-20。APP 0.4.52.d / UART 1.0.26，HDTools 0.2.0 协议兼容。

## 行为

- ttyS3 / 230400 8N1 / GPIO35 由 hd485d 独占，IPC 为 /run/hd485.sock；方式一、rx-profile=qjy。
- 温控器业务协议和处理器保持原有语义，ttyS4 静态相机链路不改。
- 业务帧校验后申请，实际应答发送完再释放；广播、异步探测、转发、OTA 的生命周期分别覆盖。
- 只过滤 H485 维护协议，不删除业务帧或 OTA 裸流内部的同样字节；详见 PROTOCOL.md。
- 心跳也会抢占终端。主机需要安排维护窗口；单条半双工总线不能让两个独立主机同时发送。
- 断链/溢出使当前事务失败，后台重连；旧回调不向新连接发送，未知结果不重放，不自动直开 UART。

## 构建

hd485d 按 tools/build-rv1106-docker.sh 在隔离快照中构建。生成的 SDK 包含头文件、
libhd485.a、libhd485_wire.a，两静态库启用 PIC。

camera_app/build_all.sh 新增必需环境变量 HD485_SDK_ROOT，指向同次 ARM SDK 安装目录。
继续使用原 CROSS_COMPILE、SSL=2、OSS_DEBUG=true 构建；UART CMake 通过该路径启用
HD485_TRANSPORT_ENABLED 并链接静态 SDK。UART 独立原生测试不提供 SDK 时仍构建原后端。
AppConfig.use_hd485 默认 true，hd485_socket 默认 /run/hd485.sock；原直接串口后端保留，
但不会在运行时连接失败后自动回退。原 App 默认忽略命令行参数的行为不变。

## 部署与回退

本轮仅使用独立目录 /userdata/tmp_var/hd-rs485-drive-0.4.0；不调用系统安装器、不改开机配置。
联调包 dist/hd485-rv1106-integration-0.4.0.tar 内含：

- rv1106/：服务、SDK、示例和配置。
- camera/：接入版程序及配套库（测试交付副本执行 strip，原构建产物保留符号）。
- baseline/：原 0.4.51.d 程序及旧 UART 库，用于同条件对比。
- test-only/：自动 OTA 检查拦截库及源码，仅用于显式测试环境，不安装到系统库路径。
- start-integration.sh：先启动 hd485d，再启动 App，记录 PID 与日志。

确认旧进程正常退出、看门狗关闭、UART 无其他所有者后执行：

```sh
cd /userdata/tmp_var/hd-rs485-drive-0.4.0
sh start-integration.sh --no-auto-update-check
```

此参数仅通过 LD_PRELOAD 使自动网络 OTA 检查返回“无更新”，防止联调时自动安装改变板卡；
正式 App、OTA 库和串口升级逻辑不替换。省略参数恢复原自动检查逻辑。启动前须确认没有
未完成的系统/APP/模型升级事务，避免原恢复流程执行安装或重启。

停止时读取 camera.pid 并核验 /proc/PID/exe，正常 SIGTERM，确认退出和 watchdog disabled；
再核验并停止 hd485d。旧 0.3.0 服务目录与原 OEM App 保留。回退原业务时先停止共享服务，
再按原启动流程恢复直接串口版本；不能把新 App 单独覆盖到没有 hd485d 的原开机环境。

## 验证

已完成：Mac 38 项服务集成、Linux 36 项（Python 3.10 不执行两个旧 file_digest CLI 用例，
Mac 已覆盖）、HDTools 13 项 JVM；真实 UART 库通过 6 项新后端模拟集成，包括 512 KiB 应答、
异步探测和心跳、OTA 裸流 MD5、PTY 抢占、断链重连不重放。

原业务回归覆盖转发生命周期、设备控制、产测、49 个 APP/模型/系统 OTA 模拟场景、上传 JSON 和发送排空、
静态抓拍事务、PWM、版本查询。所有系统 OTA/重启/产测测试均使用模拟或拦截，未对实板执行。
原始证据位于 build/integration-0.4.0-final.log、build/linux-0.4.0-final.log、
build/camera-transport-integration.log、build/camera-uart-regressions*.log、
build/hdtools-hd485-0.4.0-tests.log。

## 实机验证方法与限制

同一块 RV1106、同一 USB-RS485 链路、230400 8N1。先运行原 0.4.51.d，再运行接入版；
每轮 500 次心跳，主机为每个完整应答保留 60 ms 换向间隔，延迟从发送开始到完整应答 CRC 校验。
只读属性、状态汇总、动静态记录分页、D2 异步探测并行心跳、开关门广播均验证。
维护测试包含普通命令、PTY 被业务抢占后重新 HELLO、文件往返；旁路订阅记录必须与主机
业务 TX 的字节数和 MD5 完全一致，维护流量不计入期望业务数据。

第一次维护返回业务时，测试脚本遗漏最后一次维护应答后的换向等待，最后的 11 字节业务
请求未到达板端；补齐 60 ms 间隔后完整重跑通过。原始失败记录仍保留为
build/rv1106-integrated-no-turnaround.*，不计为通过。

0xCD 广播已触发 App 的 OPEN/CLOSE 与 Session 收尾。本轮未实际移动相机／商品，Session
frames/detects/statics 为 0；不能据此声称已验收识别、角度触发抓拍、真实图片上传或整机 OTA。
真实静态相机启动版本查询成功（APP 7.3.0s）；UART 传输、升级故障／系统安装仍由模拟覆盖。

### 磁盘与内存口径

交付可执行文件：hd485d 71,888 字节（70.2 KiB），App 1,130,696 字节，UART 库 445,920 字节。
App 和 UART 库为交付副本 strip 后大小；完整调试符号保留在隔离构建产物。
实板首次业务运行采样：原 App RSS 9,164 KiB / PSS 8,803 KiB；接入 App RSS 9,392 KiB /
PSS 9,014 KiB；hd485d RSS 892 KiB / PSS 567 KiB。仅是该时点进程页统计，不含所有 DMA／内核
分配，不能当作整个摄像头内存预算或峰值。最终测试结束读数另列。

证据：build/rv1106-baseline-memory.log、build/rv1106-integrated-memory.log；原型与新服务交接
全程 boot_id 不变；原 App 正常 SIGTERM 退出且日志确认 watchdog disabled。


## 最终实机结果与交接

2026-11-20 11:58（UTC+8），最终库已部署并完成重跑：

| 500 次心跳 | 原 0.4.51.d | 接入 0.4.52.d |
|---|---:|---:|
| 成功 | 500 / 500 | 500 / 500 |
| P50 | 10.87 ms | 42.05 ms |
| P95 | 17.12 ms | 47.34 ms |
| P99 | 22.06 ms | 49.50 ms |
| 最大 | 30.75 ms | 50.73 ms |

接入增加 IPC/事务仲裁与调度延迟，不能表述为零开销；以上测试没有超时，主机测试超时为 5 秒。
地址、SDK、波特率、系统版本和静态记录分页应答与基线逐字节一致；App 版本按预期更新，
状态汇总及动态记录会随版本、测试生成的 Session 变化。新增 ttyS4 0xAA 转发查询内外 CRC
均通过，转发后继续正常业务。普通命令、PTY 抢占／重连、文件目录浏览、1 KiB 文件上传下载通过。

最终订阅者接收 **5,709 字节**，MD5 **5928ff9eeb74f4664fc758c131095390**，与测试主机所有业务
请求完全一致；命令、PTY、目录及文件维护帧无额外字节混入。结束后 owner=0、tx=0、error=0，
PTY 数量 0，测试监听进程已退出。最终进程采样：hd485d RSS **904 KiB** / PSS **585 KiB**；
App RSS **9,380 KiB** / PSS **9,010 KiB**。最终证据位于 build/rv1106-integrated-final-business.*、
build/rv1106-filter-final-proof.log、build/rv1106-integrated-final-memory.log、build/rv1106-final-handoff.log（停止测试监听后的最终读数）。

保持运行：hd485d PID **1378**，App PID **1851**，目录 **/userdata/tmp_var/hd-rs485-drive-0.4.0**。
ttyS3 唯一持有者为 hd485d；App 持有 ttyS4 和 watchdog。PID 仅为交接快照，停止前仍须核验 exe。
本次未重启、未刷写系统、未修改开机脚本，Anyka 未更新。测试 App 保留 no_auto_update.so 环境；
重启板卡后不保证进入本联调版本，仍按原开机配置运行。

| 板端最终文件 | MD5 |
|---|---|
| rv1106/bin/hd485d | af38be49691ba3a2c1e9f9d2e1d1a00f |
| camera/hd_camera_app2 | d1250beee4ab7d120ffed9d233b8d1ac |
| camera/lib/libhd_uart_qjy_rv1106.so | f7f82e20bb3f8ea630969726b67eda24 |

板端先传完整目录，再以 staging 原始库更新最终 UART；板上旧 tar 是首轮传输证据，不用于重新部署。
本地主交付包已用最终库和文档重新打包，以旁边的 .md5/.sha256 校验文件为准。

## 用户 OTA 升级后的正式程序启动（2026-11-20 13:41 UTC+8）

xpl 自行恢复 0.4.51 后，用本次 dynamic_app_0.4.52.d.tar.gz 升级并重启。
启动日志在 /run/hd485.sock 打开失败后退出，检查确认 hd485d 未运行；OEM App/UART 库 MD5
与最终交付一致。按 xpl 要求，先启动原联调目录的服务，再执行正式 App 的 init start。
已运行 /oem/usr/bin/hd_camera_app2（PID566），hd485d PID467；10次心跳及C2/D4版本查询通过。
服务 owner=0、tx=0、error=0；hd485d 独占ttyS3，App持有ttyS4和watchdog。
本次使用原正式启动脚本，没有加载测试OTA拦截库；启动／后台自动检查均返回无更新。
未修改开机脚本、未重启。此时状态取代上文11:58临时App的交接状态。

两个进程均停止时，在板端日志串口执行完整启动流程：

```sh
cd /userdata/tmp_var/hd-rs485-drive-0.4.0/rv1106
nohup sh tools/run-rv1106.sh --allow-shell --allow-pty --fs-root /   </dev/null >../hd485d.log 2>&1 &
echo $! >../hd485d.pid
sleep 2
bin/hd485-example /run/hd485.sock status && sh /etc/init.d/S22hd_camera_app2 start
```

脚本读取 configs/rv1106-ttyS3.env：ttyS3、230400 8N1、GPIO35、idle、qjy profile。
App初始化约需数秒，查看 /userdata/hd_camera_app2/logs/console.log 中的
Step6 business components ready 和 watchdog enabled，再用业务请求确认。
当前已运行，无需重复执行上述启动命令。重启整板后仍须按顺序启动服务与App。
证据：build/rv1106-ota-start-preflight.log、build/rv1106-ota-start-ready.log、
build/rv1106-ota-start-business.log、build/rv1106-ota-start-final.log。

## camera_app 开发目录隔离

按 xpl 要求，camera_app 的全部未提交源码／文档改动（10 个已修改文件、5 个新文件，含 LTE 分析文档）
已迁入独立 worktree，分支 feat/hd485-camera-app：

/Users/xiangpengle/Documents/tmp/luckfox/worktrees/hd485-camera-app/qjy-rv1106/camera_app

后续 camera_app 修改、构建及新交付均使用此目录，不修改 luckfox 主目录的 camera_app。
主目录 camera_app 已恢复至 HEAD，Git 状态干净；其他目录原有改动的状态和 diff 校验一致。
0.4.52.d 升级包及 hd485-integration-0.4.52.d 产物已复制到新 worktree 的 dist，原交付路径保留。
未创建 Git 提交。迁移前备份：/Users/xiangpengle/Documents/tmp/hd-rs485-drive/camera-worktree-move-ltif6mwx。
