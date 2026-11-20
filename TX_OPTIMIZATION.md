# RV1106 TX 优化：0.4.1 / UART 1.0.27 / App 0.4.53.d

本次保持 230400、8N1、GPIO35、ttyS3 和方式一（idle）不变。camera_app 仅在独立 worktree 修改，业务处理、检测、文件格式和升级回调保持原逻辑。

## 实现

1. 可选分段计时。`HD485_TX_TRACE=1` 记录板端单调时钟微秒、ticket、阶段与长度，不记录载荷。分别测 IPC 到达、提交串口、写入、排空、方向切回、TX_DONE，以及 App 接收完成事件。正常运行和性能对比关闭此开关。
2. 事件唤醒。App 的任务队列通过通知管道唤醒 socket 等待；串口线程通过通知管道通知 daemon“有空间/发送完成”。条件变量等待生产者，不使用忙循环。daemon 的维护定时 tick 保留，TX 完成不再依赖它。
3. 整帧流水发送。单块仍 ≤8192 字节；大帧用 BEGIN 声明完整长度，DATA 顺序追加。32 KiB 环形缓冲满时暂缓 DATA 应答，串口边发、App 边补下一块。整帧最后才 drain、guard、切回接收并产生一次 TX_DONE。小帧保持 WRITE；旧 daemon 无新能力位时自动回退旧接口。

32 KiB 是环形缓冲容量，不是新增进程内存总量：另有 8 KiB worker 工作块和最多一个 8 KiB 等待信用的 IPC 块。所有容量固定，不按文件长度分配整帧副本。BEGIN 总长度上限 16 MiB；camera 的业务限制保持原值。

缺数据 2 秒、主动取消或 owner 断线会结束未完成帧，清除待发数据并切回接收；下一帧可以重新申请。Linux 超时条件变量使用 CLOCK_MONOTONIC，避免授时改变等待期限。真实 UART 错误仍进入故障状态。DATA 应答只代表入队，不能把它当成发完；失败半帧不自动重放。完整语义见 [PROTOCOL.md](PROTOCOL.md)。

## 验证方式与范围

- 本机 ASan/UBSan：既有协议、文件、Shell、PTY、订阅、抢占和错误路径；新增整帧边界、512 KiB 背压、越界、权限、取消、缺数据超时、断线及后续旧 WRITE 恢复。
- Linux 使用实际 camera UART 库：大应答、异步命令、广播、模拟裸流、PTY 抢占及重连不重放；新旧服务/客户端两向兼容。
- 实板大包采用内存测试 peer，E1 校验长度和 CRC32，E2 校验全部字节；每条线上应答另验业务 CRC16。没有发送实际固件升级命令。
- 完整 App 比较先预热 20 秒，空闲采样 60 秒，500 次心跳、35 次查询、3 次异步请求并发心跳和一次开关门。测试时仅用既有 shim 禁止自动网络升级；交付运行移除 shim。
- macOS 主机每次打开协议口显式设置 IOSSDATALAT=0，前 5 次查询单独保存为预热，不计入心跳统计；请求之间仍保留 60 ms 换向间隔。此前未设置 USB 延迟的历史数据仅作背景，不混入本次对比。
- 进程 PSS/RSS 包含 App+daemon，不含 UART/网络内核缓冲、DMA/CMA；CPU 为单核百分比。自然场景开关门用于回归，不构成识别准确率对比或长时间稳定性认证。



## RV1106 实测结果（2026-11-20）

最终对比：旧版 App 0.4.52.d / UART 1.0.26 / hd485d 0.4.0；新版 App 0.4.53.d / UART 1.0.27 / hd485d 0.4.1。

