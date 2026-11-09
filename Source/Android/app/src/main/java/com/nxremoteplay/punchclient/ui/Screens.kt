package com.nxremoteplay.punchclient.ui

import android.graphics.Bitmap
import android.os.Build
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout
import android.widget.ImageView
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.nxremoteplay.punchclient.BuildConfig
import com.nxremoteplay.punchclient.ConnectState
import com.nxremoteplay.punchclient.Prefs
import com.nxremoteplay.punchclient.PunchSession
import com.nxremoteplay.punchclient.R
import com.nxremoteplay.punchclient.UpdateCheck

// Home, Join and Settings drive PunchSession. Header and footer overlay the full canvas;
// panels center against the canvas height and scale by screenHeightDp / 720.

object DesignCanvas {
    const val Width = 1280f
    const val Height = 720f

    object Panel {
        const val Padding = 147f
        const val Width = DesignCanvas.Width - Padding * 2f // 986
        // Mandatory inset from every panel edge, on every screen.
        const val ContentPadding = 20f
    }
}

val LocalCanvasScale = compositionLocalOf { 1f }

val Float.cv: Dp
    @Composable get() = (this * LocalCanvasScale.current).dp
val Int.cv: Dp
    @Composable get() = this.toFloat().cv
val Float.cvSp: TextUnit
    @Composable get() = (this * LocalCanvasScale.current).sp
val Int.cvSp: TextUnit
    @Composable get() = this.toFloat().cvSp

private enum class Page {
    Home,
    Join,
    Settings,
    Player
}

private val RemoteLinkDarkScheme =
    darkColorScheme(
        primary = RemoteLinkColors.ButtonBlue,
        onPrimary = Color.White,
        background = RemoteLinkColors.BgTopLeft,
        onBackground = RemoteLinkColors.TextPrimary,
        surface = RemoteLinkColors.PanelBg,
        onSurface = RemoteLinkColors.TextPrimary,
    )

@Composable
fun RemoteLinkApp(session: PunchSession) {
    MaterialTheme(colorScheme = RemoteLinkDarkScheme) {
        var page by remember { mutableStateOf(Page.Home) }
        val state by session.state.collectAsState()
        val serverOnline by session.serverOnline.collectAsState()

        // The player appears exactly while a direct connection is up; when it ends/drops the
        // user lands back on Join with the reason shown there.
        LaunchedEffect(state) {
            if (state is ConnectState.Connected) page = Page.Player
            else if (page == Page.Player) page = Page.Join
        }

        // Back (also the gamepad B button): Join returns Home; Settings and the player have their
        // own handlers.
        // Each app start checks GitHub once; a newer release is announced outside the player.
        val update by UpdateCheck.state.collectAsState()
        var updateDialogShown by remember { mutableStateOf(UpdateCheck.popupShown) }
        LaunchedEffect(Unit) { if (UpdateCheck.state.value == UpdateCheck.State.NotChecked) UpdateCheck.start() }
        val available = update as? UpdateCheck.State.Available
        if (available != null && !updateDialogShown && page != Page.Player) {
            val context = LocalContext.current
            fun close() {
                updateDialogShown = true
                UpdateCheck.popupShown = true
            }
            AlertDialog(
                onDismissRequest = { close() },
                title = { Text("Update available") },
                text = {
                    Text(
                        "Version ${available.version} is out. You have ${BuildConfig.VERSION_NAME}.\n\n" +
                            UpdateCheck.ReleasesPageLabel)
                },
                confirmButton = {
                    TextButton(
                        onClick = {
                            close()
                            openReleasesPage(context)
                        }) {
                            Text("Open releases page")
                        }
                },
                dismissButton = { TextButton(onClick = { close() }) { Text("Later") } },
            )
        }

        BackHandler(enabled = page == Page.Join) {
            session.reset()
            page = Page.Home
        }

        Box(Modifier.fillMaxSize()) {
            // Full-bleed background (draws under the system bars too).
            Box(
                Modifier.fillMaxSize()
                    .background(
                        Brush.linearGradient(
                            colors =
                                listOf(
                                    RemoteLinkColors.BgTopLeft,
                                    RemoteLinkColors.BgBottomRight,
                                    RemoteLinkColors.BgBottomLeft,
                                )))) {
                    BackgroundGlow()
                }

            // systemBars only (not displayCutout): a landscape cutout sits on one side, and
            // reserving the same inset on both sides pushes the centered layout off-center.
            BoxWithConstraints(
                Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.systemBars)) {
                    val canvasScale = maxHeight / DesignCanvas.Height.dp

                    CompositionLocalProvider(LocalCanvasScale provides canvasScale) {
                        Box(Modifier.fillMaxSize()) {
                            when (page) {
                                Page.Home -> HomeContent(onConnectClick = { page = Page.Join })
                                Page.Join -> JoinContent(session)
                                Page.Settings ->
                                    SettingsScreen(serverOnline, onBack = { page = Page.Home })
                                Page.Player -> PlayerContent(session, onExit = { page = Page.Home })
                            }
                            // The player is a full-bleed live view with no header/footer chrome.
                            if (page != Page.Player && page != Page.Settings) {
                                TopBar(
                                    Modifier.align(Alignment.TopStart),
                                    showBack = page != Page.Home,
                                    onBack = {
                                        if (page == Page.Join) session.reset()
                                        page = Page.Home
                                    },
                                )
                                BottomBar(
                                    Modifier.align(Alignment.BottomStart),
                                    showSettingsButton = page == Page.Home,
                                    serverOnline = serverOnline,
                                    onSettingsClick = { page = Page.Settings },
                                )
                            }
                        }
                    }
                }
        }
    }
}

