package com.hdtools

import java.util.concurrent.CountDownLatch
import kotlin.concurrent.thread
import kotlin.test.*

class SerialMonitorTest {
    @Test fun rapidTransitionsWaitForLastModeAndKeepAnimationCadence() {
        var now=0L
        val m=SerialMonitor {now};val token=m.opened(TrafficCounters())
        fun at(ms: Long,mode: SerialMode) {now=ms*1_000_000;m.update(token,SerialState(mode))}
        assertTrue(m.display().redLit)
        at(100,SerialMode.WRITING);at(200,SerialMode.READING);at(300,SerialMode.IDLE)
        assertTrue(m.display().switching)
        assertFalse(m.display().redLit) // Transitions must not restart the blink cycle.
        now=799_000_000;assertTrue(m.display().switching)
        now=800_000_000;assertFalse(m.display().switching)
        assertEquals(SerialMode.IDLE,m.display().state.mode)
        at(900,SerialMode.READING)
        assertTrue(m.display().switching);assertTrue(m.display().redLit)
    }
    @Test fun readingMetadataAndTrafficDoNotResetStability() {
        var now=0L;val counters=TrafficCounters();val m=SerialMonitor {now}
        val token=m.opened(counters);m.update(token,SerialState(SerialMode.READING))
        now=499_000_000
        m.update(token,SerialState(SerialMode.READING,deadlineNanos=8_000_000_000,attempt=2))
        counters.received(500)
        now=500_000_000
        assertFalse(m.display().switching);assertEquals(2,m.display().state.attempt)
        m.update(token,m.display().state.copy(cancelling=true))
        assertTrue(m.display().state.cancelling);assertFalse(m.display().switching)
        m.update(token,SerialState(SerialMode.CLOSED));assertFalse(m.display().switching)
    }
    @Test fun newConnectionResetsCountersAndRejectsOldCallbacks() {
        val m=SerialMonitor();val old=TrafficCounters();val first=m.opened(old)
        old.received(12);old.sent(8);m.closed()
        assertEquals(Traffic(12,8),m.traffic())
        assertFalse(m.update(first,SerialState(SerialMode.READING)))
        assertEquals(SerialMode.CLOSED,m.display().state.mode)
        val next=TrafficCounters();val second=m.opened(next)
        assertEquals(Traffic(),m.traffic())
        old.received(100);assertFalse(m.update(first,SerialState(SerialMode.CLOSED)))
        next.sent(32) // Handshake failure must retain its actual I/O.
        m.update(second,SerialState(SerialMode.CLOSED))
        assertEquals(Traffic(0,32),m.traffic())
        m.clearTraffic();assertEquals(Traffic(),m.traffic())
    }
    @Test fun configurableThresholdIsValidated() {
        var now=0L;val m=SerialMonitor {now};m.opened(TrafficCounters())
        m.setStability(1000);now=999_000_000;assertTrue(m.display().switching)
        now=1_000_000_000;assertFalse(m.display().switching)
        assertFailsWith<IllegalArgumentException> {m.setStability(99)}
        assertFailsWith<IllegalArgumentException> {m.setStability(5001)}
    }
    @Test fun countersAre64BitAndClearIsAtomicAgainstIO() {
        val c=TrafficCounters()
        repeat(3) {c.received(Int.MAX_VALUE);c.sent(Int.MAX_VALUE)}
        assertEquals(Traffic(6442450941L,6442450941L),c.snapshot())
        val start=CountDownLatch(1)
        val workers=List(4) {thread {start.await();repeat(10000) {c.received(1);c.sent(1)}}}
        c.clear();start.countDown();workers.forEach {it.join()}
        assertEquals(Traffic(40000,40000),c.snapshot())
        val reset=thread {repeat(1000) {c.clear()}}
        val add=thread {repeat(1000) {c.received(1);c.sent(1)}}
        reset.join();add.join();c.clear();c.received(7);c.sent(9)
        assertEquals(Traffic(7,9),c.snapshot())
    }
}
