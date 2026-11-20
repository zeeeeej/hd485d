# HD485 协议 v1

所有多字节整数为 Little-Endian。维护协议走原始串口；本地 IPC 使用相同帧结构但不同 magic。接收按长度解析，不依赖一次 read 返回完整帧。

## 通用帧头

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 4 | 串口 `H485`；本地 IPC `I485` |
| 4 | 1 | 版本，固定 1 |
| 5 | 1 | 操作码；应答为请求操作码 OR `0x80` |
| 6 | 2 | 串口设备地址，默认 1；IPC 固定 0 |
| 8 | 4 | 串口会话号；IPC 固定 0 |
| 12 | 4 | 非零请求编号；IPC 广播事件为 0 |
| 16 | 4 | 载荷长度，不超过 8192 |
| 20 | 4 | CRC32 |
| 24 | N | 载荷 |

CRC32 使用 IEEE 反射多项式 `0xEDB88320`，初值及最终 XOR 均为 `0xFFFFFFFF`；覆盖帧头前 20 字节与载荷，跳过 CRC 字段。标准向量 `123456789` 为 `0xCBF43926`。CRC 不是认证机制。

普通应答载荷前 4 字节为目标系统 errno，0 表示成功，后接结果。错误应答不带结果。RV1106 使用 Linux errno；macOS 测试后端使用 Darwin errno，主机仅对 EAGAIN（Linux 11／Darwin 35）作异步完成重试。

## 维护会话与调度

主机必须先 HELLO，随后使用返回的非零随机会话号。每个会话只允许一个未完成请求；请求编号单调增加，不回绕。超时重发必须保持操作码、请求编号与载荷完全相同。

服务缓存最近 16 个应答。相同编号及请求 CRC 重放原应答，不再次执行命令；相同编号不同 CRC 静默忽略；已超出缓存的旧编号返回 ESTALE。设备重启使旧会话失效，主机不得以新会话自动重试结果未知的 Shell 命令。

新的 HELLO nonce 替换现有会话，取消旧命令并关闭旧文件句柄；上传临时文件保留。重复 HELLO nonce 返回同一会话号。业务抢占／参数切换可能使会话失效，主机重新 HELLO 后显式恢复文件任务。

串口应答载荷最多 256 字节（包含 errno）。主机应在总线上预留完整请求、应答、GPIO 换向和排空时间。设备没有主动推送日志；输出通过 POLL 获取。设备错误地址、未知会话和非请求帧不应答；不支持的操作码在有效会话内返回 ENOTSUP。

解析遇到损坏候选逐字节重新同步，不能确认为维护的数据返回业务流；不完整候选在无新字节超过 `2000 ms + 8216 × 12000 / baud` 后返回业务流。维护被禁止时仍过滤合法 H485 帧，但不执行。QJY 已识别业务载荷按声明长度保护，不因该超时重新解释载荷。

## 维护操作

| 操作码 | 请求载荷 | 成功结果（不含 errno） |
|---|---|---|
| 1 HELLO | `nonce:u64`，帧头 session=0 | `session:u32, file_block:u32`，block=240 |
| 2 EXEC | 1～4096 字节 UTF-8 命令，不含 NUL | 空 |
| 3 POLL | `stream:u8, offset:u32`；0 stdout、1 stderr | `state:u8, truncated:u8, exit:i32, total:u32, eof:u8, bytes[0..237]` |
| 4 CANCEL | 空 | 空 |
| 5 PUT_BEGIN | `size:u64, sha256:32 bytes, overwrite:u8, relative_path` | `resume_offset:u64` |
| 6 PUT_DATA | `offset:u64, bytes[1..240]` | `next_offset:u64` |
| 7 PUT_COMMIT | 空 | 空；计算校验中返回 EAGAIN |
| 8 GET_BEGIN | 相对路径，不含 NUL | `size:u64, sha256:32 bytes`；计算校验中返回 EAGAIN |
| 9 GET_DATA | `offset:u64` | `offset:u64, bytes[0..240]` |
| 10 STATUS | 空 | `command_state:u32, exit:i32, last_upload_offset:u64` |

POLL state：0 无命令、1 运行、2 已结束、3 已取消、4 超时。退出码未产生前为 -1；被信号终止时为 `128 + signal`。`eof=1` 表示子进程已退出且两个输出管道均已排空。结束判定需同时检查状态、eof 和读取偏移。

