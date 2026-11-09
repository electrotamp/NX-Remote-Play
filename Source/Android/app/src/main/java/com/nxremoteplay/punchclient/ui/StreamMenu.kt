package com.nxremoteplay.punchclient.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nxremoteplay.punchclient.Prefs
import com.nxremoteplay.punchclient.TouchControllerView
import kotlin.math.roundToInt
import kotlinx.coroutines.delay

// The on-screen menu while viewing a stream (and in the Settings touch-controls preview): one
// floating hamburger
// circle, draggable anywhere (position remembered), fading when idle, that stays when the touch
// controls are hidden.
// Menu: Home (stream only), Controller > (edit layout, hide/show controls, vibration / reset),
// Exit.
// It is an ordinary overlay in the app window; a tap anywhere else only closes it and never reaches
// the console.

private val MenuBg = Color(0xF2121A33)
private val MenuBorder = Color(0x73667AAB)

@Composable
internal fun StreamMenu(
    controller: TouchControllerView?,
    onHome: (() -> Unit)?,
    exitLabel: String,
    onExit: () -> Unit,
) {
    val ctx = LocalContext.current
    val density = LocalDensity.current
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val containerW = with(density) { maxWidth.toPx() }
        val containerH = with(density) { maxHeight.toPx() }
        val sizePx = with(density) { 52.dp.toPx() }
        val slopPx = with(density) { 8.dp.toPx() }
        val maxX = (containerW - sizePx).coerceAtLeast(1f)
        val maxY = (containerH - sizePx).coerceAtLeast(1f)

        val saved = remember { Prefs.getMenuPos(ctx) }
        var fx by remember { mutableFloatStateOf(saved.first.coerceIn(0f, 1f)) }
        var fy by remember { mutableFloatStateOf(saved.second.coerceIn(0f, 1f)) }
        var open by remember { mutableStateOf(false) }
        var sub by remember { mutableStateOf(false) }
        var activity by remember {
            mutableIntStateOf(0)
        } // bumps on any touch: restarts the fade timer
        var faded by remember { mutableStateOf(false) }
        var editing by remember { mutableStateOf(false) }
        var controlsShown by remember(controller) { mutableStateOf(controller?.controlsVisible ?: false) }

        LaunchedEffect(activity, open) {
            faded = false
            if (!open) {
                delay(3_000)
                faded = true
            }
        }
        val alpha by animateFloatAsState(if (faded && !open) 0.35f else 1f, label = "menuAlpha")

        val posX = fx * maxX
        val posY = fy * maxY

        // Closed: only the circle takes touches. Open: a full-screen catcher closes the menu on any
        // other tap.
        if (open) {
            Box(
                Modifier.fillMaxSize().pointerInput(Unit) {
                    detectTapGestures {
                        open = false
                        sub = false
                        activity++
                    }
                })
            val rows = if (sub) 4 else (if (onHome != null) 3 else 2)
            val panelW = with(density) { 210.dp.toPx() }
            val panelH = with(density) { (rows * 52 + 8).dp.toPx() }
            val gap = with(density) { 8.dp.toPx() }
            val px =
                (posX + sizePx / 2f - panelW / 2f).coerceIn(
                    gap, (containerW - panelW - gap).coerceAtLeast(gap))
            val py =
                if (posY + sizePx + gap + panelH <= containerH) posY + sizePx + gap
                else (posY - gap - panelH).coerceAtLeast(gap)
            Column(
                Modifier.offset { IntOffset(px.roundToInt(), py.roundToInt()) }
                    .width(210.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(MenuBg)
                    .border(1.dp, MenuBorder, RoundedCornerShape(16.dp))
                    .padding(vertical = 4.dp),
            ) {
                if (!sub) {
                    if (onHome != null)
                        MenuItem("Home") {
                            open = false
                            onHome()
                        }
                    if (controller != null) MenuItem("Controller  ›") { sub = true }
                    MenuItem(exitLabel) {
                        open = false
                        onExit()
                    }
                } else {
                    MenuItem("‹  Back") { sub = false }
                    MenuItem(if (editing) "Save layout" else "Edit layout") {
                        controller?.toggleEdit()
                        editing = controller?.editing ?: false
                        controlsShown = true
                        open = false
                        sub = false
                    }
                    MenuItem(if (controlsShown) "Hide controls" else "Show controls") {
                        controller?.toggleControls()
                        controlsShown = controller?.controlsVisible ?: false
                        editing = false
                        open = false
                        sub = false
                    }
                    MenuItem("Vibration / reset") {
                        open = false
                        sub = false
                        controller?.showOptions()
                    }
                }
            }
        }

        Box(
            Modifier.offset { IntOffset(posX.roundToInt(), posY.roundToInt()) }
                .size(52.dp)
                .alpha(alpha)
                .clip(CircleShape)
                .background(Color(0xD9141A28))
                .border(1.5.dp, Color(0xB3FFFFFF), CircleShape)
                .pointerInput(Unit) {
                    detectTapGestures {
                        open = !open
                        sub = false
                        activity++
                    }
                }
                .pointerInput(maxX, maxY) {
                    var dragged = Offset.Zero
                    detectDragGestures(
                        onDragStart = {
                            dragged = Offset.Zero
                            activity++
                        },
                        onDragEnd = {
                            Prefs.setMenuPos(ctx, fx, fy)
                            activity++
                        },
                    ) { change, drag ->
                        change.consume()
                        dragged += drag
                        if (dragged.getDistance() > slopPx) {
                            fx = (fx + drag.x / maxX).coerceIn(0f, 1f)
                            fy = (fy + drag.y / maxY).coerceIn(0f, 1f)
                        }
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            Canvas(Modifier.size(24.dp)) {
                val w = size.width
                val stroke = 3.dp.toPx()
                for (i in 0..2) {
                    val y = w * (0.2f + 0.3f * i)
                    drawLine(
                        Color.White,
                        Offset(0f, y),
                        Offset(w, y),
                        strokeWidth = stroke,
                        cap = StrokeCap.Round)
                }
            }
        }
    }
}

@Composable
private fun MenuItem(label: String, onClick: () -> Unit) {
    Box(
        Modifier.fillMaxWidth()
            .height(52.dp)
            .navClickable(RoundedCornerShape(12.dp), onClick = onClick)
            .padding(horizontal = 18.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(
            label,
            color = Color.White,
            fontFamily = Baloo2,
            fontWeight = FontWeight.ExtraBold,
            fontSize = 19.sp,
            maxLines = 1)
    }
}
