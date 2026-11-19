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

解析遇到损坏帧逐字节重新同步；不完整帧在无新字节超过 `2000 ms + 8216 × 12000 / baud` 后清空。维护被禁止时不解析数据。

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

`@name args` 调用注册命令，`@version` 内建；其余字符串仅在启用 `--allow-shell` 时交给 `/bin/sh -c`。各次请求不共享工作目录修改或 Shell 环境。

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
| 0x70 RX | 服务主动发送 | `sequence:u32, epoch:u32, bytes`，无 errno 前缀 |
| 0x71 EVENT | 服务主动发送 | `type:u32, a:u32, b:u32`，无 errno 前缀 |

mode：0 idle、1 coexist。owner：0 无控制者，其他值是服务内部连接槽编号，仅用于诊断，不是可复用的权限凭据。parity：0 none、1 odd、2 even；bits=5～8；stops=1 或 2。波特率支持 1200、2400、4800、9600、19200、38400、57600、115200、230400，以及平台定义了常量时的 460800／921600。

| 事件 type | a | b |
|---|---|---|
| 1 OWNER | 当前控制者，0 表示无 | epoch |
| 2 TX_DONE | WRITE 请求编号／SDK ticket | errno，0 为发送成功 |
| 3 RX_LOST | 丢字节数低 32 位 | 丢字节数高 32 位 |
| 4 CONFIG | epoch | baud |
| 5 FAULT | errno | 0 |

每个连接输出队列 64 KiB；优先丢最旧且未开始写出的 RX 消息。如果全为不可丢的应答／事件而无法容纳新消息，关闭该客户端连接，避免阻塞服务。SDK 另设 64 KiB RX 缓存、最多 1024 个控制事件；SDK RX 溢出也产生 RX_LOST，事件超限显式返回 ENOBUFS。

Linux 使用 Unix `SOCK_SEQPACKET`，一条消息一帧；macOS 使用流式 Socket 并按帧头重组。不支持 Linux 客户端与 macOS 服务跨机器 IPC，这也不是网络协议。

0.1.1 的板端锁文件兼容修复不改变上述协议 v1 或 SDK 公共接口。