`@name args` 调用注册命令，`@version` 内建；其余字符串仅在启用 `--allow-shell` 时交给 `/bin/sh -c`。0.2.1 起，同一维护会话保留工作目录和 OLDPWD；Shell 环境变量、函数及别名仍不跨请求保留。

EAGAIN 是已完成的“请稍后查询”应答，下一次检查使用新的请求编号；传输超时才以原编号重发。PUT_COMMIT/GET_BEGIN 的 SHA-256 每轮最多处理 16 KiB，避免一次性扫描大文件阻塞事件循环。

上传每块写入并同步后返回偏移；重复已确认块必须与现有内容一致。文件身份由目的路径、大小和哈希确定。SHA-256 通过后在同目录原子安装；不覆盖模式使用原子 link 防止目标并发创建。下载持有文件描述符并检查大小、mtime 和 ctime，发现源变化返回 ESTALE。文件协议限 64 MiB。

## 本地 IPC

| 操作码 | 请求 | 结果 |
|---|---|---|
| 1 ACQUIRE | `wait_ms:u32` | 空 |
| 2 RELEASE | 空 | 空；已接受 TX 排空后应答 |
| 3 SUBSCRIBE | 空 | 空 |
| 4 UNSUBSCRIBE | 空 | 空 |
| 5 WRITE | 1～8192 字节原始业务数据 | `accepted:u32` |
| 6 GET_CONFIG | 空 | `baud:u32, bits:u32, parity:u32, stops:u32` |
| 7 SET_CONFIG | 同上 | 空；TX 未排空返回 EBUSY |
| 8 STATUS | 空 | `mode:u32, owner:u32, epoch:u32, queued:u32, error:u32, rx_sequence:u32` |
| 12 TX_BEGIN | `total:u32`，1～16 MiB | 空；该请求编号为整帧 ticket |
| 13 TX_DATA | 1～8192 字节 | 空；有缓冲空间才确认接收 |
| 14 TX_ABORT | 空 | 空；实际取消完成仍通过 TX_DONE 通知 |
| 0x70 RX | 服务主动发送 | `sequence:u32, epoch:u32, bytes`，无 errno 前缀 |
| 0x71 EVENT | 服务主动发送 | `type:u32, a:u32, b:u32`，无 errno 前缀 |

mode：0 idle、1 coexist。owner：0 无控制者，其他值是服务内部连接槽编号，仅用于诊断，不是可复用的权限凭据。parity：0 none、1 odd、2 even；bits=5～8；stops=1 或 2。波特率支持 1200、2400、4800、9600、19200、38400、57600、115200、230400，以及平台定义了常量时的 460800／921600。

| 事件 type | a | b |
|---|---|---|
| 1 OWNER | 当前控制者，0 表示无 | epoch |
| 2 TX_DONE | WRITE 或 TX_BEGIN 请求编号／SDK ticket | errno，0 为发送成功 |
| 3 RX_LOST | 丢字节数低 32 位 | 丢字节数高 32 位 |
| 4 CONFIG | epoch | baud |
| 5 FAULT | errno | 0 |

每个连接输出队列 64 KiB；优先丢最旧且未开始写出的 RX 消息。如果全为不可丢的应答／事件而无法容纳新消息，关闭该客户端连接，避免阻塞服务。SDK 另设 64 KiB RX 缓存、最多 1024 个控制事件；SDK RX 溢出也产生 RX_LOST，事件超限显式返回 ENOBUFS。

Linux 使用 Unix `SOCK_SEQPACKET`，一条消息一帧；macOS 使用流式 Socket 并按帧头重组。不支持 Linux 客户端与 macOS 服务跨机器 IPC，这也不是网络协议。

0.1.1 的板端锁文件兼容修复不改变上述协议 v1 或 SDK 公共接口。

## HDTools 文件系统扩展（hd485d 0.2.0）

保持协议 v1、操作码 1～10 和原 `--root` 行为不变。扩展文件接口仅在显式设置
`--fs-root DIR` 时启用；未设置时能力查询可用，其余文件系统操作返回 ENOTSUP。
`--fs-root /` 对整个设备文件系统开放服务进程已有的文件权限。Shell 开关仍独立。

扩展路径为 UTF-8 绝对路径，以配置的 fs-root 映射为 `/`。路径小于 4096 字节，
单分量最多 255 字节，不接受 NUL、`.`、`..`、重复分隔符、结尾 `/`（根目录除外）
及 `.hd485-` 内部名称。每级目录使用 openat/O_NOFOLLOW，符号链接可查看属性，
不可跟随或作为传输目标；仅普通文件可以传输。虚拟文件系统中大小为零的动态文件
不适合快照下载，请通过命令终端读取（例如 `cat /proc/version`）。

