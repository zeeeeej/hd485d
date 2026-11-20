package com.hdtools

import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.ImageComposeScene
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.*
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.*
import androidx.compose.ui.window.*
import kotlinx.coroutines.*
import java.io.File
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

fun main(args: Array<String>) {
    if("--screenshot" in args) {
        val output=args.getOrNull(args.indexOf("--screenshot")+1) ?: error("缺少截图路径")
        preview(output);return
    }
    application {
        val scope=rememberCoroutineScope()
        val c=remember {Controller(scope,"--demo" in args)}
        var closeRequested by remember {mutableStateOf(false)}
        var closing by remember {mutableStateOf(false)}
        fun close() {if(!closing) {closing=true;scope.launch {c.shutdown();exitApplication()}}}
        Window(onCloseRequest={if(c.busy)closeRequested=true else close()},title="HDTools · 485 设备文件",state=rememberWindowState(width=1280.dp,height=800.dp)) {
            MaterialTheme(colorScheme=lightColorScheme(primary=Color(0xff245a93),background=Color(0xfff4f6f8))) {
                Page(c)
                if(closeRequested) AlertDialog(onDismissRequest={closeRequested=false},title={Text("取消任务并关闭？")},text={Text("文件断点会保留，重新连接后可显式恢复。结果未知的命令不会自动重试。")},confirmButton={TextButton(onClick={closeRequested=false;close()}) {Text("关闭")}},dismissButton={TextButton(onClick={closeRequested=false}) {Text("返回")}})
            }
        }
    }
}
@OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)
private fun preview(output: String)=runBlocking {
    val scope=CoroutineScope(coroutineContext+SupervisorJob())
    val c=Controller(scope,true);c.connect();c.awaitIdle();c.navigate("/userdata");c.awaitIdle();c.execute("@version");c.awaitIdle()
    val scene=ImageComposeScene(width=1280,height=800,density=Density(1f),coroutineContext=coroutineContext) {
        MaterialTheme(colorScheme=lightColorScheme(primary=Color(0xff245a93),background=Color(0xfff4f6f8))) {Page(c)}
    }
    try {repeat(8) {scene.render();delay(30)};File(output).writeBytes(scene.render().encodeToData()!!.bytes)}
    finally {scene.close();c.shutdown();scope.cancel()}
}
@Composable internal fun SmallButton(text: String,enabled: Boolean=true,onClick: ()->Unit) {
    OutlinedButton(onClick,enabled=enabled,modifier=Modifier.height(29.dp),shape=RoundedCornerShape(5.dp),contentPadding=PaddingValues(horizontal=9.dp,vertical=0.dp)) {Text(text,fontSize=12.sp,maxLines=1)}
}
@Composable internal fun Field(label: String,value: String,change: (String)->Unit,modifier: Modifier=Modifier,enabled: Boolean=true) {
    Row(modifier.height(32.dp).background(Color.White,RoundedCornerShape(5.dp)).border(1.dp,Color(0xffcbd3dd),RoundedCornerShape(5.dp)).padding(horizontal=8.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
        Text(label,fontSize=11.sp,color=Color(0xff697586))
        BasicTextField(value,change,Modifier.weight(1f),readOnly=!enabled,singleLine=true,textStyle=TextStyle(fontSize=12.sp,color=if(enabled)Color(0xff17212f) else Color.Gray))
    }
}
@Composable internal fun Panel(modifier: Modifier=Modifier,content: @Composable ColumnScope.()->Unit) {
    Column(modifier.background(Color.White,RoundedCornerShape(6.dp)).border(1.dp,Color(0xffced5df),RoundedCornerShape(6.dp)).padding(7.dp),verticalArrangement=Arrangement.spacedBy(5.dp),content=content)
}
@OptIn(ExperimentalFoundationApi::class)
@Composable internal fun Page(c: Controller) {
    val scope=rememberCoroutineScope()
    var destination by remember(c.path) {mutableStateOf(c.path)}
    var filter by remember {mutableStateOf("")}
    var terminalOpen by remember {mutableStateOf(true)}
    var input by remember {mutableStateOf("")}
    var portsOpen by remember {mutableStateOf(false)}
    var choosing by remember {mutableStateOf(false)}
    var pickerError by remember {mutableStateOf("")}
    val idle=!c.busy && c.serialState.mode !in listOf(SerialMode.READING,SerialMode.WRITING,SerialMode.TURNAROUND) && !choosing && c.confirmation==null
    val files=idle && c.connected && c.capabilities?.files==true
    fun choose(save: Boolean,name: String="",action: (File)->Unit) {
        choosing=true;pickerError=""
        scope.launch {try {chooseFile(save,name)?.let(action)} catch(e:Exception) {pickerError=e.message.orEmpty()} finally {choosing=false}}
    }
    ProvideTextStyle(TextStyle(fontSize=12.sp,lineHeight=17.sp)) {
        Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).padding(10.dp),verticalArrangement=Arrangement.spacedBy(6.dp)) {
            Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(12.dp)) {
                Text("HDTools",fontSize=20.sp,color=MaterialTheme.colorScheme.primary)
                Text("485 设备文件与命令终端",fontSize=13.sp)
                Spacer(Modifier.weight(1f))
                Text(if(c.demo) "演示模式" else "v0.2.2 · H485",color=Color(0xff697586))
                Text(if(c.connected) "● 已连接" else "○ 未连接",color=if(c.connected)Color(0xff25805e) else Color.Gray)
            }
            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(7.dp)) {
                Field("485 串口",c.port,{c.port=it},Modifier.weight(1f),idle&&!c.connected)
                Box {
                    SmallButton("选择",idle&&!c.connected) {c.refreshPorts();portsOpen=true}
                    DropdownMenu(portsOpen,{portsOpen=false}) {
                        if(c.ports.isEmpty())DropdownMenuItem(text={Text("未发现串口，可手动输入")},onClick={portsOpen=false})
                        c.ports.forEach {DropdownMenuItem(text={Text(it)},onClick={c.port=it;portsOpen=false})}
                    }
                }
                Field("波特率",c.baud,{c.baud=it},Modifier.width(144.dp),idle&&!c.connected)
                Field("地址",c.address,{c.address=it},Modifier.width(90.dp),idle&&!c.connected)
                SmallButton(if(c.connected) "断开" else "连接",idle) {if(c.connected)c.disconnect() else c.connect()}
            }
            SerialModeRow(c)
            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(7.dp)) {
                Field("设备路径",destination,{destination=it},Modifier.weight(1f),files)
                SmallButton("前往",files) {c.navigate(destination)}
                SmallButton("上级",files&&c.path!="/") {c.navigate(c.path.substringBeforeLast('/').ifEmpty {"/"})}
                SmallButton("刷新",files) {c.navigate(c.path)}
                SmallButton("上传文件",files) {choose(false,action=c::upload)}
                SmallButton("下载文件",files&&c.selected?.regular==true) {choose(true,c.selected!!.name,c::download)}
            }
            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                Field("筛选",filter,{filter=it},Modifier.width(250.dp))
                Text("${c.entries.size} 项 · 包含隐藏文件",color=Color(0xff697586))
                Spacer(Modifier.weight(1f))
                Text("文件根目录 ${c.capabilities?.root ?: "—"} · SHA-256 · 最大 64 MiB",fontSize=11.sp,color=Color(0xff697586))
            }
            Panel(Modifier.weight(1f).fillMaxWidth()) {
                Row(Modifier.fillMaxWidth().padding(horizontal=6.dp),horizontalArrangement=Arrangement.spacedBy(9.dp)) {
                    Text("名称 / 链接目标",Modifier.weight(1f));Text("类型",Modifier.width(52.dp));Text("大小",Modifier.width(110.dp));Text("权限",Modifier.width(54.dp));Text("修改时间",Modifier.width(155.dp))
                }
                HorizontalDivider(color=Color(0xffe1e6ed))
                LazyColumn(Modifier.fillMaxSize()) {
                    if(c.entries.isEmpty())item {Text(if(c.connected&&c.capabilities?.files==true) "目录为空，或尚未读取成功" else "连接 485 终端服务后浏览设备文件",Modifier.padding(12.dp),color=Color.Gray)}
                    items(c.entries.filter {it.name.contains(filter,true)},key={it.name}) {e ->
                        Row(Modifier.fillMaxWidth().background(if(c.selected==e)Color(0xffe8f0fb) else Color.Transparent).combinedClickable(enabled=files,onClick={c.selected=e},onDoubleClick={if(e.directory)c.navigate(remotePath(c.path,e.name))}).padding(6.dp),horizontalArrangement=Arrangement.spacedBy(9.dp)) {
                            Column(Modifier.weight(1f)) {
                                Text((if(e.directory) "▸ " else "")+e.name.replace("\n","\\n"),maxLines=1,overflow=TextOverflow.Ellipsis)
                                if(e.link.isNotEmpty())Text("→ ${e.link}",fontSize=11.sp,color=Color.Gray,maxLines=1)
                            }
                            Text(e.type,Modifier.width(52.dp));Text("${e.size} B",Modifier.width(110.dp));Text(e.permissions,Modifier.width(54.dp))
                            Text(runCatching {DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss").withZone(ZoneId.systemDefault()).format(Instant.ofEpochSecond(e.modified))}.getOrDefault("—"),Modifier.width(155.dp),fontSize=11.sp)
                        }
                    }
                }
            }
            c.progress?.let {p ->
                LinearProgressIndicator(progress={if(p.total==0L)1f else (p.bytes.toDouble()/p.total).toFloat()},modifier=Modifier.fillMaxWidth())
                Text("${p.bytes} / ${p.total} B · ${p.speed} B/s",fontSize=11.sp)
            }
            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                SelectionContainer {Text(pickerError.ifEmpty {c.status},Modifier.weight(1f),maxLines=3,fontSize=11.sp)}
                Spacer(Modifier.weight(0.01f))
                if(c.busy)SmallButton("取消任务",true,c::cancel)
                if(c.pty!=null && c.busy)SmallButton("关闭交互终端") {c.closePty()}
                if(c.pty!=null && !c.busy)SmallButton("命令执行") {c.showCommands()}
                SmallButton(if(c.pty==null) "打开交互终端" else "重新打开终端",idle&&c.connected&&c.capabilities?.pty==true) {terminalOpen=true;c.openPty()}
                SmallButton(if(terminalOpen) "收起终端" else "展开终端",c.pty==null || !c.busy) {terminalOpen=!terminalOpen}
            }
            if(terminalOpen && c.pty!=null) key(c.pty) {InteractiveTerminal(c.pty!!,Modifier.weight(1f).fillMaxWidth())}
            if(terminalOpen && c.pty==null) Terminal(c.terminal,Modifier.weight(1f).fillMaxWidth(),idle,c::clearTerminal,{choose(true,"hdtools-terminal.log",c::export)}) {
                Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                    Field("命令",input,{input=it},Modifier.weight(1f),idle&&c.connected)
                    SmallButton("执行",idle&&c.connected&&input.isNotBlank()) {val value=input;input="";c.execute(value)}
                    Text(if(c.capabilities?.sessionCwd==true) "工作目录随会话保留" else "独立命令 · 不保留目录",fontSize=10.sp,color=Color.Gray)
                }
            }
        }
        c.confirmation?.let {p->AlertDialog(onDismissRequest={c.confirmation=null},title={Text("确认覆盖")},text={Text(p.text)},confirmButton={TextButton(onClick=c::confirm) {Text("覆盖")}},dismissButton={TextButton(onClick={c.confirmation=null}) {Text("取消")}})}
    }
}
@Composable private fun Terminal(text: String,modifier: Modifier,canExport: Boolean,clear: ()->Unit,export: ()->Unit,input: @Composable ()->Unit) {
    val scope=rememberCoroutineScope();val scroll=rememberScrollState()
    var query by remember {mutableStateOf("")};var index by remember {mutableStateOf(0)}
    var follow by remember {mutableStateOf(false)};var reveal by remember {mutableStateOf(false)}
    var layout by remember {mutableStateOf<TextLayoutResult?>(null)}
    val matches=remember(text,query) {
        if(query.isEmpty())emptyList() else buildList {var i=text.indexOf(query,ignoreCase=true);while(i>=0) {add(i);i=text.indexOf(query,i+query.length,ignoreCase=true)}}
    }
    val active=if(matches.isEmpty())0 else index.coerceIn(0,matches.lastIndex)
    val display=remember(text,query,active) {buildAnnotatedString {append(text.ifEmpty {"命令输出保留在当前窗口，可手动导出。"});matches.forEachIndexed {i,start->addStyle(SpanStyle(background=if(i==active)Color(0xffffbb66) else Color(0xffffe9a6)),start,start+query.length)}}}
    LaunchedEffect(text,follow,scroll.maxValue) {if(follow)scroll.scrollTo(scroll.maxValue)}
    LaunchedEffect(layout,active,reveal) {if(reveal && matches.isNotEmpty()) {layout?.let {if(it.layoutInput.text==display)scroll.scrollTo(it.getLineTop(it.getLineForOffset(matches[active])).toInt().coerceIn(0,scroll.maxValue))};reveal=false}}
    Panel(modifier) {
        Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(6.dp)) {
            Text("终端")
            Field("搜索",query,{query=it;index=0;reveal=true},Modifier.weight(1f))
            Text(if(matches.isEmpty()) "0 项" else "${active+1}/${matches.size}",fontSize=11.sp)
            SmallButton("上一处",matches.isNotEmpty()) {index=(active-1+matches.size)%matches.size;reveal=true;follow=false}
            SmallButton("下一处",matches.isNotEmpty()) {index=(active+1)%matches.size;reveal=true;follow=false}
            Checkbox(follow,{follow=it},Modifier.size(22.dp));Text("自动跟随",fontSize=11.sp)
            SmallButton("导出",text.isNotEmpty()&&canExport,export)
            SmallButton("清空",text.isNotEmpty()) {clear();query=""}
            SmallButton("最新") {scope.launch {scroll.scrollTo(scroll.maxValue)}}
        }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            SelectionContainer {Text(display,Modifier.fillMaxSize().padding(end=12.dp).verticalScroll(scroll),fontFamily=FontFamily.Monospace,fontSize=12.sp,lineHeight=17.sp,onTextLayout={layout=it})}
            VerticalScrollbar(rememberScrollbarAdapter(scroll),Modifier.align(Alignment.CenterEnd).fillMaxHeight())
        }
        input()
    }
}