@Composable
private fun BackgroundGlow() {
    Canvas(Modifier.fillMaxSize()) {
        drawGlowCircle(
            Offset(size.width * 0.08f, size.height * 0.10f),
            size.minDimension * 0.32f,
            RemoteLinkColors.GlowBlue)
        drawGlowCircle(
            Offset(size.width * 0.88f, size.height * 0.14f),
            size.minDimension * 0.38f,
            RemoteLinkColors.GlowPurple)
        drawGlowCircle(
            Offset(size.width * 0.10f, size.height * 0.90f),
            size.minDimension * 0.40f,
            RemoteLinkColors.GlowTeal)
    }
}

private fun DrawScope.drawGlowCircle(center: Offset, radius: Float, color: Color) {
    drawCircle(color = color, radius = radius, center = center)
}

private const val ScreenEdgeX = 40f
private const val ScreenEdgeY = 28f

@Composable
private fun TopBar(modifier: Modifier = Modifier, showBack: Boolean, onBack: () -> Unit) {
    Row(
        modifier.fillMaxWidth().padding(horizontal = ScreenEdgeX.cv, vertical = ScreenEdgeY.cv),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (showBack) {
            Box(
                Modifier.size(54f.cv)
                    .clip(CircleShape)
                    .background(RemoteLinkColors.BackPillBg)
                    .navClickable(CircleShape, onClick = onBack),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    "‹",
                    color = RemoteLinkColors.TextPrimary,
                    fontSize = 40f.cvSp,
                    fontWeight = FontWeight.ExtraBold)
            }
            Spacer(Modifier.width(16f.cv))
            Text(
                "NX Remote Play",
                color = RemoteLinkColors.TextPrimary,
                fontFamily = Baloo2,
                fontWeight = FontWeight.ExtraBold,
                fontSize = 48f.cvSp)
        } else {
            Image(
                painter = painterResource(R.drawable.app_logo),
                contentDescription = null,
                modifier = Modifier.size(width = 39.6f.cv, height = 44f.cv),
            )
            Spacer(Modifier.width(10f.cv))
            Text(
                "NX Remote Play",
                color = RemoteLinkColors.TextPrimary,
                fontFamily = Baloo2,
                fontWeight = FontWeight.ExtraBold,
                fontSize = 48f.cvSp)
        }
    }
}