| 操作码 | 请求 | 成功结果（不含 errno） |
|---|---|---|
| 32 FS_INFO | 空 | `revision:u16=1, flags:u16, max_file:u64, block:u16=240, root_len:u16, root_bytes` |
| 33 FS_LIST | 绝对目录路径 | 空，建立目录遍历游标 |
| 34 FS_READ | `offset:u64`，首次为 0 | `eof:u8, offset:u64, bytes[0..240]` |
| 35 FS_CLOSE | 空 | 空，关闭目录／属性读取 |
| 36 FS_STAT | 绝对路径 | 空，将单个属性记录放入 FS_READ 流 |
| 37 FS_PUT | 同 PUT_BEGIN，但使用绝对路径 | `resume_offset:u64` |
| 38 FS_GET | 绝对路径 | 同 GET_BEGIN，可能返回 EAGAIN |
| 39 FILE_CLOSE | 空 | 空，关闭传输句柄，保留上传断点 |

flags 位 0：文件扩展启用；位 1：Shell 启用；位 2：Shell 会话工作目录保留（0.2.1 新增，仅启用 Shell 时置位）。root_bytes 是配置根目录的 UTF-8 字节，
最长 236 字节；未启用时为空。max_file 当前为 64 MiB。旧服务对 32 返回 ENOTSUP，
客户端仍可使用 EXEC/POLL/CANCEL，但禁用文件浏览并提示升级。

FS_READ 按顺序读取，不支持任意 seek；offset 必须为上一应答 offset 加数据长度。
重发相同请求使用原请求编号，由公共响应缓存重放，不能用新编号重复旧偏移。
扫描大量内部临时文件时会返回 EAGAIN，客户端以新请求编号和相同偏移继续。

eof=1 表示此次数据之后无更多记录。每个记录布局如下，可跨多个应答分片：

```
record_size:u32     # 后续记录长度，不含本字段
mode:u32           # POSIX 文件类型及权限位
size:u64
modified:i64       # Unix 秒
name_len:u16
link_len:u16
name_bytes         # 原始目录项名字节；HDTools 要求合法 UTF-8
link_bytes         # 符号链接目标字节；非链接为空
```

板端只保留一个最多 8192 字节的记录，不缓存完整目录。目录自身 size/mtime/ctime
或身份改变时返回 ESTALE，客户端应整体刷新，不能展示为完整快照；文件内容变化
仍需传输阶段检测。隐藏文件包括在列表中，内部 `.hd485-` 文件除外，排序由客户端完成。
FS_STAT `/` 返回名字 `.`；属性流和目录流共享一个游标。新会话、业务抢占、关闭服务
均释放游标。权限不足或遍历中条目消失时返回错误，由客户端提示刷新。

FS_PUT/FS_GET 与旧上传下载共享一个传输上下文，开始新传输会关闭旧句柄。
数据及提交继续使用 6/7/9，SHA-256 与断点语义不变，不依赖板端 sha256sum。
FS_PUT 提交前检查目的文件是否仍与开始时的 dev/ino/size/mode/mtime/ctime 一致，
不一致返回 ESTALE。检查之后使用同目录原子替换；这是提交前冲突检测，不是对其他
文件写入者的事务锁。新文件权限 0644，覆盖保留原目标 rwx 权限位，不保留 setuid/setgid。
旧 PUT_BEGIN 的权限和根目录规则不变。FILE_CLOSE 不删除 `.hd485-*.part/.meta`。


### RV1106 实测换向间隔（2026-11-19）

此板 GPIO35 方向控制链路上，主机应在完整应答后至少等待 **60 ms** 再发下一请求。
本次连续 STATUS 测量：间隔 0 / 20 ms 时 4 请求发送 8 次（每次重试一次）；
间隔 60 / 120 / 250 ms 时各 3 请求发送 3 次。60 ms 情况应答耗时约 15～19 ms。
HDTools 0.1.1 已在会话层统一应用该间隔（含错误应答），不依赖界面或操作层 sleep。
这是当前板卡／驱动／换向链路的保守实测值，不是协议帧字段或通用设备性能保证。


## Shell 会话工作目录（hd485d 0.2.1）

同一 HELLO 会话中，普通命令正常结束（包括非零退出码）后记录 Shell 的工作目录和
OLDPWD，下条命令从该目录执行。支持 `cd /userdata`、相对路径、带空格／引号路径、
`cd -` 及逻辑符号链接路径。目录状态走独立控制管道，stdout/stderr 与退出码不混入
控制标记。POLL 的 eof 包括该管道已排空，避免命令结束与目录更新的竞争。

