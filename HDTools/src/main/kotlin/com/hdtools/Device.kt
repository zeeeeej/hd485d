package com.hdtools

import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.StandardCopyOption.*
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean

internal data class Entry(val name: String,val mode: Int,val size: Long,val modified: Long,val link: String="") {
    val directory get()=(mode and 0xf000)==0x4000
    val regular get()=(mode and 0xf000)==0x8000
    val type get()=when(mode and 0xf000) {0x4000->"目录";0x8000->"文件";0xa000->"链接";else->"特殊"}
    val permissions get()=(mode and 4095).toString(8)
}
internal data class Capabilities(val files: Boolean,val shell: Boolean,val root: String,val maxFile: Long=64*1024*1024,val sessionCwd: Boolean=false)
internal data class Progress(val bytes: Long,val total: Long,val speed: Long)
internal data class CommandResult(val exit: Int,val truncated: Boolean)
internal class Stopped: Exception("已取消；文件断点保留，可重新选择同一文件恢复")
internal fun remotePath(dir: String,name: String)=if(dir=="/") "/$name" else "$dir/$name"
internal fun cleanPath(path: String): String {
    require(path.startsWith('/') && '\u0000' !in path) {"请输入绝对路径"}
    val parts=path.split('/').filter {it.isNotEmpty()}
    require(parts.none {it=="." || it==".." || it.startsWith(".hd485-")}) {"路径包含不支持的分量"}
    return "/"+parts.joinToString("/")
}
internal interface Device: AutoCloseable {
    val capabilities: Capabilities
    val cancelled: AtomicBoolean
    fun list(path: String): List<Entry>
    fun stat(path: String): Entry?
    fun execute(command: String, output: (Int,ByteArray)->Unit): CommandResult
    fun upload(local: File,remote: String,overwrite: Boolean, progress: (Progress)->Unit)
    fun download(remote: String,local: File,overwrite: Boolean,progress: (Progress)->Unit)
}
internal class RemoteDevice(private val wire: Session): Device {
    override val cancelled=AtomicBoolean()
    override val capabilities: Capabilities
    init {
        wire.hello()
        capabilities=try {
            val b=wire.call(32).le()
            require(b.short.toInt()==1)
            val flags=b.short.toInt(); val max=b.long; require(b.short.toInt()==240)
            val size=b.short.toInt() and 65535;require(b.remaining()==size)
            val root=ByteArray(size);b.get(root)
            Capabilities(flags and 1!=0,flags and 2!=0,String(root,Charsets.UTF_8),max,flags and 4!=0)
        } catch(e:DeviceError) {
            if(e.errno !in listOf(95,45)) throw e
            Capabilities(false,true,"")
        }
    }
    private fun check() {if(cancelled.get())throw Stopped()}
    private fun bytes(path: String)=cleanPath(path).toByteArray(Charsets.UTF_8).also {require(it.size<4096) {"路径太长"}}
    private fun decode(b: ByteArray)=Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).decode(java.nio.ByteBuffer.wrap(b)).toString()
    private fun entries(op: Int,path: String): List<Entry> {
        wire.call(op,bytes(path))
        try {
            var offset=0L;var pending=byteArrayOf(); val entries=mutableListOf<Entry>()
            while(true) {
                val block=wire.ready(34,buffer(8).putLong(offset).array(),::check)
                require(block.size>=9)
                val b=block.le();val eof=b.get().toInt();require(b.long==offset)
                val content=block.copyOfRange(9,block.size);offset+=content.size;pending+=content
                while(pending.size>=4) {
                    val size=pending.le().int
                    require(size in 24..8188) {"目录记录长度无效"}
                    if(pending.size<4+size)break
                    val r=pending.le();r.int
                    val mode=r.int;val length=r.long;val modified=r.long
                    val names=r.short.toInt() and 65535;val links=r.short.toInt() and 65535
                    require(size==24+names+links)
                    val name=ByteArray(names).also {r.get(it)};val link=ByteArray(links).also {r.get(it)}
                    entries+=Entry(decode(name),mode,length,modified,decode(link))
                    pending=pending.copyOfRange(4+size,pending.size)
                }
                if(eof==1) {require(pending.isEmpty());break}
            }
            return entries.sortedWith(compareByDescending<Entry>{it.directory}.thenBy {it.name.lowercase()})
        } finally {runCatching {wire.call(35)}}
    }
    override fun list(path: String)=entries(33,path)
    override fun stat(path: String): Entry?=try {entries(36,path).single()} catch(e:DeviceError) {if(e.errno==2)null else throw e}
    override fun execute(command: String,output: (Int,ByteArray)->Unit): CommandResult {
        val commandBytes=command.toByteArray(Charsets.UTF_8)
        require(commandBytes.size in 1..4096 && !command.contains('\u0000')) {"命令应为 1～4096 字节"}
        wire.call(2,commandBytes)
        val offsets=intArrayOf(0,0);var cut=false;var interrupted=false
        var exit=-1
        while(true) {
            if(cancelled.get() && !interrupted) {wire.call(4);interrupted=true}
            var done=0
            for(stream in 0..1) {
                val data=wire.call(3,buffer(5).put(stream.toByte()).putInt(offsets[stream]).array())
                require(data.size>=11)
                val b=data.le();val state=b.get().toInt();val truncated=b.get().toInt()!=0;cut=cut || truncated
                exit=b.int;val total=b.int;val eof=b.get().toInt()
                val chunk=data.copyOfRange(11,data.size);offsets[stream]+=chunk.size
                if(chunk.isNotEmpty())output(stream,chunk)
                if(state!=1 && eof==1 && offsets[stream]>=total)done++
            }
            if(done==2) return CommandResult(exit,cut)
            Thread.sleep(20)
        }
    }
    private fun digest(file: File): ByteArray {
        val hash=MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input -> val b=ByteArray(65536);while(true) {check();val n=input.read(b);if(n<0)break;hash.update(b,0,n)} }
        return hash.digest()
    }
    private fun report(bytes: Long,total: Long,start: Long,progress: (Progress)->Unit,base: Long) = progress(Progress(bytes,total,((bytes-base)*1e9/(System.nanoTime()-start).coerceAtLeast(1)).toLong()))
    override fun upload(local: File,remote: String,overwrite: Boolean,progress: (Progress)->Unit) {
        require(Files.isRegularFile(local.toPath(),NOFOLLOW_LINKS)) {"只能上传普通文件"}
        val before=Files.readAttributes(local.toPath(),BasicFileAttributes::class.java,NOFOLLOW_LINKS)
        val size=before.size();require(size<=capabilities.maxFile) {"文件超过 64 MiB"}
        val hash=digest(local);val path=bytes(remote)
        val begin=buffer(41+path.size).putLong(size).put(hash).put(if(overwrite)1 else 0).put(path).array()
        try {
            var off=wire.call(37,begin).le().long;require(off in 0..size)
            val base=off;val start=System.nanoTime();report(off,size,start,progress,base)
            RandomAccessFile(local,"r").use { f ->
                f.seek(off)
                while(off<size) {
                    check();val n=minOf(240L,size-off).toInt();val data=ByteArray(n);f.readFully(data)
                    val next=wire.call(6,buffer(8+n).putLong(off).put(data).array()).le().long
                    require(next==off+n);off=next;report(off,size,start,progress,base)
                }
            }
            val after=Files.readAttributes(local.toPath(),BasicFileAttributes::class.java,NOFOLLOW_LINKS)
            check(after.size()==size && after.lastModifiedTime()==before.lastModifiedTime() && after.fileKey()==before.fileKey() && digest(local).contentEquals(hash)) {"本地源文件已变化，未提交"}
            wire.ready(7,check=::check)
        } finally {runCatching {wire.call(39)}}
    }
    override fun download(remote: String,local: File,overwrite: Boolean,progress: (Progress)->Unit) {
        val path=local.toPath().toAbsolutePath()
        require(!Files.isSymbolicLink(path)) {"不能覆盖符号链接"}
        val before=if(Files.exists(path,NOFOLLOW_LINKS)) Files.readAttributes(path,BasicFileAttributes::class.java,NOFOLLOW_LINKS) else null
        require(before==null || (overwrite && before.isRegularFile)) {"目标已存在，请确认覆盖"}
        val part=File(local.absolutePath+".hdtools.part")
        val meta=File(local.absolutePath+".hdtools.meta")
        require(!Files.isSymbolicLink(part.toPath()) && !Files.isSymbolicLink(meta.toPath())) {"断点文件不能是符号链接"}
        try {
            val response=wire.ready(38,bytes(remote),::check);require(response.size==40)
            val size=response.le().long;require(size in 0..capabilities.maxFile)
            val hash=response.copyOfRange(8,40)
            val identity=cleanPath(remote).toByteArray()+byteArrayOf(0)+response
            if(part.exists() || meta.exists()) {
                require(part.isFile && meta.isFile && meta.readBytes().contentEquals(identity) && part.length()<=size) {"本地断点与设备文件不匹配，请另选保存位置或移走 .hdtools.part/.meta"}
            } else {
                Files.write(meta.toPath(),identity,java.nio.file.StandardOpenOption.CREATE_NEW)
                Files.createFile(part.toPath())
            }
            var off=part.length();val base=off;val start=System.nanoTime();report(off,size,start,progress,base)
            RandomAccessFile(part,"rw").use { f ->
                f.seek(off)
                while(off<size) {
                    check();val data=wire.call(9,buffer(8).putLong(off).array())
                    require(data.size>8 && data.le().long==off && data.size-8<=size-off)
                    f.write(data,8,data.size-8);off+=data.size-8;report(off,size,start,progress,base)
                }
                f.fd.sync()
            }
            check(digest(part).contentEquals(hash)) {"SHA-256 不匹配；断点保留，请移走后重新下载"}
            wire.call(9,buffer(8).putLong(size).array()) // final source identity check, including resumed complete file
            val current=if(Files.exists(path,NOFOLLOW_LINKS)) Files.readAttributes(path,BasicFileAttributes::class.java,NOFOLLOW_LINKS) else null
            check((before==null && current==null) || (before!=null && current!=null && current.isRegularFile && current.fileKey()==before.fileKey() && current.size()==before.size() && current.lastModifiedTime()==before.lastModifiedTime())) {"本地目标已变化，未覆盖"}
            check()
            if(overwrite) Files.move(part.toPath(),path,ATOMIC_MOVE,REPLACE_EXISTING)
            else Files.move(part.toPath(),path)
            Files.delete(meta.toPath())
        } finally {runCatching {wire.call(39)}}
    }
    override fun close() {wire.close()}
}