| 项目 | 旧版 | 新版 | 变化 |
|---|---:|---:|---:|
| 512 KiB 板端→主机，3 次中位耗时 | 29.516 s | 25.206 s | 减少 14.60% |
| 同上有效吞吐 | 17.347 KiB/s | 20.313 KiB/s | 提高 17.10% |
| 512 KiB 主机→板端，3 次中位耗时 | 22.825 s | 22.796 s | 基本不变 |
| 完整 App 心跳中位延迟，500 次 | 30.51 ms | 6.77 ms | 减少约 78% |
| 心跳 P95 | 36.32 ms | 15.37 ms | 明显下降 |
| 空闲 App+daemon PSS，平均 | 9482 KiB | 9568.5 KiB | +86.5 KiB |
| 空闲 App+daemon RSS，平均 | 10252 KiB | 10344 KiB | +92 KiB |
| 空闲 App+daemon CPU，单核 | 10.44% | 9.85% | -0.59 个百分点 |
| App/UART/daemon 活跃文件总字节 | 1,648,504 | 1,657,132 | +8,628 B（8.43 KiB） |

新版 daemon 单独空闲 PSS 平均 611.3 KiB（旧版 582 KiB），文件 76,376 字节；App 文件 1,130,696 字节不变，UART 库 450,060 字节。线程总数保持 30，通知管道增加 4 个 FD。CPU 是本轮短时观测，不能把 0.59 个百分点的差异外推为所有场景的固定节省；结果确认没有以忙循环换取速度。

GPIO、波特率和保护时间未缩短。新版大包耗时也接近前轮直连串口的历史结果 25.44 秒；历史值的 USB 设置与本轮不同，仅作背景参照。

结果核验：

- 正式对比覆盖 256、4096、8182、8183、65536、524288 字节上下行，512 KiB 各 3 次；全部 CRC 和内容正确。另保留第一版候选复测及开启计时的独立运行。
- 完整 App 各 500 次心跳、35 次查询、3 次异步并发检查及开关门通过。旧/新检测各处理 77 帧，无丢帧，平均推理 37.6/37.4 ms；自然场景未作精度或图像一致性结论。
- 实板发送中断开客户端后 owner=0、error=0、GPIO35=0，下一条业务应答正确；缺数据约 2.099 秒报告超时，释放后继续通信正常。
- daemon 正常重启后，客户端在固定等待 2 秒后的首次验证应答成功；这是恢复确认时间，不是精确的首次重连延迟。
- EXEC、PTY、文件往返及业务抢占通过；监听者仅得到该段测试中的 11 字节业务请求，MD5 `933678c46018755cdb03d3801ee37d34`，维护流量没有泄漏。
- ASan/UBSan：Mac 原有 38 项 + 新增 4 项通过；Linux 原有 36 项通过、2 项因 Python 3.10 主机 helper 跳过，新增 4 项和实际 UART 库 6 项通过。新客户端/旧服务、旧客户端/新服务各 6 项通过。

第一次交付检查脚本在下载后直接打开 PTY，因文件句柄未关闭得到预期的 EBUSY；补充 FILE_CLOSE 后验证通过，原始失败记录保留。最后一次构建补充 Linux 条件变量单调时钟，已用最终二进制重新完成大包、完整 App 和异常恢复测量。最终 daemon MD5 为 `507fd6a954d4953878a059eebdeb841c`。

## 分段计时定位

旧版单个 8 KiB 块的代表均值：IPC 到达→提交约 0.32 ms；提交→worker 开始写约 8.22 ms（包含 DE 置高和原有保护等待）；写入 syscall 阶段约 200.05 ms；tcdrain 约 219.94 ms；drain 返回→切回接收/ready 约 10.07 ms。ready→daemon 处理 TX_DONE 约 0.43 ms，ready→下一块 worker 开始约 29.93 ms。

因此不能把差异全部归到 socket，或认定每次都是 daemon 的 10 ms poll 超时。IPC 排队本身很短；tcdrain 的 219 ms 大部分还包含 UART 队列真实发送时间，也不能全部算成空等。旧版重复执行块级收尾、保护等待和下一块任务往返，使开销累积。

