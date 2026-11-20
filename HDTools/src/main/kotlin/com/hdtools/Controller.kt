package com.hdtools

import androidx.compose.runtime.*
import com.fazecast.jSerialComm.SerialPort
import kotlinx.coroutines.*
import java.awt.FileDialog
import java.awt.Frame
import java.io.File
import java.util.Properties
import java.util.concurrent.atomic.AtomicBoolean
import javax.swing.SwingUtilities

internal data class Confirmation(val text: String,val action: ()->Unit)
internal class Controller(private val scope: CoroutineScope,val demo: Boolean=false) {
    var port by mutableStateOf("")
    var baud by mutableStateOf("230400")
    var address by mutableStateOf("1")
    var ports by mutableStateOf<List<String>>(emptyList());private set
    var connected by mutableStateOf(false);private set
    var capabilities by mutableStateOf<Capabilities?>(null);private set
    var busy by mutableStateOf(false);private set
    var status by mutableStateOf("选择已确认用途的 485 串口后连接");private set
    var path by mutableStateOf("/");private set
    var entries by mutableStateOf<List<Entry>>(emptyList());private set
    var selected by mutableStateOf<Entry?>(null)
    var progress by mutableStateOf<Progress?>(null);private set
    var terminal by mutableStateOf("");private set
    var pty by mutableStateOf<PtyConnection?>(null);private set
    var confirmation by mutableStateOf<Confirmation?>(null)
    var serialState by mutableStateOf(SerialState(SerialMode.CLOSED));private set
    val serialMonitor=SerialMonitor()
    var stabilityMs by mutableStateOf(500L);private set
    var stabilityError by mutableStateOf("");private set
    private val preferenceGuard=Any()
    private var preferenceSave: Job?=null
    private var serialSession: Session?=null
    private var remote: Device?=null
    private var work: Job?=null
    private val preferences=File(System.getProperty("hdtools.data",File(System.getProperty("user.home"),"Documents/tmp/hd-rs485-drive/HDTools").path),"connection.properties")
    init {
        if(!demo && preferences.isFile) runCatching {
            val p=Properties();preferences.inputStream().use {p.load(it)}
            port=p.getProperty("port","");baud=p.getProperty("baud","230400");address=p.getProperty("address","1")
            stabilityMs=p.getProperty("stabilityMs")?.toLongOrNull()?.takeIf {it in 100..5000} ?: 500L
            serialMonitor.setStability(stabilityMs)
        }
    }
    private fun savePreferences() = synchronized(preferenceGuard) {
        if(demo)return@synchronized
        preferences.parentFile.mkdirs()
        val p=Properties()
        p.setProperty("port",port.trim());p.setProperty("baud",baud);p.setProperty("address",address)
        p.setProperty("stabilityMs",stabilityMs.toString())
        preferences.outputStream().use {p.store(it,"HDTools connection")}
    }
    fun setStability(text: String) {
        val value=text.toLongOrNull()
        if(value==null || value !in 100..5000) {stabilityError="请输入 100～5000 ms";return}
        stabilityMs=value;serialMonitor.setStability(value);stabilityError=""
        preferenceSave=scope.launch {
            runCatching {withContext(Dispatchers.IO) {savePreferences()}}
                .onFailure {stabilityError="设置已应用，保存失败：${it.message}"}
        }
    }
    fun clearTraffic() {serialMonitor.clearTraffic()}
    private fun append(text: String) {terminal=(terminal+text).takeLast(256*1024)}
    fun clearTerminal() {terminal=""}
    private fun task(label: String,body: suspend ()->Unit) {
        if(busy)return
        serialSession?.resetReadCancellation()
        busy=true;status=label;progress=null;remote?.cancelled?.set(false)
        work=scope.launch {
            try {body();if(status==label)status="${label}完成"}
            catch(e:ConnectionLost) {
                withContext(Dispatchers.IO) {runCatching {remote?.close()}}
                remote=null;serialSession=null;connected=false;capabilities=null;entries=emptyList();selected=null
                status=e.message.orEmpty()
            }
            catch(e:ReadCancelled) {status=e.message.orEmpty()}
            catch(e:Stopped) {status=e.message.orEmpty()}
            catch(e:Exception) {status=e.message ?: e.javaClass.simpleName}
            finally {busy=false}
        }
    }
    fun openPty()=task("打开交互终端") {
        check(capabilities?.pty==true) {"板端未启用 PTY"}
        val wire=checkNotNull(serialSession)
        // Explicit new session on reopening: never attach to an abandoned shell.
        if(pty!=null)withContext(Dispatchers.IO) {wire.hello();Thread.sleep(700)}
        val connection=PtyConnection(wire) {status=it}
        pty=connection
        status="交互终端 · 等待应答期间按键排队，串口不写入"
        status=withContext(Dispatchers.IO) {connection.run()}
    }
    fun closePty() {pty?.close();status="正在关闭交互终端…"}
    fun showCommands() {if(!busy)pty=null}
    fun refreshPorts()=task("枚举串口") {
        ports=withContext(Dispatchers.IO) {SerialPort.getCommPorts().map {it.systemPortPath}.filterNot {it.startsWith("/dev/tty.")}.distinct().sorted()}
    }
    fun connect()=task("连接设备") {
        require(port.isNotBlank() || demo) {"请选择串口"}
        val rate=baud.toInt();val addr=address.toInt();require(addr in 1..65535)
        val device=withContext(Dispatchers.IO) {
            if(demo) DemoDevice() else {
                val io=SerialIO(port.trim(),rate)
                val token=serialMonitor.opened(io.traffic)
                serialState=SerialState(SerialMode.IDLE)
                val wire=Session(io,addr,baud=rate,onState={if(serialMonitor.update(token,it))serialState=it})
                serialSession=wire
                try {RemoteDevice(wire)} catch(e:Exception) {wire.close();throw e}
            }
        }
        remote=device;capabilities=device.capabilities;connected=true
        if(demo) {serialMonitor.opened(TrafficCounters());serialState=SerialState(SerialMode.IDLE)}
        if(!demo) withContext(Dispatchers.IO) {
            runCatching {savePreferences()}
        }
        append("已连接 H485 · ${if(demo) "演示设备" else port}\n")
        if(device.capabilities.files) load("/")
        status=if(device.capabilities.files) "已连接 · 文件根目录 ${device.capabilities.root}" else "终端可用；文件浏览需 hd485d 0.2.0 并启用 --fs-root"
    }
    private suspend fun load(destination: String) {
        val target=cleanPath(destination)
        val result=withContext(Dispatchers.IO) {remote!!.list(target)}
        path=target;entries=result;selected=null
    }
    fun navigate(destination: String)=task("读取目录") {load(destination);status="${entries.size} 项 · $path"}
    fun execute(command: String)=task("执行命令") {
        append("\n$ $command\n")
        val decoders=arrayOf(Utf8Chunks(),Utf8Chunks())
        val result=withContext(Dispatchers.IO) {
            remote!!.execute(command) {stream,bytes ->
                val text=decoders[stream].feed(bytes)
                // Dispatch UI mutations in order and wait before finishing the command.
                runBlocking {withContext(scope.coroutineContext.minusKey(Job)) {append(text)}}
            }
        }
        decoders.forEach {append(it.finish())}
        append("\n[退出码 ${result.exit}${if(result.truncated) " · 输出已被设备截断" else ""}]\n")
        status="命令结束 · 退出码 ${result.exit}"
    }
    private suspend fun transfer(action: ((Progress)->Unit)->Unit) {
        withContext(Dispatchers.IO) {action {p->runBlocking {withContext(scope.coroutineContext.minusKey(Job)) {progress=p}}}}
    }
    fun upload(local: File)=task("检查上传目标") {
        val directory=path;val target=remotePath(directory,local.name)
        val old=withContext(Dispatchers.IO) {remote!!.stat(target)}
        require(old==null || old.regular) {"目标不是普通文件"}
        val start={task("上传 ${local.name}") {
            val now=withContext(Dispatchers.IO) {remote!!.stat(target)}
            check(now==old) {"目标文件已变化，请刷新后重试"}
            transfer {remote!!.upload(local,target,old!=null,it)}
            load(directory);status="上传完成 · SHA-256 校验通过"
        }}
        if(old!=null)confirmation=Confirmation("覆盖设备文件？\n$target\n${old.size} 字节",start)
        else {val checking=work;scope.launch {checking?.join();start()}}
    }
    fun download(local: File) {
        val entry=selected ?: return
        val target=remotePath(path,entry.name)
        val replace=local.exists()
        val start={task("下载 ${entry.name}") {transfer {remote!!.download(target,local,replace,it)};status="下载完成 · SHA-256 校验通过：${local.path}"}}
        if(replace)confirmation=Confirmation("覆盖本地文件？校验通过后替换。\n${local.path}",start) else start()
    }
    fun export(local: File) {
        val snapshot=terminal
        val action={task("导出日志") {withContext(Dispatchers.IO) {local.writeText(snapshot,Charsets.UTF_8)}}}
        if(local.exists())confirmation=Confirmation("覆盖日志文件？\n${local.path}",action) else action()
    }
    fun confirm() {val action=confirmation?.action;confirmation=null;action?.invoke()}
    fun cancelRead() {
        if(serialSession?.cancelRead()==true) status="正在取消本次读取…"
    }
    fun cancel() {pty?.close();remote?.cancelled?.set(true);status="正在取消任务…"}
    suspend fun shutdown() {
        preferenceSave?.join()
        pty?.close();remote?.cancelled?.set(true);serialSession?.cancelRead();work?.join()
        withContext(Dispatchers.IO) {remote?.close()}
        remote=null;serialSession=null;connected=false;capabilities=null;entries=emptyList();selected=null;confirmation=null;pty=null
        serialMonitor.closed();serialState=SerialState(SerialMode.CLOSED)
        status="串口已关闭"
    }
    fun disconnect() {scope.launch {shutdown()}}
    suspend fun awaitIdle() {work?.join()}
}
internal class Utf8Chunks {
    private var pending=byteArrayOf()
    fun feed(bytes: ByteArray): String {
        pending+=bytes
        val decoder=Charsets.UTF_8.newDecoder().onMalformedInput(java.nio.charset.CodingErrorAction.REPLACE)
        val input=java.nio.ByteBuffer.wrap(pending)
        val output=java.nio.CharBuffer.allocate(pending.size+1)
        decoder.decode(input,output,false)
        pending=pending.copyOfRange(input.position(),pending.size)
        output.flip();return output.toString()
    }
    fun finish(): String {val text=String(pending,Charsets.UTF_8);pending=byteArrayOf();return text}
}
internal suspend fun chooseFile(save: Boolean,name: String=""): File? {
    val result=CompletableDeferred<File?>()
    SwingUtilities.invokeLater {
        try {
            val dialog=FileDialog(null as Frame?,if(save) "保存文件" else "选择上传文件",if(save)FileDialog.SAVE else FileDialog.LOAD)
            dialog.file=name;dialog.isVisible=true
            result.complete(dialog.file?.let {File(dialog.directory,it)});dialog.dispose()
        } catch(e:Exception) {result.completeExceptionally(e)}
    }
    return result.await()
}
internal class DemoDevice: Device {
    override val capabilities=Capabilities(true,true,"/",sessionCwd=true)
    override val cancelled=AtomicBoolean()
    private val data=mutableMapOf("/userdata/read me.txt" to "HDTools 485 设备文件演示\n".toByteArray(),"/userdata/.hidden" to byteArrayOf(1,2,3))
    override fun list(path: String): List<Entry> = if(path=="/") listOf(Entry("userdata",0x41ed,4096,1795046400),Entry("etc",0x41ed,4096,1795046400),Entry("dev",0x41ed,4096,1795046400)) else data.filterKeys {it.substringBeforeLast('/')==path}.map {Entry(it.key.substringAfterLast('/'),0x81a4,it.value.size.toLong(),1795046400)}
    override fun stat(path: String)=data[path]?.let {Entry(path.substringAfterLast('/'),0x81a4,it.size.toLong(),1795046400)}
    override fun execute(command: String,output: (Int,ByteArray)->Unit): CommandResult {output(0,"演示模式：$command\nhd485d 0.3.0\n".toByteArray());return CommandResult(0,false)}
    override fun upload(local: File,remote: String,overwrite: Boolean,progress: (Progress)->Unit) {data[remote]=local.readBytes();progress(Progress(local.length(),local.length(),10000))}
    override fun download(remote: String,local: File,overwrite: Boolean,progress: (Progress)->Unit) {local.writeBytes(data.getValue(remote));progress(Progress(local.length(),local.length(),10000))}
    override fun close() {}
}
