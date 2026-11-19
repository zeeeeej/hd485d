package com.hdtools

import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.ExecutionException
import kotlin.test.*

class SerialModeTest {
    private fun reply(p: Packet,error: Int=0,body: ByteArray=byteArrayOf()) =
        Wire.encode(Packet(p.op or 128,p.address,p.session,p.id,buffer(4+body.size).putInt(error).put(body).array()))

    @Test fun fragmentedAndUnrelatedFramesKeepReadModeUntilCompleteReply() {
        lateinit var session: Session
        var request: Packet?=null;var reads=0
        val states=mutableListOf<SerialMode>()
        val io=object: WireIO {
            override fun write(bytes: ByteArray) {
                assertEquals(SerialMode.WRITING,session.state.mode)
                request=FrameParser().feed(bytes).single()
            }
            override fun read(): ByteArray {
                assertEquals(SerialMode.READING,session.state.mode)
                val p=request!!;val response=reply(p,body=byteArrayOf(42))
                return if(reads++==0) reply(p.copy(id=p.id+1))+response.copyOfRange(0,15)
                    else response.copyOfRange(15,response.size)
            }
            override fun close() {}
        }
        session=Session(io,onState={states+=it.mode})
        assertContentEquals(byteArrayOf(42),session.call(10))
        assertEquals(2,reads)
        assertEquals(listOf(SerialMode.WRITING,SerialMode.READING,SerialMode.IDLE),states)
        assertFalse(session.cancelRead())
    }

    @Test fun timeoutLeavesReadModeBeforeEveryRetry() {
        lateinit var session: Session
        var writes=0
        val states=mutableListOf<SerialMode>()
        val io=object: WireIO {
            override fun write(bytes: ByteArray) {assertEquals(SerialMode.WRITING,session.state.mode);writes++}
            override fun read()=byteArrayOf()
            override fun close() {}
        }
        session=Session(io,timeoutMs=10,turnaroundMs=0,onState={states+=it.mode})
        assertFailsWith<ConnectionLost> {session.call(10)}
        assertEquals(3,writes)
        assertEquals(List(3) {listOf(SerialMode.WRITING,SerialMode.READING,SerialMode.IDLE)}.flatten(),states)
        assertEquals(SerialMode.IDLE,session.state.mode)
    }

    @Test fun manualCancelStopsRetryCleanupAndRejectsConcurrentWrites() {
        val reading=CountDownLatch(1)
        val executor=Executors.newSingleThreadExecutor()
        val written=mutableListOf<Packet>()
        var replyEnabled=false
        val io=object: WireIO {
            override fun write(bytes: ByteArray) {written+=FrameParser().feed(bytes).single()}
            override fun read()=if(replyEnabled) reply(written.last()) else byteArrayOf()
            override fun close() {}
        }
        val session=Session(io,timeoutMs=10000,onState={if(it.mode==SerialMode.READING)reading.countDown()})
        try {
            val waiting=executor.submit<ByteArray> {session.call(10)}
            assertTrue(reading.await(1,TimeUnit.SECONDS))
            assertFailsWith<SerialBusy> {session.call(2,"ls".toByteArray())}
            assertTrue(session.cancelRead())
            assertIs<ReadCancelled>(assertFailsWith<ExecutionException> {waiting.get(1,TimeUnit.SECONDS)}.cause)
            assertEquals(SerialMode.IDLE,session.state.mode)
            assertEquals(1,written.size)
            assertFailsWith<ReadCancelled> {session.call(39)}
            assertEquals(1,written.size)
            session.resetReadCancellation();replyEnabled=true
            assertContentEquals(byteArrayOf(),session.call(10))
            assertEquals(2,written.size)
            assertNotEquals(written.first().id,written.last().id)
        } finally {session.close();executor.shutdownNow()}
    }

    @Test fun errorAndCloseCannotLeaveStuckReadMode() {
        lateinit var session: Session
        var request: Packet?=null
        val io=object: WireIO {
            override fun write(bytes: ByteArray) {request=FrameParser().feed(bytes).single()}
            override fun read()=reply(request!!,13)
            override fun close() {}
        }
        session=Session(io)
        assertFailsWith<DeviceError> {session.call(10)}
        assertEquals(SerialMode.IDLE,session.state.mode)
        session.close()
        assertEquals(SerialMode.CLOSED,session.state.mode)
        assertFailsWith<ConnectionLost> {session.call(10)}
        assertEquals(SerialMode.CLOSED,session.state.mode)
    }

    @Test fun cancellingDirectoryReadDoesNotSendAutomaticClose() {
        val reading=CountDownLatch(1)
        val executor=Executors.newSingleThreadExecutor()
        val operations=mutableListOf<Int>()
        var request: Packet?=null
        val io=object: WireIO {
            override fun write(bytes: ByteArray) {request=FrameParser().feed(bytes).single();operations+=request!!.op}
            override fun read(): ByteArray {
                val p=request!!
                return when(p.op) {
                    1 -> reply(p.copy(session=12),body=buffer(8).putInt(12).putInt(240).array())
                    32 -> reply(p,body=buffer(16).putShort(1).putShort(3).putLong(64*1024*1024L).putShort(240).putShort(0).array())
                    34 -> {reading.countDown();byteArrayOf()}
                    else -> reply(p)
                }
            }
            override fun close() {}
        }
        val session=Session(io,timeoutMs=10000,turnaroundMs=0)
        try {
            val device=RemoteDevice(session)
            val work=executor.submit<List<Entry>> {device.list("/")}
            assertTrue(reading.await(1,TimeUnit.SECONDS))
            assertTrue(session.cancelRead())
            assertIs<ReadCancelled>(assertFailsWith<ExecutionException> {work.get(1,TimeUnit.SECONDS)}.cause)
            assertEquals(listOf(1,32,33,34),operations)
            assertEquals(SerialMode.IDLE,session.state.mode)
        } finally {session.close();executor.shutdownNow()}
    }
}
