package com.nxremoteplay.punchclient.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import com.nxremoteplay.punchclient.R

// Fonts and colors shared by the app screens.
val Baloo2 =
    FontFamily(
        Font(R.font.baloo2_semibold, FontWeight.SemiBold),
        Font(R.font.baloo2_extrabold, FontWeight.ExtraBold))

object RemoteLinkColors {
    val BgTopLeft = Color(0xFF090619)
    val BgBottomRight = Color(0xFF040611)
    val BgBottomLeft = Color(0xFF1A0517)

    val GlowBlue = Color(0x2914527F)
    val GlowPurple = Color(0x24401A66)
    val GlowTeal = Color(0x241A5766)

    val CardGradientTop = Color(0xFF1A91FA)
    val CardGradientBottom = Color(0xFF2E33C7)
    val CardOutline = Color(0x78B8D6FF)

    val PanelBg = Color(0xF0191F3B)
    val PanelBorder = Color(0x73667AAB)

    val TextPrimary = Color.White
    val TextSubtle = Color(0xFFB8C7EB)
    val TextMuted = Color(0xFF8C99B2)

    val ButtonBlue = Color(0xFF146EF5)

    val SettingsRowBg = Color(0xF5121C38)

    val StatusGreen = Color(0xFF1FE0A3)

    val BackPillBg = Color(0xEB0D1F3B)
    val SettingsPillBg = Color(0xB80A1F3B)
}
