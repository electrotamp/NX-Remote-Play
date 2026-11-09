@file:OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)

package com.nxremoteplay.punchclient.ui

import android.view.ViewGroup
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.nxremoteplay.punchclient.Prefs
import com.nxremoteplay.punchclient.R
import kotlinx.coroutines.delay

// Settings page in the shared NX-Remote-Play settings design (sidebar of categories + a card of
// rows with icon, title, description and a toggle / value / chevron), the same layout the Windows
// desktop client and the Switch NRO use. Sizes are in the 1280x720 design canvas (.cv / .cvSp);
// unlike the desktop this page also fills a wider-than-16:9 screen: the content column grows.

private val IconColor = Color(0xFFE0EBFF)
private val SidebarBlue = Color(0xFF1F63E8)
private val CardBg = Color(0xEB121C38)
private val CardBorder = Color(0x73667AAB)
private val RowDivider = Color(0x4D667AAB)
private val DescColor = Color(0xFF9EB0D6)
private val ToggleOn = Color(0xFF1290FA)
private val ToggleOff = Color(0xFF7F8A9E)

private class SettingsItem(
    val icon: String,
    val title: String,
    val desc: String,
    val toggle: Boolean? = null,
    val value: String? = null,
    val chevron: Boolean = false,
    val onClick: (() -> Unit)? = null,
)

private val TabNames = listOf("Connection", "Controller", "Testing", "About")
private val TabIcons = listOf("wifi", "pad", "flask", "info")
private val TabSubtitles =
    listOf(
        "Configure how the app connects to your PC or server.",
        "Configure touch controls for remote play.",
        "Experimental and diagnostic options.",
        "App information and connection details.",
    )

