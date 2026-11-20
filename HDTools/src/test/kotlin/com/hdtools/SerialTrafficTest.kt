package com.hdtools

import kotlin.test.*

class SerialTrafficTest {
    private class Driver: SerialDriver {
        override var isOpen=true
        var onWrite: (ByteArray,Int)->Int={b,o->b.size-o}
        var onRead: (ByteArray)->Int={0}
        override fun write(bytes: ByteArray,offset: Int)=onWrite(bytes,offset)
        override fun read(bytes: ByteArray)=onRead(bytes)
        override fun close() {isOpen=false}
    }
    @Test fun partialWriteFailureRetainsOnlyAcceptedBytes() {
        val driver=Driver();val io=SerialIO(driver,230400)
        val counts=ArrayDeque(listOf(0,2,3,-1));val offsets=mutableListOf<Int>()
        driver.onWrite={_,offset->offsets+=offset;counts.removeFirst()}
        assertFailsWith<IllegalStateException> {io.write(ByteArray(10))}
        assertEquals(listOf(0,0,2,5),offsets)
        assertEquals(Traffic(tx=5),io.traffic.snapshot())
        io.close();assertEquals(5,io.traffic.snapshot().tx)
    }
    @Test fun fragmentedAndInvalidReadsCountBeforeParsingOrDisconnect() {
        val driver=Driver();val io=SerialIO(driver,230400)
        val parts=ArrayDeque(listOf(byteArrayOf(),byteArrayOf(1,2),byteArrayOf(9,8,7)))
        driver.onRead={b->val data=parts.removeFirst();data.copyInto(b);data.size}
        repeat(3) {io.read()}
        assertEquals(5,io.traffic.snapshot().rx)
        driver.onRead={b->b[0]=1;driver.isOpen=false;1}
        assertFailsWith<IllegalStateException> {io.read()}
        assertEquals(6,io.traffic.snapshot().rx)
        driver.onRead={-1}
        assertFailsWith<IllegalStateException> {io.read()}
        assertEquals(6,io.traffic.snapshot().rx)
    }
    @Test fun retryCountsFullFramesAndAllResponseBytes() {
        val driver=Driver();val io=SerialIO(driver,230400)
        var writes=0;var request=byteArrayOf();var reply=byteArrayOf();var sentReply=false
        driver.onWrite={b,offset->
            assertEquals(0,offset);writes++;request=b
            val p=FrameParser().feed(b).single()
            val good=Wire.encode(Packet(p.op or 128,p.address,99,p.id,buffer(12).putInt(0).putInt(99).putInt(240).array()))
            val bad=good.copyOf().also {it[24]=1}
            reply=byteArrayOf(1,2,3)+bad+good
            b.size
        }
        driver.onRead={b->
            if(writes<2 || sentReply)0 else {sentReply=true;reply.copyInto(b);reply.size}
        }
        val session=Session(io,timeoutMs=10,turnaroundMs=0)
        session.hello()
        assertEquals(2,writes)
        assertEquals(Traffic(reply.size.toLong(),request.size.toLong()*2),io.traffic.snapshot())
        session.close()
    }
}
