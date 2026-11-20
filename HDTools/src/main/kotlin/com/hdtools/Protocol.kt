package com.hdtools

import com.fazecast.jSerialComm.SerialPort
import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.ByteOrder.LITTLE_ENDIAN
import java.security.SecureRandom
import java.util.zip.CRC32
import java.util.concurrent.atomic.AtomicBoolean

internal fun buffer(n: Int): ByteBuffer = ByteBuffer.allocate(n).order(LITTLE_ENDIAN)
internal fun ByteArray.le(): ByteBuffer = ByteBuffer.wrap(this).order(LITTLE_ENDIAN)
internal data class Packet(val op: Int, val address: Int, val session: Int, val id: Int, val body: ByteArray)
internal object Wire {
    fun encode(p: Packet): ByteArray {
        require(p.body.size <= 8192)
        val b = buffer(24+p.body.size).put("H485".toByteArray()).put(1).put(p.op.toByte())
            .putShort(p.address.toShort()).putInt(p.session).putInt(p.id).putInt(p.body.size).putInt(0).put(p.body).array()
        val crc = CRC32().apply { update(b,0,20); update(b,24,p.body.size) }.value.toInt()
        ByteBuffer.wrap(b).order(LITTLE_ENDIAN).putInt(20,crc)
        return b
    }
}
internal class FrameParser {
    private var pending = byteArrayOf()
    fun clear() { pending = byteArrayOf() }
    fun feed(bytes: ByteArray): List<Packet> {
        pending += bytes
        val result = mutableListOf<Packet>()
        var offset = 0
        while (pending.size-offset >= 24) {
            val b = ByteBuffer.wrap(pending,offset,pending.size-offset).slice().order(LITTLE_ENDIAN)
            val size = b.getInt(16)
            if (b.getInt(0) != 0x35383448 || b.get(4).toInt()!=1 || size !in 0..8192) { offset++; continue }
            if (pending.size-offset < 24+size) break
            val crc = CRC32().apply {update(pending,offset,20);update(pending,offset+24,size)}.value.toInt()
            if (crc != b.getInt(20)) {offset++;continue}
            result += Packet(b.get(5).toInt() and 255,b.getShort(6).toInt() and 65535,b.getInt(8),b.getInt(12),pending.copyOfRange(offset+24,offset+24+size))
            offset += 24+size
        }
        pending = pending.copyOfRange(offset,pending.size)
        return result
    }
}
internal interface WireIO: Closeable {
    fun write(bytes: ByteArray)
    fun read(): ByteArray
}
internal interface SerialDriver: Closeable {
    val isOpen: Boolean
    fun write(bytes: ByteArray, offset: Int): Int
    fun read(bytes: ByteArray): Int
}
private fun openDriver(path: String, baud: Int): SerialDriver {
    require(baud in listOf(1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600))
    val port=SerialPort.getCommPort(path)
    port.setComPortParameters(baud,8,SerialPort.ONE_STOP_BIT,SerialPort.NO_PARITY)
    port.setFlowControl(SerialPort.FLOW_CONTROL_DISABLED)
    port.setComPortTimeouts(SerialPort.TIMEOUT_NONBLOCKING,0,0)
    check(port.openPort()) {"无法打开串口，请检查权限及占用：$path"}
    try {port.flushIOBuffers()} catch(e:Exception) {port.closePort();throw e}
    return object: SerialDriver {
        override val isOpen get()=port.isOpen
        override fun write(bytes: ByteArray,offset: Int)=port.writeBytes(bytes,bytes.size-offset,offset)
        override fun read(bytes: ByteArray)=port.readBytes(bytes,bytes.size)
        override fun close() {port.closePort()}
    }
}
internal class SerialIO(private val driver: SerialDriver, private val baud: Int,
                        val traffic: TrafficCounters=TrafficCounters()): WireIO {
    constructor(path: String,baud: Int): this(openDriver(path,baud),baud)
    override fun write(bytes: ByteArray) {
        var offset=0
        val deadline=System.nanoTime()+maxOf(5000L,bytes.size*12000L/baud+2000)*1_000_000L
        while(offset<bytes.size) {
            check(driver.isOpen) {"串口已断开"}
            val count=driver.write(bytes,offset)
            traffic.sent(count) // Include successful partial writes even when this request fails.
            check(count>=0 && System.nanoTime()<deadline) {"串口写入失败或超时"}
            offset+=count
            if(count==0) Thread.sleep(2)
        }
    }
    override fun read(): ByteArray {
        val bytes=ByteArray(4096)
        val count=driver.read(bytes)
        traffic.received(count)
        check(count>=0 && driver.isOpen) {"串口已断开"}
        return bytes.copyOf(count)
    }
    override fun close() { driver.close() }
}
internal class ConnectionLost(message: String, cause: Throwable?=null): Exception(message,cause)
internal class DeviceError(val errno: Int): Exception("设备错误 $errno："+when(errno) {
    2 -> "路径不存在"; 13 -> "权限不足"; 17 -> "目标或断点文件已存在"; 22 -> "参数或文件类型不支持"
    28 -> "设备空间不足"; 40,62 -> "不允许通过符号链接访问"; 95,45 -> "服务未启用此功能或版本过旧"
    116,70 -> "文件、目录或会话已变化，请刷新或重新连接"; else -> "操作失败"
})
internal enum class SerialMode(val label: String) {
    CLOSED("未连接"), IDLE("空闲 · 可读写"), TURNAROUND("换向等待"), WRITING("写模式 · 发送中"), READING("读模式 · 等待应答")
}
internal data class SerialState(val mode: SerialMode, val deadlineNanos: Long=0, val attempt: Int=0, val cancelling: Boolean=false)
internal class SerialBusy: Exception("串口正在处理请求，读模式下禁止额外写入")
internal class ReadCancelled: Exception("已取消本次读取；设备执行结果未知，请刷新核对或重新连接。本次请求不会自动补发。")
internal class Session(val io: WireIO, val address: Int=1, val timeoutMs: Long=1000,
                       private val baud: Int=230400, private val turnaroundMs: Long=60,
                       private val onState: (SerialState)->Unit={}): Closeable {
    private val parser=FrameParser()
    private var sequence=0L
    private var nextWriteAt=0L
    private var session=0
    private val active=AtomicBoolean()
    private val stateGuard=Any()
    @Volatile private var closed=false
    @Volatile private var readAborted=false
    @Volatile var state=SerialState(SerialMode.IDLE);private set
    init { require(address in 1..65535 && turnaroundMs>=0 && timeoutMs>0 && baud>0) }
    private fun publish(value: SerialState) = synchronized(stateGuard) {
        if(!closed && state!=value) {state=value;onState(value)}
    }
    fun cancelRead(): Boolean = synchronized(stateGuard) {
        if(closed || state.mode!=SerialMode.READING || readAborted) return false
        readAborted=true
        state=state.copy(cancelling=true);onState(state)
        true
    }
    // Explicit new operation only. Do not reset this in call(): finally blocks must not send cleanup
    // requests after the user aborted the current receive.
    fun resetReadCancellation() = synchronized(stateGuard) {
        check(!active.get()) {"当前请求尚未退出"}
        readAborted=false
    }
    private fun checkRead() {
        if(readAborted) throw ReadCancelled()
        if(closed) throw ConnectionLost("串口已关闭")
    }
    private fun finishRead() = synchronized(stateGuard) {
        checkRead()
        publish(SerialState(SerialMode.IDLE))
    }
    fun hello() {
        val nonce=ByteArray(8).also {SecureRandom().nextBytes(it)}
        val result=call(1,nonce).le()
        require(result.remaining()==8)
        session=result.int
        require(session!=0 && result.int==240) {"不支持的会话或文件块大小"}
    }
    fun call(op: Int, data: ByteArray=byteArrayOf()): ByteArray {
        if(!active.compareAndSet(false,true)) throw SerialBusy()
        try {
            checkRead()
            check(++sequence<=0xffffffffL) {"会话编号耗尽，请重新连接"}
            val id=sequence.toInt()
            val packet=Wire.encode(Packet(op,address,if(op==1)0 else session,id,data))
            repeat(3) {attempt->
                checkRead()
                if(System.nanoTime()<nextWriteAt) publish(SerialState(SerialMode.TURNAROUND,nextWriteAt))
                while(System.nanoTime()<nextWriteAt) {
                    checkRead()
                    val left=nextWriteAt-System.nanoTime()
                    if(left>0) Thread.sleep(minOf(2L,left/1_000_000L),(left%1_000_000L).toInt())
                }
                checkRead()
                publish(SerialState(SerialMode.WRITING,attempt=attempt+1))
                try {io.write(packet)} catch(e:Exception) {throw ConnectionLost("串口写入失败，请重新连接：${e.message}",e)}
                val until=System.nanoTime()+(timeoutMs+(packet.size+280)*12000L/baud)*1_000_000L
                publish(SerialState(SerialMode.READING,until,attempt+1))
                while(System.nanoTime()<until) {
                    checkRead()
                    val bytes=try {io.read()} catch(e:Exception) {throw ConnectionLost("串口读取失败，请重新连接：${e.message}",e)}
                    checkRead()
                    for(p in parser.feed(bytes)) {
                        if(p.op!=(op or 128) || p.id!=id || p.address!=address || (op!=1 && p.session!=session)) continue
                        require(p.body.size>=4) {"应答不完整"}
                        nextWriteAt=System.nanoTime()+turnaroundMs*1_000_000L
                        finishRead()
                        val error=p.body.le().int
                        if(error!=0) throw DeviceError(error)
                        return p.body.copyOfRange(4,p.body.size)
                    }
                    if(bytes.isEmpty()) Thread.sleep(2)
                }
                checkRead()
                // Leave receive mode before retrying; retries preserve the exact request ID.
                publish(SerialState(SerialMode.IDLE))
                parser.clear()
            }
            throw ConnectionLost("设备未响应：可能业务占用或连接异常。操作结果未知，请重新连接后核对；不会自动重新执行。")
        } finally {
            if(readAborted) {
                parser.clear()
                nextWriteAt=System.nanoTime()+turnaroundMs*1_000_000L
            }
            publish(SerialState(SerialMode.IDLE))
            active.set(false)
        }
    }
    fun ready(op: Int, data: ByteArray=byteArrayOf(), check: ()->Unit={}): ByteArray {
        val deadline=System.nanoTime()+180_000_000_000L
        while(true) {
            check()
            try { return call(op,data) } catch(e: DeviceError) {
                if(e.errno !in listOf(11,35) || System.nanoTime()>=deadline) throw e
                Thread.sleep(20)
            }
        }
    }
    override fun close() {
        synchronized(stateGuard) {
            if(closed) return
            closed=true;state=SerialState(SerialMode.CLOSED);onState(state)
        }
        io.close()
    }
}