初始目录为服务启动时所在目录；新 HELLO、业务抢占导致会话重置后恢复初始目录。
被 CANCEL 或超时中止的命令不提交目录变更。目录更改只影响本会话后续 Shell 子进程，
不改变服务本身 cwd、文件浏览路径、`--root` 或 `--fs-root`。

这仍是无 PTY 的逐命令执行，并非完整交互式 Shell：export、函数和 alias 不保留。
内部使用 FD 9 和 EXIT trap 回报目录；显式覆盖该描述符／trap、或使用 exec 替换 Shell
可能使本条命令不能回报目录，此时保留上次已确认的目录。
Python CLI 每次启动都会创建新会话；跨 CLI 进程不保留目录。同一 HDTools 连接则保留。

## 0.3.0 交互 PTY（方式一）

`--allow-shell --allow-pty --mode idle` 启用；FS_INFO flags bit3（8）表示支持。
普通 EXEC 和文件 API 不变。单维护会话最多一个 PTY；PTY 存在时 EXEC 和文件操作
返回 EBUSY，状态查询仍可用。OPEN 与正在执行的命令/文件任务互斥。

| 操作码 | 请求 | 成功应答 |
|---|---|---|
| 48 PTY_OPEN | rows:u16, cols:u16 | PTY 状态，无输出数据 |
| 49 PTY_IO | id:u32, input_offset:u64, output_ack:u64, bytes[0..200] | PTY 状态及输出 |
| 50 PTY_RESIZE | id:u32, rows:u16, cols:u16 | PTY 状态，无输出数据 |
| 51 PTY_CLOSE | id:u32 | PTY 状态，可重复关闭 |
| 52 PTY_STATUS | id:u32 | PTY 状态，无输出数据 |

PTY 状态（不含通用 errno）：`id:u32, state:u8, exit:i32, eof:u8,
input_next:u64, output_offset:u64, more:u8, length:u16, bytes[length]`，固定头 29 字节。
state：1 运行、2 Shell 退出、3 关闭/取消/抢占、4 超时。exit 未产生为 -1。
eof 仅在子进程已回收、输出已排空并确认后置 1。尺寸范围 1～1000。
PTY ID 在服务生命周期内递增，不复用。不存在或不匹配的 ID 返回 ESTALE。

input_offset 必须等于 input_next；成功表示输入已进入有界队列，不表示程序已经执行。
输入队列 4 KiB，容量不足整块返回 EAGAIN，不接受其中一部分。超时重发保持请求 ID 和
载荷不变，由通用应答缓存去重；EAGAIN 后用新请求 ID 重试同一偏移。
output_ack 确认已接收的输出前缀，不得超出先前 IO 应答发送的范围。IO 返回从确认位置
开始最多 200 字节，more 表示还有积压；输出队列 64 KiB，满时对 PTY 施加背压而不丢字节。
关闭或超时会丢弃未确认的缓冲输出；随后到达、越过保留范围的 IO 确认返回 EINVAL，
可用 STATUS 查询终态。stdout/stderr 合并为终端流，不能按普通 EXEC 的双流解析。

PTY Shell 使用控制终端、TERM=xterm，从普通命令会话当前目录启动；其内部目录、环境和
别名保留，关闭后不回写普通命令的目录。主机通过尺寸变更 ioctl 触发 SIGWINCH；
Ctrl+C/Ctrl+D 等作为原始输入字节交给终端行规程。需要 HDTools 的终端模拟器显示。

`--pty-timeout-ms` 默认 20000，范围 5000～300000；有效 PTY 请求更新保活。
主机正常轮询维持连接。业务抢占立即禁止维护通信，关闭 PTY 并异步终止会话，500 ms
宽限后强制回收；不阻塞业务仲裁。新 HELLO 取消旧会话。刻意 setsid 脱离的守护进程
不属于交互作业回收保证。默认不支持断线恢复。

HDTools 取消读取后不发送清理或后续按键，板端由超时回收；重新打开是显式新会话。
正常“关闭交互终端”发送 CLOSE。运行时可能需要最多约 500 ms 完成进程回收。
EOF/退出信息与终端提示符无关，不通过识别 Shell 文本决定会话是否结束。


## 0.4.0 业务接收过滤与原始接收段

