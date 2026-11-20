package com.hdtools

import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.ImageComposeScene
import androidx.compose.ui.unit.Density
import java.io.ByteArrayInputStream
import javax.imageio.ImageIO
import kotlinx.coroutines.*
import kotlin.test.*

class SerialStatusUiTest {
    @OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)
    @Test fun switchingIndicatorDisappearsWithoutFurtherStateOrTrafficEvents()=runBlocking {
        val scope=CoroutineScope(coroutineContext+SupervisorJob())
        val c=Controller(scope,true)
        c.setStability("1000")
        val scene=ImageComposeScene(width=1280,height=100,density=Density(1f),coroutineContext=coroutineContext) {
            MaterialTheme {SerialModeRow(c)}
        }
        fun redPixels(): Int {
            val bytes=scene.render().encodeToData()!!.bytes
            val img=ImageIO.read(ByteArrayInputStream(bytes))
            var count=0
            for(y in 0 until img.height)for(x in 0 until img.width) {
                val pixel=img.getRGB(x,y)
                val r=(pixel ushr 16) and 255;val g=(pixel ushr 8) and 255;val b=pixel and 255
                if(r>150 && g<140 && b<140)count++
            }
            return count
        }
        try {
            // Warm up rendering while disconnected, before starting the stability clock.
            scene.render();delay(60);scene.render()
            c.connect();c.awaitIdle()
            var seen=false
            for(i in 0 until 12) {
                delay(25)
                if(redPixels()>4) {seen=true;break}
            }
            assertTrue(seen,"Switching must display the red indicator")
            // No further state callbacks or counter changes: only the UI clock can hide it.
            delay(1200)
            repeat(3) {scene.render();delay(50)}
            assertEquals(0,redPixels(),"Stable idle must remove the switching indicator")
            assertFalse(c.serialMonitor.display().switching)
            assertEquals(SerialMode.IDLE,c.serialState.mode)
        } finally {scene.close();c.shutdown();scope.cancel()}
    }
}
