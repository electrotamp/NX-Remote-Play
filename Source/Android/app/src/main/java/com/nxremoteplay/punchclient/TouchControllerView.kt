package com.nxremoteplay.punchclient

import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.min

/**
 * Independent pointer ownership: either a control or the console touchscreen. The menu (hamburger)
 * is a Compose overlay that drives this view through toggleEdit / toggleControls / showOptions.
 * Positions are normalized over the view, so layouts survive aspect/size changes.
 */
class TouchControllerView(
    context: Context,
    private val pad: (Long, Int, Int, Int, Int) -> Unit,
    private val touch: (Boolean, Int, Int) -> Unit,
    // Streams start with the controls hidden (TV and external-controller use); the settings
    // preview passes true.
    startVisible: Boolean = false,
) : View(context) {
    data class Control(
        val id: String,
        val label: String,
        val bit: Int,
        val x: Float,
        val y: Float,
        val stick: Boolean = false
    )

    companion object {
        val controls =
            listOf(
                Control("zl", "ZL", 8, .065f, .08f),
                Control("l", "L", 6, .18f, .08f),
                Control("zr", "ZR", 9, .935f, .08f),
                Control("r", "R", 7, .82f, .08f),
                Control("ls", "L stick", -1, .13f, .42f, true),
                Control("rs", "R stick", -1, .76f, .75f, true),
                Control("up", "↑", 13, .25f, .62f),
                Control("down", "↓", 15, .25f, .86f),
                Control("left", "←", 12, .19f, .74f),
                Control("right", "→", 14, .31f, .74f),
                Control("x", "X", 2, .87f, .30f),
                Control("b", "B", 1, .87f, .56f),
                Control("y", "Y", 3, .80f, .43f),
                Control("a", "A", 0, .94f, .43f),
                Control("minus", "−", 11, .40f, .15f),
                Control("plus", "+", 10, .60f, .15f),
                Control("l3", "L3", 4, .08f, .77f),
                Control("r3", "R3", 5, .93f, .82f),
            )
    }

    private val prefs = context.getSharedPreferences("touch_controller_v1", Context.MODE_PRIVATE)
    private val positions =
        controls
            .associate {
                it.id to Pair(prefs.getFloat(it.id + "x", it.x), prefs.getFloat(it.id + "y", it.y))
            }
            .toMutableMap()
    private val owners = mutableMapOf<Int, String>()
    private val axes = mutableMapOf<String, Pair<Float, Float>>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    @Suppress("DEPRECATION")
    private val vibrator = context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    var editing = false
        private set

    var controlsVisible = startVisible
        private set

    private var buttonVibration = prefs.getBoolean("buttonVibration", true)
    private var stickVibration = prefs.getBoolean("stickVibration", true)
    private var buzzing = false
    private var touchOwner: Int? = null
    private var dragOffset = Pair(0f, 0f)
    private var active = true
    private var dialog: AlertDialog? = null
    private val unit
        get() = min(width / 1280f, height / 720f)

    private fun radius(c: Control) = (if (c.stick) 66f else 32f) * unit

    private fun center(c: Control): Pair<Float, Float> {
        val p = positions.getValue(c.id)
        val r = radius(c)
        return Pair(
            r + p.first.coerceIn(0f, 1f) * (width - 2 * r).coerceAtLeast(1f),
            r + p.second.coerceIn(0f, 1f) * (height - 2 * r).coerceAtLeast(1f))
    }

    init {
        isFocusable = true
        contentDescription = "Switch touch controller"
    }

    override fun onDetachedFromWindow() {
        releaseAll()
        dialog?.dismiss()
        super.onDetachedFromWindow()
    }

    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        active = hasWindowFocus
        if (!hasWindowFocus) releaseAll()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        if (visibility != VISIBLE) releaseAll()
    }

    fun releaseAll() {
        owners.clear()
        axes.clear()
        touchOwner = null
        pad(0, 0, 0, 0, 0)
        touch(false, 0, 0)
        vibrator?.cancel()
        buzzing = false
        invalidate()
    }

    fun setEditMode(value: Boolean) {
        releaseAll()
        editing = value
        controlsVisible = true
        invalidate()
    }

    fun toggleEdit() {
        if (editing) savePositions()
        setEditMode(!editing)
    }

    fun toggleControls() {
        releaseAll()
        controlsVisible = !controlsVisible
        editing = false
        invalidate()
    }

    private fun savePositions() {
        val e = prefs.edit()
        positions.forEach { (id, p) -> e.putFloat(id + "x", p.first).putFloat(id + "y", p.second) }
        e.apply()
    }

    private fun vibrateButton() {
        if (!buttonVibration || vibrator?.hasVibrator() != true) return
        if (Build.VERSION.SDK_INT >= 26 && buzzing)
            vibrator.vibrate(
                VibrationEffect.createWaveform(longArrayOf(0, 22, 1000), intArrayOf(0, 160, 70), 2))
        else if (Build.VERSION.SDK_INT >= 26)
            vibrator.vibrate(VibrationEffect.createOneShot(22, VibrationEffect.DEFAULT_AMPLITUDE))
        else {
            @Suppress("DEPRECATION") vibrator.vibrate(22)
        }
    }

    private fun updateBuzz() {
        val wanted =
            active &&
                !editing &&
                stickVibration &&
                axes.values.any { hypot(it.first, it.second) > .08f }
        if (wanted == buzzing) return
        buzzing = wanted
        if (!wanted) vibrator?.cancel()
        else if (vibrator?.hasVibrator() == true) {
            if (Build.VERSION.SDK_INT >= 26)
                vibrator.vibrate(
                    VibrationEffect.createWaveform(longArrayOf(0, 1000), intArrayOf(0, 70), 1))
            else {
                @Suppress("DEPRECATION") vibrator.vibrate(longArrayOf(0, 1000), 1)
            }
        }
    }

    private fun sendState() {
        var keys = 0L
        controls
            .filter { it.bit >= 0 && it.id in owners.values }
            .forEach { keys = keys or (1L shl it.bit) }
        fun axis(id: String, y: Boolean): Int {
            val p = axes[id] ?: return 0
            val v = if (y) -p.second else p.first
            return (v * 32767).toInt().coerceIn(-32767, 32767)
        }
        pad(keys, axis("ls", false), axis("ls", true), axis("rs", false), axis("rs", true))
        updateBuzz()
        invalidate()
    }

    private fun movePointer(id: Int, x: Float, y: Float) {
        val c = controls.find { it.id == owners[id] } ?: return
        val r = radius(c)
        if (editing) {
            positions[c.id] =
                Pair(
                    ((x - dragOffset.first - r) / (width - 2 * r).coerceAtLeast(1f)).coerceIn(
                        0f, 1f),
                    ((y - dragOffset.second - r) / (height - 2 * r).coerceAtLeast(1f)).coerceIn(
                        0f, 1f))
            invalidate()
        } else if (c.stick) {
            val p = center(c)
            var dx = (x - p.first) / r
            var dy = (y - p.second) / r
            val length = hypot(dx, dy)
            if (length > 1f) {
                dx /= length
                dy /= length
            }
            if (length < .08f) {
                dx = 0f
                dy = 0f
            }
            axes[c.id] = Pair(dx, dy)
        }
    }

    private fun sendScreenTouch(x: Float, y: Float) {
        val scale = min(width / 1280f, height / 720f)
        touch(
            true,
            ((x - (width - 1280 * scale) / 2) / scale).toInt().coerceIn(0, 1279),
            ((y - (height - 720 * scale) / 2) / scale).toInt().coerceIn(0, 719))
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val i = event.actionIndex
        val id = event.getPointerId(i)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> {
                val x = event.getX(i)
                val y = event.getY(i)
                val c =
                    if (controlsVisible)
                        controls.asReversed().firstOrNull {
                            val p = center(it)
                            hypot(x - p.first, y - p.second) <= radius(it) &&
                                it.id !in owners.values
                        }
                    else null
                if (c != null) {
                    if (!editing || owners.isEmpty()) {
                        owners[id] = c.id
                        val p = center(c)
                        dragOffset = Pair(x - p.first, y - p.second)
                        if (!editing) {
                            if (!c.stick) vibrateButton()
                            movePointer(id, x, y)
                            sendState()
                        }
                    }
                } else if (!editing && touchOwner == null) {
                    touchOwner = id
                    sendScreenTouch(x, y)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                for (index in 0 until event.pointerCount) {
                    val pointer = event.getPointerId(index)
                    movePointer(pointer, event.getX(index), event.getY(index))
                    if (pointer == touchOwner) sendScreenTouch(event.getX(index), event.getY(index))
                }
                if (!editing) sendState()
            }
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP -> {
                owners.remove(id)?.let { axes.remove(it) }
                if (id == touchOwner) {
                    touchOwner = null
                    touch(false, 0, 0)
                }
                if (editing) {
                    savePositions()
                    invalidate()
                } else sendState()
                performClick()
            }
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    fun showOptions() {
        releaseAll()
        dialog =
            AlertDialog.Builder(context)
                .setTitle("Touch controller")
                .setMultiChoiceItems(
                    arrayOf("Button vibration", "Continuous stick vibration"),
                    booleanArrayOf(buttonVibration, stickVibration)) { _, which, checked ->
                        if (which == 0) buttonVibration = checked else stickVibration = checked
                        prefs
                            .edit()
                            .putBoolean("buttonVibration", buttonVibration)
                            .putBoolean("stickVibration", stickVibration)
                            .apply()
                        updateBuzz()
                    }
                .setNeutralButton("Reset positions") { _, _ ->
                    controls.forEach { positions[it.id] = Pair(it.x, it.y) }
                    savePositions()
                    invalidate()
                }
                .setPositiveButton("Done", null)
                .create()
        dialog?.show()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (!controlsVisible) return
        if (editing) {
            paint.color = Color.argb(45, 70, 190, 240)
            for (x in 0..16) canvas.drawLine(
                width * x / 16f, 0f, width * x / 16f, height.toFloat(), paint)
            drawLabel(
                canvas,
                "Drag any control • Save layout when finished",
                width / 2f,
                height * .48f,
                22 * unit)
        }
        controls.forEach { c ->
            val p = center(c)
            val r = radius(c)
            val held = c.id in owners.values
            paint.style = Paint.Style.FILL
            paint.color = if (held) Color.argb(165, 34, 164, 220) else Color.argb(100, 12, 23, 40)
            canvas.drawCircle(p.first, p.second, r, paint)
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = 2 * unit
            paint.color = if (editing) Color.CYAN else Color.argb(190, 235, 244, 255)
            canvas.drawCircle(p.first, p.second, r, paint)
            paint.style = Paint.Style.FILL
            if (c.stick) {
                val a = axes[c.id] ?: Pair(0f, 0f)
                paint.color = Color.argb(145, 210, 230, 245)
                canvas.drawCircle(
                    p.first + a.first * r * .65f, p.second + a.second * r * .65f, r * .34f, paint)
                drawLabel(canvas, c.label, p.first, p.second + r * .65f, 17 * unit)
            } else
                drawLabel(
                    canvas, c.label, p.first, p.second, (if (c.label.length > 2) 17 else 26) * unit)
        }
    }

    private fun drawLabel(canvas: Canvas, text: String, x: Float, y: Float, size: Float) {
        paint.style = Paint.Style.FILL
        paint.color = Color.WHITE
        paint.textSize = size
        paint.textAlign = Paint.Align.CENTER
        paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
        canvas.drawText(text, x, y - (paint.ascent() + paint.descent()) / 2, paint)
    }
}