同一 524298 字节业务帧，旧版拆成 65 次物理发送任务，新版是 1 次。新版连续写入约 24980 ms，末尾 drain 约 219 ms、切回/ready 约 9.74 ms、ready→TX_DONE 约 0.19 ms。吞吐提升来自减少重复收尾并重叠“补数据”和“串口发送”，没有取消物理排空确认。

计时来自软件阶段时间戳，未使用逻辑分析仪；DE sysfs 状态用于异常恢复检查，不能由主机 USB read 间隔推断逐位线缆空闲时间。上述吞吐为业务帧测试；HDTools 维护文件协议仍是 240 字节块的请求应答方式。

## 部署与回退

隔离包结构为 `hd_camera_app2`、`lib/libhd_uart_qjy_rv1106.so`、`rv1106/`、`tools/start-rv1106-optimized.sh`，不覆盖 OEM 文件，也不改 `/etc/init.d`。

先正常停止当前 App，等待退出并确认 watchdog 关闭，再停止 daemon。不要 SIGKILL App。解包到独立目录后执行：

```sh
sh tools/start-rv1106-optimized.sh /userdata/tmp_var/hd485-opt-0.4.1
```

启动顺序是 daemon 独占 ttyS3 → 核对 IPC 状态 → App 通过 `/run/hd485.sock` 接入；App 保持自己的 ttyS4、watchdog。脚本设置隔离库路径，显式移除 `LD_PRELOAD` 和计时环境变量。查看 `app.log`、`daemon.log` 排查启动，日志可能含业务配置，不要整份外传。

回退时仍先正常停止隔离 App，再停止其 daemon，随后启动原 0.4.0 daemon 和 OEM `S22hd_camera_app2 start`。OEM 0.4.52.d 及原 UART 库保持原样。隔离包不是开机自启动升级；重启后按原 init 配置运行。

## 复现入口

- 本机：`HD485_BUILD=<native-build> python3 tests/integration.py`、`tests/stream_tx.py`。文件传输主机工具需要 Python ≥3.11；Python 3.10 的 Linux 环境跳过两项主机文件 helper 用例，Mac Python 3.12 完整执行。
- camera 联调：`HD485_BUILD=... HD485_CAMERA_PEER=... python3 tests/camera_transport.py`。
- 实板：`tools/rv1106_tx_optimization.py`，参数明确指定 console、485 口和证据输出目录。此工具会正常启停测试进程，结束后保留优化版 App+daemon；执行前检查两个串口无其他读写者。
- 交叉编译沿用 camera_app 的 ARM/uClibc 工具链；新 SDK 为 UART 构建的 `HD485_SDK_ROOT`。构建在独立 Docker 源码快照中进行，没有修改另一份挂载源码。

## 交付确认与证据

最终独立启动脚本已实板执行，App 版本查询为 0.4.53.d，随后 100 次心跳通过。daemon 实际占用 ttyS3，App 占用 ttyS4/watchdog，并加载隔离目录的 UART 库；没有 LD_PRELOAD、计时环境变量或残留测试进程。OEM App/UART 的 MD5 均保持原值，整个测试 boot_id 未变化。

交付物：HD485 仓库 `dist/hd485-rv1106-0.4.1.tar`（daemon/SDK）；camera worktree `dist/hd485-optimized-0.4.53.d.tar`（隔离 App+UART+daemon+启动脚本），均附 MD5/SHA256。原始证据在 camera worktree 的 `dist/hd485-tx-opt-0.4.1/`，摘要及 trace 在 `docs/records/hd485-tx-opt-20261120/`。使用 `tools/summarize_tx_optimization.py <initial-dir> <final-dir> <summary.json>` 可重算对比。

本次未在 Anyka 实板验证，不把 RV1106 的性能数值或 GPIO 时序结论套用到 Anyka。未修改主 camera_app 目录，未提交 Git。

后续版本命名：同一优化实现的 App 已改为 0.5.53.d，用 0.5.x 区分 hd485d 依赖分支。本文的 0.4.53.d 测量保留历史版本；新板部署见 [hd-camera-app2_hd485d部署.md](hd-camera-app2_hd485d部署.md)。