@Composable
internal fun SettingsScreen(serverOnline: Boolean?, onBack: () -> Unit) {
    val context = LocalContext.current
    BackHandler(onBack = onBack)
    var tab by rememberSaveable { mutableIntStateOf(0) }
    val tabFocus = remember { FocusRequester() }
    val inputMode = LocalInputModeManager.current
    LaunchedEffect(Unit) {
        delay(80)
        inputMode.requestInputMode(InputMode.Keyboard)
        runCatching { tabFocus.requestFocus() }
    }
    var useDefaultServer by remember { mutableStateOf(Prefs.getUseDefaultServer(context)) }
    var customServer by remember { mutableStateOf(Prefs.getCustomServer(context)) }
    var forceCellular by remember { mutableStateOf(Prefs.getForceCellular(context)) }
    var udpVideo by remember { mutableStateOf(Prefs.getUdpVideo(context)) }
    var audioDelayMs by remember { mutableStateOf(Prefs.getAudioDelayMs(context)) }
    var upscaleMode by remember { mutableStateOf(Prefs.getUpscaleMode(context)) }
    var showServerDialog by remember { mutableStateOf(false) }
    var showPreview by remember { mutableStateOf(false) }
    val update by com.nxremoteplay.punchclient.UpdateCheck.state.collectAsState()

    val items: List<SettingsItem> =
        when (tab) {
            0 ->
                listOf(
                    SettingsItem(
                        "ring",
                        "Use default server",
                        "Use the default NX-Remote-Play server address.",
                        toggle = useDefaultServer,
                        onClick = {
                            useDefaultServer = !useDefaultServer
                            Prefs.setUseDefaultServer(context, useDefaultServer)
                            if (!useDefaultServer && customServer.isBlank()) showServerDialog = true
                        }),
                    SettingsItem(
                        "globe",
                        "Server",
                        "Custom server address or IP (optional :port).",
                        value = customServer.ifBlank { "Not set" },
                        chevron = true,
                        onClick = { showServerDialog = true }),
                    SettingsItem(
                        "phone",
                        "Force cellular data (testing)",
                        "Use mobile data even when Wi-Fi is available.",
                        toggle = forceCellular,
                        onClick = {
                            forceCellular = !forceCellular
                            Prefs.setForceCellular(context, forceCellular)
                        }),
                )
            1 ->
                listOf(
                    SettingsItem(
                        "pad",
                        "Touch controls",
                        "Customize the on-screen layout and test vibration.",
                        chevron = true,
                        onClick = { showPreview = true }),
                )
            2 ->
                listOf(
                    SettingsItem(
                        "video",
                        "Video over UDP",
                        "Stream video using UDP (testing).",
                        toggle = udpVideo,
                        onClick = {
                            udpVideo = !udpVideo
                            Prefs.setUdpVideo(context, udpVideo)
                        }),
                    SettingsItem(
                        "video",
                        "Picture upscaling",
                        "Sharper picture when the screen is bigger than 1280x720. Off, FSR (AMD FidelityFX Super Resolution 1, best) or Cubic (lighter). Takes effect the next time you connect.",
                        value = Prefs.UpscaleNames[upscaleMode],
                        chevron = true,
                        onClick = {
                            upscaleMode = (upscaleMode + 1) % Prefs.UpscaleNames.size
                            Prefs.setUpscaleMode(context, upscaleMode)
                        }),
                    SettingsItem(
                        "video",
                        "Audio delay",
                        "Sound queued before playback. Lower = sound earlier; raise it if audio crackles.",
                        value = "$audioDelayMs ms",
                        chevron = true,
                        onClick = {
                            val presets = Prefs.AudioDelayPresetsMs
                            audioDelayMs =
                                presets[
                                    (presets.indexOfFirst { it > audioDelayMs }.takeIf { it >= 0 }
                                        ?: 0)]
                            Prefs.setAudioDelayMs(context, audioDelayMs)
                        }),
                )
            else ->
                listOf(
                    SettingsItem(
                        "info",
                        "App version",
                        "",
                        value = com.nxremoteplay.punchclient.BuildConfig.VERSION_NAME),
                    SettingsItem(
                        "ring",
                        "Check for updates",
                        "Asks GitHub whether a newer release is out, as the app does each time it starts. Tap to check again.",
                        value = com.nxremoteplay.punchclient.UpdateCheck.statusText(update),
                        chevron = true,
                        onClick = { com.nxremoteplay.punchclient.UpdateCheck.start() }),
                    SettingsItem(
                        "globe",
                        "Releases page",
                        com.nxremoteplay.punchclient.UpdateCheck.ReleasesPageLabel,
                        chevron = true,
                        onClick = { openReleasesPage(context) }),
                    SettingsItem("ring", "Connection", "", value = "Direct (TCP hole punch)"),
                    SettingsItem("video", "Video", "", value = "H.264 / JPEG"),
                    SettingsItem(
                        "info",
                        "Export logs",
                        "Saves the app's logs and the latest stream traces as a zip in Downloads/NX-Remote-Play. Send it with a description of what went wrong.",
                        chevron = true,
                        onClick = {
                            com.nxremoteplay.punchclient.BaselinePublicLogs.save(
                                context, manual = true)
                        }),
                )
        }

    Box(
        Modifier.fillMaxSize()
            .padding(20f.cv)
            .clip(RoundedCornerShape(30f.cv))
            .border(2f.cv, CardBorder, RoundedCornerShape(30f.cv)),
    ) {
        Column(Modifier.fillMaxSize().padding(horizontal = 36f.cv, vertical = 24f.cv)) {
            // Header
            Row(
                Modifier.fillMaxWidth().height(56f.cv),
                verticalAlignment = Alignment.CenterVertically) {
                    Image(
                        painterResource(R.drawable.app_logo),
                        null,
                        Modifier.size(width = 40f.cv, height = 44f.cv))
                    Spacer(Modifier.width(14f.cv))
                    Text(
                        "NX Remote Play",
                        color = RemoteLinkColors.TextPrimary,
                        fontFamily = Baloo2,
                        fontWeight = FontWeight.ExtraBold,
                        fontSize = 34f.cvSp)
                    Spacer(Modifier.weight(1f))
                    Text(
                        com.nxremoteplay.punchclient.BuildConfig.VERSION_NAME.substringBefore('-'),
                        color = RemoteLinkColors.TextSubtle,
                        fontFamily = Baloo2,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 22f.cvSp)
                }
            Spacer(Modifier.height(12f.cv))
            Row(Modifier.weight(1f).fillMaxWidth()) {
                // Sidebar
                Column(
                    Modifier.width(250f.cv).fillMaxHeight(),
                    verticalArrangement = Arrangement.spacedBy(10f.cv)) {
                        TabNames.forEachIndexed { i, name ->
                            val selected = i == tab
                            Row(
                                Modifier.fillMaxWidth()
                                    .height(58f.cv)
                                    .clip(RoundedCornerShape(16f.cv))
                                    .background(if (selected) SidebarBlue else Color.Transparent)
                                    .then(
                                        if (selected) Modifier.focusRequester(tabFocus)
                                        else Modifier)
                                    .navClickable(
                                        RoundedCornerShape(16f.cv), onFocus = { tab = i }) {
                                            tab = i
                                        }
                                    .padding(horizontal = 18f.cv),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                SettingsIcon(
                                    TabIcons[i], if (selected) Color.White else IconColor, 30f.cv)
                                Spacer(Modifier.width(18f.cv))
                                Text(
                                    name,
                                    color =
                                        if (selected) Color.White else RemoteLinkColors.TextSubtle,
                                    fontFamily = Baloo2,
                                    fontWeight = FontWeight.ExtraBold,
                                    fontSize = 24f.cvSp,
                                    maxLines = 1)
                            }
                        }
                    }
                Spacer(Modifier.width(18f.cv))
                Box(Modifier.width(2f.cv).fillMaxHeight().background(RowDivider))
                Spacer(Modifier.width(26f.cv))
                // Content
                Column(Modifier.weight(1f).fillMaxHeight()) {
                    Text(
                        TabNames[tab],
                        color = Color.White,
                        fontFamily = Baloo2,
                        fontWeight = FontWeight.ExtraBold,
                        fontSize = 44f.cvSp)
                    Text(
                        TabSubtitles[tab],
                        color = RemoteLinkColors.TextSubtle,
                        fontFamily = Baloo2,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 21f.cvSp)
                    Spacer(Modifier.height(14f.cv))
                    Column(
                        Modifier.weight(1f, fill = false)
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(18f.cv))
                            .background(CardBg)
                            .border(1.5f.cv, CardBorder, RoundedCornerShape(18f.cv))
                            .verticalScroll(rememberScrollState()),
                    ) {
                        items.forEachIndexed { i, item ->
                            SettingsRowView(item)
                            if (i < items.lastIndex)
                                Box(
                                    Modifier.padding(start = 64f.cv, end = 20f.cv)
                                        .fillMaxWidth()
                                        .height(1f.cv)
                                        .background(RowDivider))
                        }
                    }
                }
            }
            Spacer(Modifier.height(10f.cv))
            // Footer: Back (left), server status (right)
            Row(
                Modifier.fillMaxWidth().height(50f.cv),
                verticalAlignment = Alignment.CenterVertically) {
                    Row(
                        Modifier.height(48f.cv)
                            .clip(RoundedCornerShape(24f.cv))
                            .background(Color(0xFF1F2B49))
                            .navClickable(RoundedCornerShape(24f.cv), onClick = onBack)
                            .padding(horizontal = 18f.cv),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        PadGlyph(PadButton.East, 28f.cv)
                        Spacer(Modifier.width(12f.cv))
                        Text(
                            "Back",
                            color = Color.White,
                            fontFamily = Baloo2,
                            fontWeight = FontWeight.ExtraBold,
                            fontSize = 24f.cvSp)
                    }
                    Spacer(Modifier.weight(1f))
                    Box(
                        Modifier.size(13f.cv)
                            .clip(CircleShape)
                            .background(
                                when (serverOnline) {
                                    true -> RemoteLinkColors.StatusGreen
                                    false -> Color(0xFFF04545)
                                    null -> RemoteLinkColors.TextMuted
                                }))
                    Spacer(Modifier.width(10f.cv))
                    Text(
                        when (serverOnline) {
                            true -> "Server online"
                            false -> "Server offline"
                            null -> "Checking server..."
                        },
                        color = RemoteLinkColors.TextSubtle,
                        fontFamily = Baloo2,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 22f.cvSp)
                }
        }
    }

    if (showPreview) TouchControlsPreview { showPreview = false }

    if (showServerDialog) {
        var editValue by remember { mutableStateOf(customServer) }
        fun commit() {
            // Used exactly as typed (trimmed); only accepted if it parses as host[:port].
            val typed = editValue.trim()
            if (Prefs.parseServerAddress(typed) != null) {
                Prefs.setCustomServer(context, typed)
                customServer = typed
            }
            showServerDialog = false
        }
        AlertDialog(
            // B (gamepad) closes the dialog, which also takes the keyboard down. A dialog is its
            // own window, so
            // the Activity never sees its keys.
            modifier =
                Modifier.onPreviewKeyEvent { e ->
                    if (e.key == Key.ButtonB && e.type == KeyEventType.KeyUp) {
                        showServerDialog = false
                        true
                    } else false
                },
            onDismissRequest = { showServerDialog = false },
            title = { Text("Server") },
            text = {
                ControllerField(
                    value = editValue,
                    onValueChange = { newValue: String -> editValue = newValue },
                    title = "Server",
                    kind = KbKind.Address,
                    maxLen = 64,
                    centered = false,
                    placeholder = "IP or name, optional :port (default 7921)",
                    openOnStart = true,
                    // The dialog is only a text box: Done / Start saves and closes it with the
                    // keyboard, B closes both without saving.
                    onClosed = { accepted -> if (accepted) commit() else showServerDialog = false },
                )
            },
            confirmButton = {
                TextButton(onClick = { commit() }) { Text("Save") }
            },
            dismissButton = {
                TextButton(onClick = { showServerDialog = false }) { Text("Cancel") }
            },
        )
    }
}

