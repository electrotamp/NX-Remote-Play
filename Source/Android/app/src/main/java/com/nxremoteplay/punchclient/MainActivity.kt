package com.nxremoteplay.punchclient

import android.graphics.Color
import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.nxremoteplay.punchclient.ui.RemoteLinkApp

// Compose Home, Join and Settings screens share one PunchSession for media and controller input.
class MainActivity : ComponentActivity() {
    private lateinit var session: PunchSession

    // Controller state, forwarded as a virtual Pro Controller on the console. Face buttons are
    // mapped by POSITION (Android reports the bottom face button as BUTTON_A even on Nintendo
    // pads): bottom -> Switch B, right -> A, top -> X, left -> Y. HidNpadButton bit order.
    private var padKeys = 0L
    private var padHatKeys = 0L
    private var padTriggerKeys = 0L
    private val padSticks = IntArray(4)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        session = PunchSession(applicationContext)

        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        // Immersive: status/nav bars hide and reappear on an edge swipe.
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val insets = WindowInsetsControllerCompat(window, window.decorView)
        insets.hide(WindowInsetsCompat.Type.systemBars())
        insets.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

        setContent { RemoteLinkApp(session) }
    }

    // Dialogs (the keyboard) can bring the bars back; whenever the app regains focus they are
    // hidden again, and
    // only an edge swipe shows them.
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            WindowInsetsControllerCompat(window, window.decorView)
                .hide(WindowInsetsCompat.Type.systemBars())
            com.nxremoteplay.punchclient.ui.PadInput.refresh()
        }
    }

    // The footer's server-status probe only runs while the app is on screen (see PunchSession).
    override fun onStart() {
        super.onStart()
        session.setForeground(true)
        session.startServerProbe()
    }

    override fun onStop() {
        padKeys = 0
        padHatKeys = 0
        padTriggerKeys = 0
        padSticks.fill(0)
        session.setForeground(false)
        session.stopServerProbe()
        super.onStop()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) session.dispose()
    }

    private fun padBitFor(keyCode: Int): Long =
        when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_B -> 1L shl 0
            KeyEvent.KEYCODE_BUTTON_A -> 1L shl 1
            KeyEvent.KEYCODE_BUTTON_Y -> 1L shl 2
            KeyEvent.KEYCODE_BUTTON_X -> 1L shl 3
            KeyEvent.KEYCODE_BUTTON_THUMBL -> 1L shl 4
            KeyEvent.KEYCODE_BUTTON_THUMBR -> 1L shl 5
            KeyEvent.KEYCODE_BUTTON_L1 -> 1L shl 6
            KeyEvent.KEYCODE_BUTTON_R1 -> 1L shl 7
            KeyEvent.KEYCODE_BUTTON_L2 -> 1L shl 8
            KeyEvent.KEYCODE_BUTTON_R2 -> 1L shl 9
            KeyEvent.KEYCODE_BUTTON_START -> 1L shl 10 // Plus
            KeyEvent.KEYCODE_BUTTON_SELECT -> 1L shl 11 // Minus
            KeyEvent.KEYCODE_DPAD_LEFT -> 1L shl 12
            KeyEvent.KEYCODE_DPAD_UP -> 1L shl 13
            KeyEvent.KEYCODE_DPAD_RIGHT -> 1L shl 14
            KeyEvent.KEYCODE_DPAD_DOWN -> 1L shl 15
            else -> 0L
        }

    private fun isGamepad(source: Int): Boolean =
        (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

    private fun sendPad() {
        session.sendPad(
            padKeys or padHatKeys or padTriggerKeys,
            padSticks[0],
            padSticks[1],
            padSticks[2],
            padSticks[3])
    }

    // The pad is forwarded to the console only while a connection is up; everywhere else (the menus
    // and
    // Settings) it drives the app itself: D-pad moves focus, A presses, B goes back.
    private fun forwardingPad(): Boolean = session.state.value is ConnectState.Connected

    // This overrides Activity's public callback through androidx.activity.ComponentActivity.
    // Lint inherits the library restriction from its androidx.core implementation class.
    @android.annotation.SuppressLint("RestrictedApi")
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (isGamepad(event.source) && event.action == KeyEvent.ACTION_DOWN)
            com.nxremoteplay.punchclient.ui.PadInput.note(event.device)
        if (isGamepad(event.source) &&
            event.action == KeyEvent.ACTION_DOWN &&
            event.repeatCount == 0)
            android.util.Log.i(
                "PadKeys",
                "gamepad key ${KeyEvent.keyCodeToString(event.keyCode)} from ${event.device?.name}")
        // The controller's Guide / PS button (BUTTON_MODE), when the system lets the app see it: it
        // is the console's HOME
        // button while streaming, and never anything else (not the phone's home screen).
        if (isGamepad(event.source) && event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0 && forwardingPad())
                session.sendHomeTap()
            return true
        }
        if (isGamepad(event.source) && !forwardingPad()) {
            when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_A ->
                    return super.dispatchKeyEvent(
                        KeyEvent(
                            event.downTime,
                            event.eventTime,
                            event.action,
                            KeyEvent.KEYCODE_DPAD_CENTER,
                            event.repeatCount,
                            event.metaState,
                            event.deviceId,
                            event.scanCode,
                            event.flags,
                            event.source))
                KeyEvent.KEYCODE_BUTTON_B -> {
                    if (event.action == KeyEvent.ACTION_UP) {
                        val controller = WindowInsetsControllerCompat(window, window.decorView)
                        val imeUp =
                            ViewCompat.getRootWindowInsets(window.decorView)
                                ?.isVisible(WindowInsetsCompat.Type.ime()) == true
                        // B closes the keyboard first, then backs out one screen. It never closes
                        // the app: on the main screen
                        // nothing handles Back, so it does nothing there.
                        if (imeUp) controller.hide(WindowInsetsCompat.Type.ime())
                        else if (onBackPressedDispatcher.hasEnabledCallbacks())
                            onBackPressedDispatcher.onBackPressed()
                    }
                    return true
                }
            }
            return super.dispatchKeyEvent(event)
        }
        if (isGamepad(event.source)) {
            val bit = padBitFor(event.keyCode)
            if (bit != 0L) {
                if (event.repeatCount == 0) {
                    when (event.action) {
                        KeyEvent.ACTION_DOWN -> padKeys = padKeys or bit
                        KeyEvent.ACTION_UP -> padKeys = padKeys and bit.inv()
                    }
                    sendPad()
                }
                return true // swallow, including auto-repeat
            }
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (isGamepad(event.source) && event.action == MotionEvent.ACTION_MOVE && forwardingPad()) {
            fun stick(v: Float): Int {
                val d = if (kotlin.math.abs(v) < 0.08f) 0f else v // small deadzone
                return (d.coerceIn(-1f, 1f) * 32767f).toInt()
            }
            padSticks[0] = stick(event.getAxisValue(MotionEvent.AXIS_X))
            padSticks[1] = -stick(event.getAxisValue(MotionEvent.AXIS_Y)) // Android +Y is down
            padSticks[2] = stick(event.getAxisValue(MotionEvent.AXIS_Z))
            padSticks[3] = -stick(event.getAxisValue(MotionEvent.AXIS_RZ))

            val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
            val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
            padHatKeys =
                (if (hx < -0.5f) 1L shl 12 else 0L) or
                    (if (hy < -0.5f) 1L shl 13 else 0L) or
                    (if (hx > 0.5f) 1L shl 14 else 0L) or
                    (if (hy > 0.5f) 1L shl 15 else 0L)

            val lt =
                maxOf(
                    event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                    event.getAxisValue(MotionEvent.AXIS_BRAKE))
            val rt =
                maxOf(
                    event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                    event.getAxisValue(MotionEvent.AXIS_GAS))
            padTriggerKeys = (if (lt > 0.5f) 1L shl 8 else 0L) or (if (rt > 0.5f) 1L shl 9 else 0L)

            sendPad()
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }
}
