package com.hdtools

internal data class Traffic(val rx: Long=0, val tx: Long=0)

/** Counts completed driver I/O, independently of parsing and request success. */
internal class TrafficCounters {
    private var value=Traffic()
    @Synchronized fun received(count: Int) { if(count>0)value=value.copy(rx=value.rx+count) }
    @Synchronized fun sent(count: Int) { if(count>0)value=value.copy(tx=value.tx+count) }
    @Synchronized fun snapshot()=value
    @Synchronized fun clear() { value=Traffic() }
}

internal data class ModeDisplay(val state: SerialState, val switching: Boolean, val redLit: Boolean)

/** Every transition is recorded before Compose can coalesce UI updates. */
internal class SerialMonitor(private val clock: ()->Long=System::nanoTime) {
    private var generation=0L
    private var state=SerialState(SerialMode.CLOSED)
    private var changedAt=clock()
    private var animationAt=changedAt
    private var stabilityMs=500L
    private var counters=TrafficCounters()

    @Synchronized fun setStability(ms: Long) {
        require(ms in 100..5000)
        stabilityMs=ms
    }
    @Synchronized fun opened(traffic: TrafficCounters): Long {
        generation++
        counters=traffic
        state=SerialState(SerialMode.IDLE)
        changedAt=clock();animationAt=changedAt
        return generation
    }
    @Synchronized fun update(token: Long, next: SerialState): Boolean {
        if(token!=generation)return false
        val now=clock()
        if(next.mode!=state.mode) {
            if(!switching(now))animationAt=now
            changedAt=now
        }
        state=next
        return true
    }
    @Synchronized fun closed() {
        generation++ // Late callbacks from the old connection must not reopen the display.
        state=SerialState(SerialMode.CLOSED)
    }
    private fun switching(now: Long)=state.mode!=SerialMode.CLOSED && now-changedAt<stabilityMs*1_000_000L
    @Synchronized fun display(now: Long=clock()): ModeDisplay {
        return ModeDisplay(state,switching(now),(now-animationAt)/250_000_000L%2==0L)
    }
    @Synchronized fun traffic()=counters.snapshot()
    @Synchronized fun clearTraffic() { counters.clear() }
}
