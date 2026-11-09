package com.nxremoteplay.punchclient.ui

import android.view.InputDevice
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.sp

// Controller-specific button glyphs for hint text. Buttons are named by POSITION (Android reports
// the bottom face
// button as BUTTON_A on every pad), and drawn with the label the player's own controller has there:
//   Xbox / generic: A B X Y in colour      PlayStation: cross, circle, square, triangle
// Nintendo: B A Y X (swapped).

enum class PadStyle {
    Xbox,
    PlayStation,
    Nintendo
}

enum class PadButton {
    South,
    East,
    West,
    North,
    Start
}

object PadInput {
    var style by mutableStateOf(PadStyle.Xbox)
        private set

    private fun styleOf(device: InputDevice?): PadStyle? {
        device ?: return null
        val name = device.name.lowercase()
        return when {
            device.vendorId == 0x054c ||
                "dualsense" in name ||
                "dualshock" in name ||
                "playstation" in name -> PadStyle.PlayStation
            device.vendorId == 0x057e ||
                "nintendo" in name ||
                "pro controller" in name ||
                "joy-con" in name -> PadStyle.Nintendo
            else -> PadStyle.Xbox
        }
    }

    private fun isPad(d: InputDevice) =
        (d.sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD

    // With several controllers connected (e.g. an Xbox and a DualSense) the glyphs follow the one
    // in use. A different
    // controller only takes over after the current one has been idle for a moment, so using two at
    // once never flips them.
    private const val HANDOVER_MS = 3000L
    private var activeId = -1
    private var lastActiveAt = 0L

    private fun connected(id: Int) = id >= 0 && InputDevice.getDevice(id) != null

    private fun adopt(device: InputDevice) {
        styleOf(device)?.let { style = it }
        activeId = device.id
        lastActiveAt = android.os.SystemClock.uptimeMillis()
    }

    /**
     * At start-up / when the app regains focus: pick the first connected gamepad, unless one is
     * already in use.
     */
    fun refresh() {
        if (connected(activeId)) return
        InputDevice.getDeviceIds()
            .map { InputDevice.getDevice(it) }
            .firstOrNull { it != null && isPad(it) }
            ?.let { adopt(it) }
    }

    /** A button press from [device]. */
    fun note(device: InputDevice?) {
        if (device == null || !isPad(device)) return
        val now = android.os.SystemClock.uptimeMillis()
        when {
            device.id == activeId -> lastActiveAt = now
            !connected(activeId) || now - lastActiveAt > HANDOVER_MS -> adopt(device)
        }
    }
}

@Composable
internal fun PadGlyph(button: PadButton, size: Dp) {
    val style = PadInput.style
    val letter =
        when (style) {
            PadStyle.Nintendo ->
                when (button) {
                    PadButton.South -> "B"
                    PadButton.East -> "A"
                    PadButton.West -> "Y"
                    PadButton.North -> "X"
                    else -> ""
                }
            else ->
                when (button) {
                    PadButton.South -> "A"
                    PadButton.East -> "B"
                    PadButton.West -> "X"
                    PadButton.North -> "Y"
                    else -> ""
                }
        }
    if (button != PadButton.Start && style != PadStyle.PlayStation) {
        val fill =
            when (style) {
                PadStyle.Nintendo -> Color(0xFF454B5A)
                else ->
                    when (button) {
                        PadButton.South -> Color(0xFF43A047)
                        PadButton.East -> Color(0xFFE53935)
                        PadButton.West -> Color(0xFF1E88E5)
                        else -> Color(0xFFFDD835)
                    }
            }
        val ink =
            if (style == PadStyle.Xbox && button == PadButton.North) Color(0xFF332B00)
            else Color.White
        Box(
            Modifier.size(size).background(fill, androidx.compose.foundation.shape.CircleShape),
            contentAlignment = Alignment.Center) {
                Text(
                    letter,
                    color = ink,
                    fontFamily = FontFamily.SansSerif,
                    fontWeight = FontWeight.Bold,
                    fontSize = (size.value * .62f).sp,
                    style =
                        TextStyle(platformStyle = PlatformTextStyle(includeFontPadding = false)))
            }
        return
    }
    Canvas(Modifier.size(size)) {
        val u = this.size.minDimension / 2f
        val c = center
        val t = u * .17f
        fun p(x: Float, y: Float) = Offset(c.x + x * u, c.y + y * u)
        drawCircle(Color(0xFF2A3042), u, c)
        val white = Color.White
        if (button == PadButton.Start) {
            if (style == PadStyle.Nintendo) {
                drawLine(white, p(-.5f, 0f), p(.5f, 0f), t, StrokeCap.Round)
                drawLine(white, p(0f, -.5f), p(0f, .5f), t, StrokeCap.Round)
            } else
                for (y in listOf(-.42f, 0f, .42f)) drawLine(
                    white, p(-.5f, y), p(.5f, y), t, StrokeCap.Round)
            return@Canvas
        }
        val stroke = Stroke(width = t, cap = StrokeCap.Round, join = StrokeJoin.Round)
        when (button) {
            PadButton.South -> {
                val col = Color(0xFF7CB4FF)
                drawLine(col, p(-.42f, -.42f), p(.42f, .42f), t, StrokeCap.Round)
                drawLine(col, p(.42f, -.42f), p(-.42f, .42f), t, StrokeCap.Round)
            }
            PadButton.East -> drawCircle(Color(0xFFFF6B7A), u * .46f, c, style = stroke)
            PadButton.West ->
                drawRect(
                    Color(0xFFE48AE4),
                    p(-.4f, -.4f),
                    androidx.compose.ui.geometry.Size(.8f * u, .8f * u),
                    style = stroke)
            else ->
                drawPath(
                    Path().apply {
                        moveTo(p(0f, -.5f).x, p(0f, -.5f).y)
                        lineTo(p(.48f, .36f).x, p(.48f, .36f).y)
                        lineTo(p(-.48f, .36f).x, p(-.48f, .36f).y)
                        close()
                    },
                    Color(0xFF4DD6A4),
                    style = stroke)
        }
    }
}

/** One hint: glyph, then what it does. */
@Composable
internal fun PadHint(
    button: PadButton,
    text: String,
    textColor: Color = RemoteLinkColors.TextMuted,
    big: Boolean = false
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        PadGlyph(button, (if (big) 28f else 20f).cv)
        Spacer(Modifier.width(8f.cv))
        Text(
            text,
            color = textColor,
            fontFamily = Baloo2,
            fontWeight = FontWeight.SemiBold,
            fontSize = (if (big) 22f else 17f).cvSp,
            maxLines = 1)
    }
}