默认 SUBSCRIBE 和 ACQUIRE 自动订阅均只接收业务字节。先分类，再广播：结构、版本、
长度及 CRC 正确的 H485 帧不进入订阅队列，包含外部地址、旧会话、未知操作码和应答帧。
维护是否执行仍遵循地址、会话、owner、等待申请和模式检查；隐藏不等于执行。
不完整候选最多缓存 HD_PACKET 字节，空闲超时为 `2000 + HD_PACKET*12000/baud` ms；
CRC 错误、非法头或超时未确认字节按顺序作为普通数据输出。接收序号现在按业务输出块递增。

`--rx-profile generic|qjy`：默认 generic；RV1106 配套脚本选择 qjy。
qjy 识别 AA 5A 和 8 字节业务头，长度不超过 512 KiB 时，保护后续 payload+CRC16：
流式转发、不检查其中 H485、不缓冲整帧，业务 CRC 由原业务解析器验证。
已确认长度的业务载荷不会因任意空闲间隔被重新解释；这与原业务解析器的长度语义一致。
任意未知裸流无法只靠魔数无歧义区分，应通过原始接收段接口声明；不能把 generic 过滤
外推为对任意二进制业务协议均透明。

新增 I485 IPC（H485 主机协议不变）：

| 操作 | 编号 | 请求 | 成功应答（不含 errno） |
|---|---:|---|---|
| CAPABILITIES | 9 | 空 | flags:u32 LE |
| RX_RAW_BEGIN | 10 | bytes:u64 LE | 空 |
| RX_RAW_END | 11 | 空 | 空 |

flags：bit0 业务接收过滤、bit1 定长原始接收、bit2 QJY 帧载荷保护。
SDK 对应 `hd485_get_capabilities`、`hd485_rx_raw_begin`、`hd485_rx_raw_end`。
BEGIN/END 仅 owner 可调用；bytes 必须非零，已有候选头、未结束业务帧或原始段返回 EBUSY。
BEGIN 成功后接下来的精确 N 字节全部为业务数据，包括任何看似 H485 的内容，既不剔除也不执行。
必须在向业务主机发送 OTA 接收许可前声明。收满自动结束；END、RELEASE 或 owner 断开时中止。
原始段没有独立超时，由业务 OTA 原有超时负责结束，避免慢速合法传输被重新解释为维护命令。

## 0.4.1 整帧发送与事件唤醒

`CAPABILITIES` 新增 bit 3 (`HD485_CAP_STREAM_TX=8`)。老 WRITE 接口保持不变；新版 camera UART 1.0.27 对超过 8192 字节的完整业务帧使用 BEGIN/DATA，旧服务无此能力时回退 WRITE。小帧仍使用 WRITE。

BEGIN 必须由 owner 在无 TX 时调用，声明包含业务头、载荷、CRC 的完整长度。DATA 按顺序追加，累计达到声明长度后自动收尾，不另发 END。接受 DATA 只代表进入有界发送队列，**不代表物理发送完成**。重复 DATA 不具备幂等性；结果未知的 IPC 失败必须断开，禁止重放半帧。

串口 worker 使用固定 32 KiB 环形缓冲及 8 KiB 工作块，服务最多暂存一个 8 KiB 等待信用的 DATA。缓冲满时暂缓该 RPC 应答，其他客户端和 UART 接收仍被服务。空缓冲连续等待 2 秒取消未完成帧；主动 ABORT、owner 断线也取消，清除待发数据并释放 DE。取消/缺数据超时报告 ECANCELED/ETIMEDOUT，允许下一帧继续；真实串口 I/O 故障仍报告 FAULT。未完成流式发送时 RELEASE/WRITE/SET_CONFIG 返回 EBUSY；先 ABORT 并等待 TX_DONE，或等待正常完成后再 RELEASE。

GPIO 后端整帧只置高 DE 一次，只在最终 tcdrain 和原有 guard 后置低 DE、发送一次 TX_DONE。不同 DATA 间不做 drain/换向。生产消费、完成通知使用条件变量和非阻塞通知管道，不使用忙轮询；daemon 原有维护定时 tick 保留，发送完成无需等 tick。硬件/kernel 后端方向行为由其驱动决定。

设置 `HD485_TX_TRACE=1` 可在 daemon/业务进程日志记录单调时钟微秒、ticket、长度和阶段，不记录业务载荷。阶段含 ipc_write/ipc_begin、submit、worker_start、write、drain_begin/end、ready、tx_done；App 另记录 request/accepted/done。默认关闭，性能正式复测关闭，计时定位另开独立运行。
