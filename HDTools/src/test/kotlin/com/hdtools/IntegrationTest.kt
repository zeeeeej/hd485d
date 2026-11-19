package com.hdtools

import java.io.File
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.file.Files
import java.util.concurrent.TimeUnit
import kotlin.test.*

class IntegrationTest {
    private class SocketIO(port: Int): WireIO {
        private val socket=Socket("127.0.0.1",port).apply {soTimeout=10}
        override fun write(bytes: ByteArray) {socket.getOutputStream().write(bytes);socket.getOutputStream().flush()}
        override fun read(): ByteArray = try {val b=ByteArray(4096);val n=socket.getInputStream().read(b);check(n>=0);b.copyOf(n)} catch(e:SocketTimeoutException) {byteArrayOf()}
        override fun close() {socket.close()}
    }
    private fun peer(test: (RemoteDevice, File)->Unit) {
        org.junit.Assume.assumeFalse(System.getProperty("os.name").startsWith("Windows"))
        val source=File(System.getenv("HD485_SOURCE"))
        val root=Files.createTempDirectory("hdt-").toFile()
        val process=ProcessBuilder("python3",File(source,"tools/hdtools_test_peer.py").path,root.path,File(source,"build/hd485d").path)
            .redirectError(File(root,"bridge.log")).start()
        var remote: RemoteDevice?=null
        try {
            val port=process.inputStream.bufferedReader().readLine()?.toInt() ?: error(File(root,"bridge.log").readText())
            remote=RemoteDevice(Session(SocketIO(port)))
            test(remote,root)
        } finally {
            remote?.close()
            if(!process.waitFor(10,TimeUnit.SECONDS))process.destroyForcibly()
        }
    }
    @Test fun actualDaemonFilesAndShell()=peer {d,root->
        assertTrue(d.capabilities.files)
        assertTrue(d.capabilities.sessionCwd)
        assertEquals(0,d.execute("cd '${root.path.replace("'", "'\"'\"'")}'") {_,_->}.exit)
        val cwd=StringBuilder()
        assertEquals(0,d.execute("pwd") {_,b->cwd.append(String(b))}.exit)
        assertEquals(root.path+"\n",cwd.toString())
        val name="中文 空格\n"+"x".repeat(220)
        File(root,name).writeText("hello")
        File(root,".hidden").writeText("")
        Files.createSymbolicLink(File(root,"link").toPath(),File("/etc").toPath())
        assertTrue(d.list("/").any {it.name==name})
        assertEquals("/etc",d.stat("/link")!!.link)
        assertFailsWith<DeviceError> {d.list("/link")}
        assertFailsWith<IllegalArgumentException> {d.list("/../etc")}
        val output=StringBuilder();val decoders=arrayOf(Utf8Chunks(),Utf8Chunks())
        val result=d.execute("printf '你好'; printf 'error' >&2; exit 7") {i,b->output.append(decoders[i].feed(b))}
        assertEquals(7,result.exit);assertTrue(output.contains("你好"));assertTrue(output.contains("error"))
        val local=File(root,"local.bin");val content=ByteArray(2457) {(it%256).toByte()};local.writeBytes(content)
        d.upload(local,"/uploaded.bin",false) {}
        assertContentEquals(content,File(root,"uploaded.bin").readBytes())
        val downloaded=File(root,"result.bin")
        d.download("/uploaded.bin",downloaded,false) {}
        assertContentEquals(content,downloaded.readBytes())
        assertFailsWith<IllegalArgumentException> {d.download("/uploaded.bin",downloaded,false) {}}
    }
    @Test fun cancellationAndExplicitResume()=peer {d,root->
        val local=File(root,"source.bin");local.writeBytes(ByteArray(1200) {it.toByte()})
        assertFailsWith<Stopped> {d.upload(local,"/resumed.bin",false) {if(it.bytes>=240)d.cancelled.set(true)}}
        assertFalse(File(root,"resumed.bin").exists())
        d.cancelled.set(false)
        var resumed=-1L
        d.upload(local,"/resumed.bin",false) {if(resumed<0)resumed=it.bytes}
        assertEquals(240,resumed)
        assertContentEquals(local.readBytes(),File(root,"resumed.bin").readBytes())
        val target=File(root,"download.bin")
        assertFailsWith<Stopped> {d.download("/resumed.bin",target,false) {if(it.bytes>=240)d.cancelled.set(true)}}
        assertFalse(target.exists());assertTrue(File(target.path+".hdtools.part").exists())
        d.cancelled.set(false);resumed=-1
        d.download("/resumed.bin",target,false) {if(resumed<0)resumed=it.bytes}
        assertEquals(240,resumed);assertContentEquals(local.readBytes(),target.readBytes())
        d.cancelled.set(true)
        val r=d.execute("sleep 20") {_,_->}
        assertTrue(r.exit!=0)
    }
    @Test fun zeroFileAndChangedDownload()=peer {d,root->
        val empty=File(root,"empty");empty.writeBytes(byteArrayOf())
        d.upload(empty,"/empty-upload",false) {}
        val output=File(root,"empty-download");d.download("/empty-upload",output,false) {}
        assertEquals(0,output.length())
        File(root,"source").writeBytes(ByteArray(1000))
        val target=File(root,"target")
        assertFailsWith<Stopped> {d.download("/source",target,false) {if(it.bytes>=240)d.cancelled.set(true)}}
        d.cancelled.set(false);File(root,"source").writeBytes(ByteArray(1000) {1})
        assertFailsWith<IllegalArgumentException> {d.download("/source",target,false) {}}
        assertFalse(target.exists())
    }
    @Test fun framingAndUtf8() {
        val packet=Packet(130,1,42,7,byteArrayOf(0,1,2))
        val raw=Wire.encode(packet);val parser=FrameParser()
        assertTrue(parser.feed(byteArrayOf(8,9)+raw.copyOfRange(0,15)).isEmpty())
        val received=parser.feed(raw.copyOfRange(15,raw.size)).single()
        assertEquals(packet.op,received.op);assertContentEquals(packet.body,received.body)
        val bad=raw.copyOf().also {it[25]=99}
        assertEquals(1,parser.feed(bad+raw).size)
        val decoder=Utf8Chunks();val chars="中文".toByteArray()
        assertEquals("",decoder.feed(chars.copyOfRange(0,2)))
        assertEquals("中文",decoder.feed(chars.copyOfRange(2,chars.size)))
    }
    @Test fun turnaroundAppliesAfterSuccessAndDeviceError() {
        var request: Packet?=null
        val timestamps=mutableListOf<Long>()
        val io=object: WireIO {
            override fun write(bytes: ByteArray) {
                timestamps+=System.nanoTime();request=FrameParser().feed(bytes).single()
            }
            override fun read(): ByteArray {
                val p=request ?: return byteArrayOf()
                request=null
                val body=if(p.op==1) buffer(12).putInt(0).putInt(99).putInt(240).array()
                    else buffer(4).putInt(13).array()
                return Wire.encode(Packet(p.op or 128,p.address,99,p.id,body))
            }
            override fun close() {}
        }
        val s=Session(io,turnaroundMs=60)
        s.hello()
        repeat(2) {assertFailsWith<DeviceError> {s.call(10)}}
        timestamps.zipWithNext().forEach {(a,b)->assertTrue(b-a>=60_000_000L)}
    }
    @Test fun retryDoesNotChangeRequest() {
        var expected: ByteArray?=null;var writes=0
        val io=object:WireIO {
            override fun write(bytes: ByteArray) {if(expected!=null)assertContentEquals(expected,bytes);expected=bytes;writes++}
            override fun read(): ByteArray {
                if(writes<2)return byteArrayOf()
                val p=FrameParser().feed(expected!!).single()
                return Wire.encode(Packet(p.op or 128,p.address,99,p.id,buffer(12).putInt(0).putInt(99).putInt(240).array()))
            }
            override fun close() {}
        }
        Session(io,timeoutMs=10).hello();assertEquals(2,writes)
    }
}
