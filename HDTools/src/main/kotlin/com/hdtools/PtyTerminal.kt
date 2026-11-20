package com.hdtools

import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.awt.SwingPanel
import com.jediterm.core.util.TermSize
import com.jediterm.terminal.TtyConnector
import com.jediterm.terminal.ui.JediTermWidget
import com.jediterm.terminal.ui.settings.DefaultSettingsProvider
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import javax.swing.SwingUtilities

internal data class PtyReply(val id:Int,val state:Int,val exit:Int,val eof:Boolean,val input:Long,
                             val offset:Long,val more:Boolean,val data:ByteArray) {
    companion object {
        fun parse(bytes:ByteArray):PtyReply {
            require(bytes.size>=29) {"PTY 应答不完整"}
            val b=bytes.le();val id=b.int;val state=b.get().toInt();val exit=b.int;val eof=b.get().toInt()!=0
            val input=b.long;val offset=b.long;val more=b.get().toInt()!=0;val size=b.short.toInt() and 65535
            require(size<=200 && b.remaining()==size && state in 1..4 && input>=0 && offset>=0)
            return PtyReply(id,state,exit,eof,input,offset,more,ByteArray(size).also {b.get(it)})
        }
    }
}
/** All physical writes are performed by run(), through Session's single request gate. */
internal class PtyConnection(private val wire:Session,private val notice:(String)->Unit={}):TtyConnector {
    private val guard=Any()
    private var input=byteArrayOf()
    private val output=ArrayBlockingQueue<String>(512)
    private val dimensions=AtomicReference<TermSize?>(null)
    @Volatile private var closing=false
    @Volatile private var ended=false
    @Volatile private var exitCode=-1
    private var readPending=""
    override fun getName()="H485 PTY"
    override fun isConnected()=!ended
    override fun ready()=readPending.isNotEmpty() || output.isNotEmpty() || ended
    override fun write(text:String)=write(text.toByteArray(Charsets.UTF_8))
    override fun write(bytes:ByteArray) {
        synchronized(guard) {
            if(ended || closing)return
            if(bytes.size>4096-input.size) {notice("输入队列已满（4 KiB），本次输入未发送；请分段粘贴");return}
            input+=bytes
        }
    }
    override fun resize(size:TermSize) {dimensions.set(TermSize(size.columns.coerceIn(1,1000),size.rows.coerceIn(1,1000)))}
    override fun close() {closing=true;synchronized(guard) {input=byteArrayOf()}}
    override fun waitFor():Int {while(!ended)Thread.sleep(20);return exitCode}
    override fun read(buffer:CharArray,offset:Int,length:Int):Int {
        if(length==0)return 0
        while(readPending.isEmpty()) {
            val text=output.poll(100,TimeUnit.MILLISECONDS)
            if(text!=null)readPending=text else if(ended)return -1
        }
        val count=minOf(length,readPending.length)
        readPending.toCharArray(buffer,offset,0,count);readPending=readPending.substring(count)
        return count
    }
    private fun emit(text:String) {
        if(text.isNotEmpty() && !output.offer(text))error("终端显示队列已满，请关闭后重新打开终端")
    }
    fun run():String {
        try {
            val first=PtyReply.parse(wire.call(48,buffer(4).putShort(24).putShort(80).array()))
            val id=first.id;var inputOffset=0L;var outputOffset=0L;val decoder=Utf8Chunks()
            while(!closing) {
                dimensions.getAndSet(null)?.let {size->
                    wire.call(50,buffer(8).putInt(id).putShort(size.rows.toShort()).putShort(size.columns.toShort()).array())
                }
                val bytes=synchronized(guard) {input.take(200).toByteArray()}
                val request=buffer(20+bytes.size).putInt(id).putLong(inputOffset).putLong(outputOffset).put(bytes).array()
                val result=try {PtyReply.parse(wire.call(49,request))} catch(e:DeviceError) {
                    if(e.errno in listOf(11,35)) {Thread.sleep(20);continue}
                    throw e
                }
                require(result.id==id && result.input==inputOffset+bytes.size && result.offset==outputOffset) {"PTY 数据偏移不一致"}
                synchronized(guard) {if(!closing)input=input.copyOfRange(bytes.size,input.size)}
                inputOffset=result.input;outputOffset+=result.data.size
                emit(decoder.feed(result.data))
                if(result.eof) {
                    emit(decoder.finish());exitCode=result.exit
                    return if(result.state==4) "交互终端已超时结束" else "交互终端已结束 · 退出码 ${result.exit}"
                }
                if(!result.more && result.data.isEmpty()) {
                    repeat(25) {if(!closing && synchronized(guard) {input.isEmpty()} && dimensions.get()==null)Thread.sleep(10)}
                }
            }
            wire.call(51,buffer(4).putInt(id).array())
            return "交互终端已关闭"
        } finally {
            // No cleanup writes here: cancelRead forbids further automatic requests.
            synchronized(guard) {input=byteArrayOf()};ended=true
        }
    }
}

@Composable internal fun InteractiveTerminal(connection:PtyConnection,modifier:Modifier=Modifier) {
    val widget=remember(connection) {
        var result:JediTermWidget?=null
        val create={result=JediTermWidget(80,24,object:DefaultSettingsProvider() {
            override fun getBufferMaxLinesCount()=2000
            override fun audibleBell()=false
            override fun getTerminalFontSize()=14f
        }).also {it.ttyConnector=connection;it.start()}}
        if(SwingUtilities.isEventDispatchThread())create() else SwingUtilities.invokeAndWait(create)
        result!!
    }
    DisposableEffect(widget) {onDispose {SwingUtilities.invokeLater {widget.close()}}}
    SwingPanel(factory={widget},modifier=modifier)
}
