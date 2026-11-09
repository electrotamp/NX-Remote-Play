package com.nxremoteplay.punchclient.ui

import android.os.Build
import android.view.Gravity
import android.view.ViewGroup
import android.view.WindowManager
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.key.utf16CodePoint
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

// The app's own keyboard. The system (soft) keyboard is never used: a text field here is a
// focusable box that
// opens this keyboard when pressed (touch, A, Enter) and also takes typing from a physical
// keyboard.
//   Code    : letters + digits, always capitals (Session ID / Password).
//   Address : letters, digits and the symbols used in web addresses (server name / IP[:port]).
// Gamepad: D-pad moves, A types, X = delete, Y = shift, Start = done, B = close.

enum class KbKind {
    Code,
    Address
}

private const val AddressSymbols = ".:/-_?=&#@%+~"

private fun accept(c: Char, kind: KbKind): Char? =
    when {
        c.code > 127 -> null
        kind == KbKind.Code -> if (c.isLetterOrDigit()) c.uppercaseChar() else null
        else -> if (c.isLetterOrDigit() || c in AddressSymbols) c else null
    }

@Composable
internal fun ControllerField(
    value: String,
    onValueChange: (String) -> Unit,
    title: String,
    kind: KbKind,
    maxLen: Int,
    modifier: Modifier = Modifier,
    centered: Boolean = true,
    placeholder: String = "",
    openOnStart: Boolean =
        false, // the keyboard comes up as soon as the field appears (a dialog that is only a text
    // box)
    onClosed: ((accepted: Boolean) -> Unit)? =
        null, // keyboard closed: true = Done/Start, false = B / dismissed
) {
    var open by remember { mutableStateOf(openOnStart) }
    val current by rememberUpdatedState(value)
    val shape = RoundedCornerShape(12f.cv)
    Box(
        modifier
            .fillMaxWidth()
            .heightIn(min = 60f.cv)
            .clip(shape)
            .background(RemoteLinkColors.SettingsRowBg)
            .border(1f.cv, RemoteLinkColors.PanelBorder, shape)
            // Physical keyboard typing straight into the focused field.
            .onKeyEvent { e ->
                if (e.type != KeyEventType.KeyDown) return@onKeyEvent false
                if (e.key == Key.Backspace) {
                    if (current.isNotEmpty()) onValueChange(current.dropLast(1))
                    return@onKeyEvent true
                }
                val typed = accept(e.utf16CodePoint.toChar(), kind)
                if (typed != null && e.utf16CodePoint > 32) {
                    if (current.length < maxLen) onValueChange(current + typed)
                    true
                } else false
            }
            .navClickable(shape) { open = true }
            .padding(horizontal = 16f.cv, vertical = 8f.cv),
        contentAlignment = if (centered) Alignment.Center else Alignment.CenterStart,
    ) {
        val empty = value.isEmpty()
        Text(
            if (empty) placeholder.ifEmpty { " " } else value,
            color = if (empty) RemoteLinkColors.TextMuted else RemoteLinkColors.TextPrimary,
            fontFamily = Baloo2,
            fontWeight = FontWeight.ExtraBold,
            fontSize = 30f.cvSp,
            textAlign = if (centered) TextAlign.Center else TextAlign.Start,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
    if (open)
        GamepadKeyboard(title, value, kind, maxLen, onValueChange) { accepted ->
            open = false
            onClosed?.invoke(accepted)
        }
}

@Composable
private fun GamepadKeyboard(
    title: String,
    value: String,
    kind: KbKind,
    maxLen: Int,
    onChange: (String) -> Unit,
    onClose: (accepted: Boolean) -> Unit
) {
    var shift by remember { mutableStateOf(false) }
    val current by rememberUpdatedState(value)
    val first = remember { FocusRequester() }
    // The key is attached a moment after composition; asking too early throws (it crashed when
    // opened straight from a dialog).
    LaunchedEffect(Unit) {
        delay(80)
        runCatching { first.requestFocus() }
    }

    fun type(c: Char) {
        val shown = if (kind == KbKind.Address && !shift && c.isLetter()) c.lowercaseChar() else c
        val typed = accept(shown, kind) ?: return
        if (current.length < maxLen) onChange(current + typed)
    }
    fun delete() {
        if (current.isNotEmpty()) onChange(current.dropLast(1))
    }

    // Holding delete repeats like a computer's backspace: one delete at once, then after a pause a
    // steady, not-too-fast repeat.
    val scope = rememberCoroutineScope()
    var repeater by remember { mutableStateOf<Job?>(null) }
    fun startDelete() {
        if (repeater?.isActive == true) return
        repeater = scope.launch {
            delete()
            delay(450)
            while (true) {
                delete()
                delay(90)
            }
        }
    }
    fun stopDelete() {
        repeater?.cancel()
        repeater = null
    }

    // Its own window, full screen width, stuck to the bottom: the panel is centred on the screen
    // whatever the app's insets.
    Dialog(
        onDismissRequest = { onClose(false) },
        properties =
            DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
            val view = LocalView.current
            // Hidden until the window has been moved to the bottom and resized, so it never jumps
            // or animates in.
            var ready by remember { mutableStateOf(false) }
            SideEffect {
                if (ready) return@SideEffect
                (view.parent as? DialogWindowProvider)?.window?.let { w ->
                    w.setGravity(Gravity.BOTTOM)
                    w.setLayout(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                    w.setDimAmount(0f)
                    w.setWindowAnimations(0)
                    w.addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS)
                    if (Build.VERSION.SDK_INT >= 28)
                        w.attributes =
                            w.attributes.also {
                                it.layoutInDisplayCutoutMode =
                                    WindowManager.LayoutParams
                                        .LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                            }
                    WindowCompat.setDecorFitsSystemWindows(w, false)
                    // Bars stay hidden until a swipe from the edge; a plain touch near the bottom
                    // must not start the system gesture.
                    WindowInsetsControllerCompat(w, view).let {
                        it.systemBarsBehavior =
                            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                        it.hide(WindowInsetsCompat.Type.systemBars())
                    }
                }
                ready = true
            }
            Box(
                Modifier.fillMaxWidth().alpha(if (ready) 1f else 0f),
                contentAlignment = Alignment.BottomCenter) {
                    // Full screen width at the bottom, like a system keyboard.
                    Column(
                        Modifier.fillMaxWidth()
                            .clip(RoundedCornerShape(topStart = 16f.cv, topEnd = 16f.cv))
                            .background(Color(0xFA0E1530))
                            .border(
                                1.5f.cv,
                                RemoteLinkColors.PanelBorder,
                                RoundedCornerShape(topStart = 16f.cv, topEnd = 16f.cv))
                            .onPreviewKeyEvent { e ->
                                if (e.key == Key.ButtonX) { // held X = held backspace
                                    if (e.type == KeyEventType.KeyDown) startDelete()
                                    else if (e.type == KeyEventType.KeyUp) stopDelete()
                                    return@onPreviewKeyEvent true
                                }
                                if (e.type != KeyEventType.KeyUp)
                                    return@onPreviewKeyEvent e.key in
                                        listOf(Key.ButtonB, Key.ButtonY, Key.ButtonStart)
                                when (e.key) {
                                    Key.ButtonB -> {
                                        onClose(false)
                                        true
                                    }
                                    Key.ButtonStart -> {
                                        onClose(true)
                                        true
                                    }
                                    Key.ButtonY -> {
                                        if (kind == KbKind.Address) shift = !shift
                                        true
                                    }
                                    else -> false
                                }
                            }
                            .padding(
                                start = 12f.cv,
                                end = 12f.cv,
                                top = 8f.cv,
                                bottom = 26f.cv), // bottom: keep the keys out of the system-gesture
                        // strip
                        verticalArrangement = Arrangement.spacedBy(6f.cv),
                    ) {
                        // Split layout, like a foldable's keyboard: a half under each thumb, with
                        // the echo of what has been typed
                        // (the keyboard covers the field itself) and Done in the gap between them.
                        val leftRows = buildList {
                            add("12345")
                            add("QWERT")
                            add("ASDFG")
                            add("ZXCV")
                            if (kind == KbKind.Address) add(".:/-_")
                        }
                        val rightRows = buildList {
                            add("67890")
                            add("YUIOP")
                            add("HJKL")
                            add("BNM")
                            if (kind == KbKind.Address) add("?=&#@")
                        }

                        @Composable
                        fun half(rows: List<String>, left: Boolean, modifier: Modifier) {
                            Column(modifier, verticalArrangement = Arrangement.spacedBy(6f.cv)) {
                                rows.forEachIndexed { r, chars ->
                                    Row(
                                        Modifier.fillMaxWidth(),
                                        horizontalArrangement = Arrangement.spacedBy(6f.cv)) {
                                            var used = 0
                                            if (left && r == 3) {
                                                if (kind == KbKind.Address)
                                                    KbKey(
                                                        "Shift",
                                                        Modifier.weight(1f),
                                                        highlighted = shift) {
                                                            shift = !shift
                                                        }
                                                else Spacer(Modifier.weight(1f))
                                                used++
                                            }
                                            chars.forEachIndexed { i, ch ->
                                                val label =
                                                    if (kind == KbKind.Address &&
                                                        !shift &&
                                                        ch.isLetter())
                                                        ch.lowercaseChar()
                                                    else ch
                                                KbKey(
                                                    label.toString(),
                                                    Modifier.weight(1f)
                                                        .then(
                                                            if (left && r == 0 && i == 0)
                                                                Modifier.focusRequester(first)
                                                            else Modifier)) {
                                                        type(ch)
                                                    }
                                                used++
                                            }
                                            if (!left && r == 3) {
                                                HoldKey(
                                                    "Delete",
                                                    Modifier.weight(1f),
                                                    onStart = ::startDelete,
                                                    onStop = ::stopDelete)
                                                KbKey("Done", Modifier.weight(1f), accent = true) {
                                                    onClose(true)
                                                }
                                                used += 2
                                            }
                                            repeat(5 - used) { Spacer(Modifier.weight(1f)) }
                                        }
                                }
                            }
                        }

                        val gridHeight = (leftRows.size * 48f + (leftRows.size - 1) * 6f).cv
                        Row(
                            Modifier.fillMaxWidth().height(gridHeight),
                            horizontalArrangement = Arrangement.spacedBy(14f.cv)) {
                                half(leftRows, true, Modifier.weight(1f))
                                // The gap between the halves: a display of what has been typed, and
                                // the button hints.
                                Column(
                                    Modifier.weight(.62f).fillMaxHeight(),
                                    verticalArrangement = Arrangement.spacedBy(8f.cv)) {
                                        Box(
                                            Modifier.weight(1f)
                                                .fillMaxWidth()
                                                .clip(RoundedCornerShape(12f.cv))
                                                .background(RemoteLinkColors.SettingsRowBg)
                                                .border(
                                                    1f.cv,
                                                    RemoteLinkColors.PanelBorder,
                                                    RoundedCornerShape(12f.cv))
                                                .padding(horizontal = 12f.cv, vertical = 8f.cv),
                                        ) {
                                            Row(
                                                Modifier.align(Alignment.TopStart).fillMaxWidth(),
                                                horizontalArrangement = Arrangement.SpaceBetween) {
                                                    Text(
                                                        title,
                                                        color = RemoteLinkColors.TextMuted,
                                                        fontFamily = Baloo2,
                                                        fontWeight = FontWeight.ExtraBold,
                                                        fontSize = 20f.cvSp,
                                                        maxLines = 1)
                                                    Text(
                                                        "${value.length}/$maxLen",
                                                        color = RemoteLinkColors.TextMuted,
                                                        fontFamily = Baloo2,
                                                        fontWeight = FontWeight.SemiBold,
                                                        fontSize = 18f.cvSp,
                                                        maxLines = 1)
                                                }
                                            Text(
                                                value + "_",
                                                color = Color.White,
                                                fontFamily = Baloo2,
                                                fontWeight = FontWeight.ExtraBold,
                                                fontSize = 38f.cvSp,
                                                textAlign = TextAlign.Center,
                                                maxLines = 2,
                                                overflow = TextOverflow.Ellipsis,
                                                modifier =
                                                    Modifier.align(Alignment.Center)
                                                        .padding(top = 14f.cv))
                                        }
                                        Row(
                                            Modifier.fillMaxWidth(),
                                            horizontalArrangement = Arrangement.SpaceEvenly) {
                                                PadHint(PadButton.South, "type", big = true)
                                                PadHint(PadButton.West, "delete", big = true)
                                            }
                                        Row(
                                            Modifier.fillMaxWidth(),
                                            horizontalArrangement = Arrangement.SpaceEvenly) {
                                                if (kind == KbKind.Address)
                                                    PadHint(PadButton.North, "shift", big = true)
                                                PadHint(PadButton.East, "close", big = true)
                                            }
                                    }
                                half(rightRows, false, Modifier.weight(1f))
                            }
                    }
                }
        }
}

@Composable
private fun KbKey(
    label: String,
    modifier: Modifier,
    highlighted: Boolean = false,
    accent: Boolean = false,
    onClick: () -> Unit
) {
    val shape = RoundedCornerShape(10f.cv)
    Box(
        modifier
            .height(48f.cv)
            .clip(shape)
            .background(
                if (accent) RemoteLinkColors.ButtonBlue
                else if (highlighted) Color(0xFF2E5FC4) else Color(0xFF1B2547))
            .navClickable(shape, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            label,
            color = Color.White,
            fontFamily = Baloo2,
            fontWeight = FontWeight.ExtraBold,
            fontSize = 22f.cvSp,
            maxLines = 1)
    }
}

// A key that acts when pressed and keeps acting while held (touch, or A / Enter on a focused key).
@Composable
private fun HoldKey(label: String, modifier: Modifier, onStart: () -> Unit, onStop: () -> Unit) {
    val shape = RoundedCornerShape(10f.cv)
    var held by remember { mutableStateOf(false) }
    Box(
        modifier
            .height(48f.cv)
            .clip(shape)
            .background(Color(0xFF1B2547))
            .onKeyEvent { e ->
                if (e.key !in listOf(Key.ButtonA, Key.Enter, Key.NumPadEnter, Key.DirectionCenter))
                    return@onKeyEvent false
                if (e.type == KeyEventType.KeyDown && !held) {
                    held = true
                    onStart()
                } else if (e.type == KeyEventType.KeyUp) {
                    held = false
                    onStop()
                }
                true
            }
            .focusRing(shape)
            .focusable()
            .pointerInput(Unit) {
                detectTapGestures(
                    onPress = {
                        onStart()
                        tryAwaitRelease()
                        onStop()
                    })
            },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            label,
            color = Color.White,
            fontFamily = Baloo2,
            fontWeight = FontWeight.ExtraBold,
            fontSize = 22f.cvSp,
            maxLines = 1)
    }
}