@Composable
private fun BottomBar(
    modifier: Modifier = Modifier,
    showSettingsButton: Boolean,
    serverOnline: Boolean?,
    onSettingsClick: () -> Unit
) {
    Row(
        modifier.fillMaxWidth().padding(horizontal = ScreenEdgeX.cv, vertical = ScreenEdgeY.cv),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (showSettingsButton) {
            Box(
                Modifier.heightIn(min = 42f.cv)
                    .clip(RoundedCornerShape(21f.cv))
                    .background(RemoteLinkColors.SettingsPillBg)
                    .navClickable(RoundedCornerShape(21f.cv), onClick = onSettingsClick)
                    .padding(horizontal = 12f.cv, vertical = 6f.cv),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    "Settings",
                    color = RemoteLinkColors.TextPrimary,
                    fontFamily = Baloo2,
                    fontWeight = FontWeight.ExtraBold,
                    fontSize = 36f.cvSp)
            }
        }
        Spacer(Modifier.weight(1f))
        Box(
            Modifier.size(16f.cv)
                .clip(CircleShape)
                .background(
                    when (serverOnline) {
                        true -> RemoteLinkColors.StatusGreen
                        false -> Color(0xFFF04545)
                        null -> RemoteLinkColors.TextMuted
                    }))
        Spacer(Modifier.width(12f.cv))
        Text(
            when (serverOnline) {
                true -> "Server online"
                false -> "Server offline"
                null -> "Checking server..."
            },
            color = RemoteLinkColors.TextSubtle,
            fontFamily = Baloo2,
            fontWeight = FontWeight.SemiBold,
            fontSize = 30f.cvSp)
    }
}

@Composable
private fun HomeContent(onConnectClick: () -> Unit) {
    val scale = LocalCanvasScale.current

    // Card title width is 68.3% of the card (pixel-measured against the real desktop client);
    // the font size self-corrects against the real Text()'s own overflow result.
    val titleMaxWidth = 340f * 0.683f
    var titleFontSize by remember(scale) { mutableFloatStateOf(74f) }

    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        // Fixed 340x350 canvas units with a 28-unit radius; this card does not wrap its content.
        Column(
            Modifier.size(width = 340f.cv, height = 350f.cv)
                .clip(RoundedCornerShape(28f.cv))
                .background(
                    Brush.linearGradient(
                        listOf(
                            RemoteLinkColors.CardGradientTop, RemoteLinkColors.CardGradientBottom)))
                .border(2f.cv, RemoteLinkColors.CardOutline, RoundedCornerShape(28f.cv))
                .initialFocus()
                .navClickable(RoundedCornerShape(28f.cv), onClick = onConnectClick),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Image(
                painter = painterResource(R.drawable.connect_joycon_wireless),
                contentDescription = null,
                modifier = Modifier.size(width = 80f.cv, height = 69f.cv),
            )
            Spacer(Modifier.height(21f.cv))
            Text(
                "Connect To",
                color = RemoteLinkColors.TextPrimary,
                fontFamily = Baloo2,
                fontWeight = FontWeight.ExtraBold,
                fontSize = (titleFontSize * scale).sp,
                textAlign = TextAlign.Center,
                maxLines = 1,
                softWrap = false,
                modifier = Modifier.width(titleMaxWidth.cv),
                onTextLayout = { result ->
                    if (result.didOverflowWidth && titleFontSize > 20f) titleFontSize -= 1f
                },
            )
            Text(
                "A Switch",
                color = RemoteLinkColors.TextPrimary,
                fontFamily = Baloo2,
                fontWeight = FontWeight.ExtraBold,
                fontSize = (titleFontSize * scale).sp,
                textAlign = TextAlign.Center)
        }
    }
}

