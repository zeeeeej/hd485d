import org.jetbrains.compose.desktop.application.dsl.TargetFormat
plugins {
    kotlin("jvm") version "2.2.21"
    id("org.jetbrains.compose") version "1.9.3"
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.21"
}
version = "0.2.0"
kotlin { jvmToolchain(17) }
dependencies {
    implementation("org.jetbrains.jediterm:jediterm-ui:3.72")
    implementation("org.jetbrains.jediterm:jediterm-core:3.72")
    implementation(compose.desktop.currentOs)
    implementation(compose.material3)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
    implementation("com.fazecast:jSerialComm:2.11.2")
    testImplementation(kotlin("test"))
}
compose.desktop {
    application {
        mainClass = "com.hdtools.MainKt"
        nativeDistributions {
            targetFormats(TargetFormat.Dmg, TargetFormat.Exe)
            packageName = "HDTools"
            packageVersion = project.version.toString()
            modules("jdk.unsupported", "java.management")
            macOS { bundleID = "com.hdtools.desktop"; packageVersion = "1.2.0" }
            windows { menuGroup = "HDTools"; shortcut = true }
        }
    }
}
tasks.test {
    environment("HD485_SOURCE", rootDir.parentFile.absolutePath)
    systemProperty("java.io.tmpdir", File(System.getProperty("user.home"),"Documents/tmp/hd-rs485-drive/hdtools-tests").also { it.mkdirs() }.path)
}
tasks.withType<JavaExec>().configureEach {
    systemProperty("java.io.tmpdir", File(System.getProperty("user.home"),"Documents/tmp/hd-rs485-drive/hdtools-runtime").also { it.mkdirs() }.path)
}