@Composable
private fun SettingsRowView(item: SettingsItem) {
    val hasDesc = item.desc.isNotEmpty()
    Row(
        Modifier.fillMaxWidth()
            .heightIn(min = (if (hasDesc) 74f else 64f).cv)
            .then(
                if (item.onClick != null)
                    Modifier.navClickable(RoundedCornerShape(14f.cv), onClick = item.onClick)
                else Modifier)
            .padding(horizontal = 22f.cv, vertical = 8f.cv),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        SettingsIcon(item.icon, IconColor, 30f.cv)
        Spacer(Modifier.width(18f.cv))
        Column(Modifier.weight(1f)) {
            Text(
                item.title,
                color = Color.White,
                fontFamily = Baloo2,
                fontWeight = FontWeight.ExtraBold,
                fontSize = 22f.cvSp,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis)
            if (hasDesc)
                Text(
                    item.desc,
                    color = DescColor,
                    fontFamily = Baloo2,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 17f.cvSp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis)
        }
        Spacer(Modifier.width(14f.cv))
        if (item.toggle != null) {
            val on = item.toggle
            Box(
                Modifier.size(width = 66f.cv, height = 36f.cv)
                    .clip(RoundedCornerShape(50))
                    .background(if (on) ToggleOn else ToggleOff)
                    .padding(4f.cv),
                contentAlignment = if (on) Alignment.CenterEnd else Alignment.CenterStart,
            ) {
                Box(Modifier.size(28f.cv).clip(CircleShape).background(Color.White))
            }
        }
        if (item.value != null) {
            Text(
                item.value,
                color = Color.White,
                fontFamily = Baloo2,
                fontWeight = FontWeight.ExtraBold,
                fontSize = 22f.cvSp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.widthIn(max = 300f.cv))
        }
        if (item.chevron) {
            Spacer(Modifier.width(8f.cv))
            SettingsIcon("chev", IconColor, 22f.cv)
        }
    }
}