@Composable
private fun JoinContent(session: PunchSession) {
    val context = LocalContext.current
    // Pre-fill the last connection attempt for convenient reconnecting.
    var sessionId by remember { mutableStateOf(Prefs.getLastSessionId(context)) }
    var password by remember { mutableStateOf(Prefs.getLastSessionPassword(context)) }
    var localError by remember { mutableStateOf<String?>(null) }
    // Remembered details complete: start on Join; otherwise on the Session ID field.
    val startOnJoin = remember { sessionId.length == 5 && password.length == 5 }
    val state by session.state.collectAsState()
    val status by session.status.collectAsState()

    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        // Join panel: 986 wide, 371 as a floor (heightIn, never a hard height - Compose's real
        // text metrics run taller than ImGui's), 20-unit mandatory content padding.
        Column(
            Modifier.width(DesignCanvas.Panel.Width.cv)
                .heightIn(min = 371f.cv)
                .clip(RoundedCornerShape(28f.cv))
                .background(RemoteLinkColors.PanelBg)
                .border(1f.cv, RemoteLinkColors.PanelBorder, RoundedCornerShape(28f.cv))
                .verticalScroll(rememberScrollState())
                .padding(DesignCanvas.Panel.ContentPadding.cv),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "Join A Switch",
                color = RemoteLinkColors.TextPrimary,
                fontFamily = Baloo2,
                fontWeight = FontWeight.ExtraBold,
                fontSize = 60f.cvSp)
            Spacer(Modifier.height(8f.cv))
            Text(
                "Enter the Session ID and Password shown on the Switch's Host screen.",
                color = RemoteLinkColors.TextSubtle,
                fontFamily = Baloo2,
                fontWeight = FontWeight.SemiBold,
                fontSize = 32f.cvSp,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(24f.cv))

            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(24f.cv)) {
                LabeledField(
                    "SESSION ID",
                    sessionId,
                    { sessionId = it.uppercase().take(5) },
                    Modifier.weight(1f),
                    if (startOnJoin) Modifier else Modifier.initialFocus())
                LabeledField(
                    "PASSWORD",
                    password,
                    { password = it.uppercase().take(5) },
                    Modifier.weight(1f))
            }

            Spacer(Modifier.height(24f.cv))

            val busy = state is ConnectState.Requesting || state is ConnectState.Punching
            Button(
                onClick = {
                    val server = Prefs.parseServerAddress(Prefs.getServerAddress(context))
                    if (server == null) {
                        localError =
                            "The server address in Settings isn't valid (expected host:port)."
                        return@Button
                    }
                    localError = null
                    Prefs.setLastSession(context, sessionId, password)
                    session.connect(
                        server.first,
                        server.second,
                        sessionId,
                        password,
                        Prefs.getForceCellular(context))
                },
                enabled = !busy && sessionId.length == 5 && password.length == 5,
                modifier =
                    Modifier.fillMaxWidth()
                        .heightIn(min = 66f.cv)
                        .then(if (startOnJoin) Modifier.initialFocus() else Modifier)
                        .focusRing(RoundedCornerShape(32f.cv)),
                shape = RoundedCornerShape(32f.cv),
                colors =
                    ButtonDefaults.buttonColors(
                        containerColor = RemoteLinkColors.ButtonBlue, contentColor = Color.White),
                contentPadding = PaddingValues(0.dp),
            ) {
                Text(
                    if (busy) "Connecting..." else "Join",
                    color = Color.White,
                    fontFamily = Baloo2,
                    fontWeight = FontWeight.ExtraBold,
                    fontSize = 32f.cvSp)
            }

            val statusText: String? =
                localError
                    ?: when (val s = state) {
                        is ConnectState.Idle -> null
                        is ConnectState.Requesting -> status.ifBlank { "Contacting the server..." }
                        is ConnectState.Punching -> "Opening a direct connection..."
                        is ConnectState.Connected -> "Connected."
                        is ConnectState.Ended -> s.message
                        is ConnectState.Error -> s.message
                    }
            if (statusText != null) {
                Spacer(Modifier.height(12f.cv))
                Text(
                    statusText,
                    color =
                        if (localError != null ||
                            state is ConnectState.Error ||
                            state is ConnectState.Ended)
                            Color(0xFFFF8080)
                        else RemoteLinkColors.TextSubtle,
                    fontFamily = Baloo2,
                    fontSize = 28f.cvSp,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
private fun LabeledField(
    label: String,
    value: String,
    onValueChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    fieldModifier: Modifier = Modifier
) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            label,
            color = RemoteLinkColors.TextMuted,
            fontFamily = Baloo2,
            fontWeight = FontWeight.ExtraBold,
            fontSize = 28f.cvSp)
        Spacer(Modifier.height(8f.cv))
        // No system keyboard: ControllerField opens the app's own gamepad-friendly keyboard.
        ControllerField(
            value,
            onValueChange,
            modifier = fieldModifier,
            title = if (label == "SESSION ID") "Session ID" else "Password",
            kind = KbKind.Code,
            maxLen = 5)
    }
}

