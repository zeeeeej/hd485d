package com.hdtools

import java.nio.file.Files
import java.io.File
import java.util.Properties
import kotlinx.coroutines.*
import kotlin.test.*

class SerialPreferencesTest {
    @Test fun applyPersistsSettingsAndInvalidInputKeepsLastValue()=runBlocking {
        val folder=Files.createTempDirectory("hdtools-preferences-").toFile()
        val previous=System.getProperty("hdtools.data")
        System.setProperty("hdtools.data",folder.path)
        val scope=CoroutineScope(SupervisorJob()+Dispatchers.Unconfined)
        try {
            val c=Controller(scope)
            assertEquals(500,c.stabilityMs)
            c.port="test-only";c.baud="115200";c.address="7"
            c.setStability("750");c.shutdown() // Waits for the asynchronous save.
            val restored=Controller(scope)
            assertEquals(750,restored.stabilityMs)
            assertEquals("test-only",restored.port);assertEquals("115200",restored.baud);assertEquals("7",restored.address)
            for(invalid in listOf("", "abc", "99", "5001", "999999999999999999999999")) {
                restored.setStability(invalid)
                assertEquals(750,restored.stabilityMs);assertTrue(restored.stabilityError.isNotBlank())
            }
            assertEquals(750,Controller(scope).stabilityMs)
            val prefs=File(folder,"connection.properties")
            val p=Properties();prefs.inputStream().use {p.load(it)}
            p.setProperty("stabilityMs","bad");prefs.outputStream().use {p.store(it,null)}
            assertEquals(500,Controller(scope).stabilityMs)
        } finally {
            scope.cancel()
            if(previous==null)System.clearProperty("hdtools.data") else System.setProperty("hdtools.data",previous)
        }
    }
}