@Composable
internal fun SettingsIcon(kind: String, color: Color, size: Dp) {
    Canvas(Modifier.size(size)) {
        val u = this.size.minDimension / 2f
        val c = center
        val t = (u * .15f).coerceAtLeast(1.5f)
        val stroke = Stroke(width = t, cap = StrokeCap.Round, join = StrokeJoin.Round)
        fun p(x: Float, y: Float) = Offset(c.x + x * u, c.y + y * u)
        fun line(a: Offset, b: Offset) =
            drawLine(color, a, b, strokeWidth = t, cap = StrokeCap.Round)
        fun box(x0: Float, y0: Float, x1: Float, y1: Float, r: Float) =
            drawRoundRect(
                color,
                p(x0, y0),
                Size((x1 - x0) * u, (y1 - y0) * u),
                CornerRadius(r * u),
                style = stroke)
        when (kind) {
            "wifi" -> {
                val o = p(0f, .6f)
                drawCircle(color, t * .8f, o)
                for (r in listOf(.5f, .9f, 1.3f)) drawArc(
                    color,
                    -135f,
                    90f,
                    false,
                    Offset(o.x - r * u, o.y - r * u),
                    Size(2 * r * u, 2 * r * u),
                    style = stroke)
            }
            "signal" -> {
                val o = p(0f, 0f)
                drawCircle(color, t * .9f, o)
                for (r in listOf(.55f, 1.0f)) {
                    drawArc(
                        color,
                        -45f,
                        90f,
                        false,
                        Offset(o.x - r * u, o.y - r * u),
                        Size(2 * r * u, 2 * r * u),
                        style = stroke)
                    drawArc(
                        color,
                        135f,
                        90f,
                        false,
                        Offset(o.x - r * u, o.y - r * u),
                        Size(2 * r * u, 2 * r * u),
                        style = stroke)
                }
            }
            "pad" -> {
                box(-1.15f, -.65f, 1.15f, .65f, .6f)
                line(p(-.85f, 0f), p(-.25f, 0f))
                line(p(-.55f, -.3f), p(-.55f, .3f))
                drawCircle(color, t * .7f, p(.45f, .15f))
                drawCircle(color, t * .7f, p(.8f, -.15f))
            }
            "globe" -> {
                drawCircle(color, 1.05f * u, c, style = stroke)
                line(p(-1.05f, 0f), p(1.05f, 0f))
                drawOval(color, p(-.45f, -1.05f), Size(.9f * u, 2.1f * u), style = stroke)
            }
            "phone" -> {
                box(-.55f, -1f, .55f, 1f, .2f)
                drawCircle(color, t * .6f, p(0f, .7f))
            }
            "video" -> {
                box(-1.05f, -.8f, 1.05f, .8f, .22f)
                line(p(-1.05f, -.35f), p(1.05f, -.35f))
            }
            "flask" -> {
                val path =
                    Path().apply {
                        moveTo(p(-.3f, -1f).x, p(-.3f, -1f).y)
                        lineTo(p(-.3f, -.2f).x, p(-.3f, -.2f).y)
                        lineTo(p(-.85f, .85f).x, p(-.85f, .85f).y)
                        lineTo(p(.85f, .85f).x, p(.85f, .85f).y)
                        lineTo(p(.3f, -.2f).x, p(.3f, -.2f).y)
                        lineTo(p(.3f, -1f).x, p(.3f, -1f).y)
                    }
                drawPath(path, color, style = stroke)
                line(p(-.55f, -1f), p(.55f, -1f))
            }
            "info" -> {
                drawCircle(color, 1.05f * u, c, style = stroke)
                drawCircle(color, t * .7f, p(0f, -.5f))
                line(p(0f, -.1f), p(0f, .6f))
            }
            "chev" -> {
                line(p(-.35f, -.8f), p(.35f, 0f))
                line(p(.35f, 0f), p(-.35f, .8f))
            }
            else -> { // "ring"
                drawCircle(color, 1.05f * u, c, style = stroke)
                drawCircle(color, .45f * u, c, style = stroke)
            }
        }
    }
}