@Composable internal fun SerialModeRow(c: Controller) {
    var traffic by remember(c) {mutableStateOf(c.serialMonitor.traffic())}
    var now by remember {mutableStateOf(System.nanoTime())}
    var stabilityInput by remember(c.stabilityMs) {mutableStateOf(c.stabilityMs.toString())}
    LaunchedEffect(c) {
        var nextTraffic=0L
        while(true) {
            now=System.nanoTime()
            if(now>=nextTraffic) {traffic=c.serialMonitor.traffic();nextTraffic=now+100_000_000L}
            delay(50)
        }
    }
    val state=c.serialState // Controls follow live state, never the display delay.
    // Read the Compose clock unconditionally, including while switching/idle.
    val display=c.serialMonitor.display(now)
    val reading=state.mode==SerialMode.READING
    val closed=state.mode==SerialMode.CLOSED
    val switching=!closed && display.switching
    val color=when(state.mode) {
        SerialMode.READING -> Color(0xff99620a)
        SerialMode.WRITING -> Color(0xff245a93)
        SerialMode.IDLE -> Color(0xff25805e)
        else -> Color(0xff697586)
    }
    Column(verticalArrangement=Arrangement.spacedBy(4.dp)) {
        Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)) {
            Row(Modifier.width(260.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(6.dp)) {
                Text("串口模式：",fontSize=12.sp)
                when {
                    closed -> Text(SerialMode.CLOSED.label,color=color,fontSize=12.sp)
                    state.cancelling -> Text("正在取消读取…",color=color,fontSize=12.sp)
                    switching -> {
                        Box(Modifier.size(8.dp).background(Color(0xffd23e3e).copy(alpha=if(display.redLit)1f else .18f),androidx.compose.foundation.shape.CircleShape))
                        Box(Modifier.size(8.dp).background(Color(0xff25805e).copy(alpha=if(display.redLit).18f else 1f),androidx.compose.foundation.shape.CircleShape))
                        Text("状态切换中",fontSize=12.sp,color=Color(0xff697586))
                    }
                    else -> Text(display.state.mode.label,color=color,fontSize=12.sp)
                }
            }
            Text("RX: ${String.format(java.util.Locale.US,"%,d",traffic.rx)} B",Modifier.width(210.dp),fontFamily=FontFamily.Monospace,fontSize=12.sp,maxLines=1)
            Text("TX: ${String.format(java.util.Locale.US,"%,d",traffic.tx)} B",Modifier.width(210.dp),fontFamily=FontFamily.Monospace,fontSize=12.sp,maxLines=1)
            SmallButton("清零") {c.clearTraffic();traffic=c.serialMonitor.traffic()}
            Spacer(Modifier.weight(1f))
            SmallButton("取消读取",reading&&!state.cancelling,c::cancelRead)
        }
        Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)) {
            Field("稳定时间(ms)",stabilityInput,{stabilityInput=it},Modifier.width(190.dp))
            SmallButton("应用") {c.setStability(stabilityInput)}
            if(c.stabilityError.isNotEmpty())Text(c.stabilityError,color=Color(0xffc53636),fontSize=11.sp)
            else if(reading && !switching && !state.cancelling) {
                val remaining=(state.deadlineNanos-now).coerceAtLeast(0)
                Text("第 ${state.attempt}/3 次 · 剩余 ${String.format("%.1f",remaining/1e9)} 秒 · 禁止写入",fontSize=11.sp,color=color)
            }
        }
    }
}