// Full-bleed SurfaceView renders H.264; an ImageView above it shows JPEG fallback frames.
// Keep media below the touch controller and size the video box to 16:9 so touch
// coordinates map onto the console's 1280x720 display.
@Composable
private fun PlayerContent(session: PunchSession, onExit: () -> Unit) {
    BackHandler {
        session.reset()
        onExit()
    }

    BoxWithConstraints(Modifier.fillMaxSize().background(Color.Black)) {
        val videoAspect = 16f / 9f
        val containerAspect = constraints.maxWidth.toFloat() / constraints.maxHeight.toFloat()
        val targetWidthPx: Float
        val targetHeightPx: Float
        if (containerAspect > videoAspect) {
            targetHeightPx = constraints.maxHeight.toFloat()
            targetWidthPx = targetHeightPx * videoAspect
        } else {
            targetWidthPx = constraints.maxWidth.toFloat()
            targetHeightPx = targetWidthPx / videoAspect
        }
        val density = LocalDensity.current
        var controller by remember {
            mutableStateOf<com.nxremoteplay.punchclient.TouchControllerView?>(null)
        }

        AndroidView(
            modifier =
                Modifier.size(
                        with(density) { targetWidthPx.toDp() },
                        with(density) { targetHeightPx.toDp() })
                    .align(Alignment.Center),
            factory = { ctx ->
                val container = FrameLayout(ctx)
                val surfaceView = SurfaceView(ctx)
                val overlay =
                    ImageView(ctx).apply {
                        scaleType = ImageView.ScaleType.FIT_CENTER
                        visibility = android.view.View.GONE
                    }
                container.addView(surfaceView, FrameLayout.LayoutParams(-1, -1))
                container.addView(overlay, FrameLayout.LayoutParams(-1, -1))

                // Keep H.264 below the ImageView and the touch controller in the app window.
                surfaceView.setZOrderOnTop(false)
                var showingJpeg = false
                session.onJpegFrame = { bmp: Bitmap ->
                    if (!showingJpeg) {
                        showingJpeg = true
                        surfaceView.setZOrderOnTop(false)
                        overlay.visibility = android.view.View.VISIBLE
                    }
                    overlay.setImageBitmap(bmp)
                }
                session.onRealVideoFrame = {
                    if (showingJpeg) {
                        showingJpeg = false
                        overlay.visibility = android.view.View.GONE
                        surfaceView.setZOrderOnTop(false)
                    }
                }

                surfaceView.holder.addCallback(
                    object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            // Request a refresh rate matching the 60 fps stream. This is only
                            // a display hint; it does not buffer, throttle, or delay frames.
                            if (Build.VERSION.SDK_INT >= 30) {
                                try {
                                    if (Build.VERSION.SDK_INT >= 31) {
                                        holder.surface.setFrameRate(
                                            60f,
                                            Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                                            Surface.CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS,
                                        )
                                    } else {
                                        holder.surface.setFrameRate(
                                            60f,
                                            Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                                        )
                                    }
                                    Log.i("NXRemotePlay", "Requested 60 Hz video surface refresh")
                                } catch (ex: Exception) {
                                    Log.w("NXRemotePlay", "Display refresh hint unavailable", ex)
                                }
                            }
                            session.startStream(holder.surface)
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int
                        ) {}

                        override fun surfaceDestroyed(holder: SurfaceHolder) {}
                    })

                // Video remains below the controller in the same window.
                val touchController =
                    com.nxremoteplay.punchclient.TouchControllerView(
                        ctx, session::sendTouchPad, session::sendTouch)
                container.addView(touchController, FrameLayout.LayoutParams(-1, -1))
                controller = touchController
                container
            },
        )

        DisposableEffect(Unit) {
            onDispose {
                session.onJpegFrame = null
                session.onRealVideoFrame = null
            }
        }

        // Floating hamburger: Home, Controller options, Exit. Stays when the touch controls are
        // hidden.
        StreamMenu(
            controller = controller, onHome = { session.sendHomeTap() }, exitLabel = "Exit") {
                session.reset()
                onExit()
            }
    }
}

internal fun openReleasesPage(context: android.content.Context) {
    runCatching {
        context.startActivity(
            android.content.Intent(
                    android.content.Intent.ACTION_VIEW, android.net.Uri.parse(UpdateCheck.ReleasesPage))
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}