// The page's primary element takes focus as soon as the page appears, so a controller press is not
// needed just to start highlighting.
@Composable
internal fun Modifier.initialFocus(): Modifier {
    val requester = remember { FocusRequester() }
    val inputMode = LocalInputModeManager.current
    // Buttons only take focus in keyboard/gamepad input mode, which a fresh launch is not in until
    // the first key press.
    LaunchedEffect(Unit) {
        delay(80)
        inputMode.requestInputMode(InputMode.Keyboard)
        runCatching { requester.requestFocus() }
    }
    return this.focusRequester(requester)
}

// Focus ring for D-pad / gamepad navigation (replaces the default focus shadow).
@Composable
internal fun Modifier.focusRing(shape: Shape, onFocus: (() -> Unit)? = null): Modifier {
    var focused by remember { mutableStateOf(false) }
    return this.onFocusChanged {
            focused = it.hasFocus
            if (it.hasFocus) onFocus?.invoke()
        }
        .then(if (focused) Modifier.border(3f.cv, Color(0xFF6BC2FF), shape) else Modifier)
}

// Clickable reachable with a D-pad / gamepad: focus ring, A button acts like Enter. onFocus fires
// when focus arrives.
@Composable
internal fun Modifier.navClickable(
    shape: Shape,
    onFocus: (() -> Unit)? = null,
    onClick: () -> Unit
): Modifier =
    this.onKeyEvent { e ->
            if (e.key == Key.ButtonA && e.type == KeyEventType.KeyUp) {
                onClick()
                true
            } else false
        }
        .focusRing(shape, onFocus)
        .clickable(
            interactionSource =
                remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
            indication = null,
            onClick = onClick)

// Full-screen preview of the touch controller, used to arrange the buttons and test vibration. Its
// own menu (the hamburger)
// has Controller options and Close; the buttons send nothing anywhere.
@Composable
private fun TouchControlsPreview(onClose: () -> Unit) {
    Dialog(
        onDismissRequest = onClose,
        properties =
            DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
            val view = LocalView.current
            SideEffect {
                (view.parent as? DialogWindowProvider)?.window?.let { w ->
                    w.setLayout(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
                    w.setBackgroundDrawableResource(android.R.color.black)
                    w.addFlags(android.view.WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS)
                    if (android.os.Build.VERSION.SDK_INT >= 28)
                        w.attributes =
                            w.attributes.also {
                                it.layoutInDisplayCutoutMode =
                                    android.view.WindowManager.LayoutParams
                                        .LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                            }
                    WindowCompat.setDecorFitsSystemWindows(w, false)
                    WindowInsetsControllerCompat(w, view).let {
                        it.systemBarsBehavior =
                            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                        it.hide(WindowInsetsCompat.Type.systemBars())
                    }
                }
            }
            var controller by remember {
                mutableStateOf<com.nxremoteplay.punchclient.TouchControllerView?>(null)
            }
            Box(Modifier.fillMaxSize().background(Color.Black)) {
                androidx.compose.ui.viewinterop.AndroidView(
                    modifier = Modifier.fillMaxSize(),
                    factory = { ctx ->
                        com.nxremoteplay.punchclient
                            .TouchControllerView(
                                ctx, { _, _, _, _, _ -> }, { _, _, _ -> }, startVisible = true)
                            .also { controller = it }
                    },
                )
                StreamMenu(
                    controller = controller, onHome = null, exitLabel = "Close", onExit = onClose)
            }
        }
}
