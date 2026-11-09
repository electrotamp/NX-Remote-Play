#pragma once

#include <string>
#include <cstdint>
#include <string_view>
#include <cstring>
#include <cctype>
#include <algorithm>

#include "../UI/UI.hpp"
#include "../UI/Image.hpp"
#include "../Platform/Platform.hpp"

#include <switch.h>

// Shared from main.cpp
extern Image::Img NxRemotePlayLogo;

// Shared panel dimensions for the fixed 1280x720 scene canvas.
namespace ScreenPanel
{
constexpr float Padding = 147.0f;
constexpr float PosX = Padding;
constexpr float Width = UI::WindowWidth - Padding * 2.0f;
// Settings uses this height; Connect, Join and Host size their panels independently.
constexpr float Height = 528.0f;

// Content coordinates are relative to the panel child window.
constexpr float ContentPadding = 20.0f;
constexpr float ContentWidth = Width - ContentPadding * 2.0f;
constexpr float ContentBottom = Height - ContentPadding;
} // namespace ScreenPanel

static inline void CenterImage(const Image::Img &img, float imageScale = 1)
{
    auto win = ImGui::GetWindowWidth();
    ImGui::SetCursorPosX(win / 2 - img.Size().x * imageScale / 2);
    ImGui::Image(img, img.Size() * imageScale);
}

static inline void CenterText(const std::string_view text)
{
    auto win = ImGui::GetWindowWidth();
    auto sz = ImGui::CalcTextSize(text.data(), text.data() + text.size(), false, win);

    ImGui::SetCursorPosX(win / 2 - sz.x / 2);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

static void SetupMainWindow(const char *name, float padding = 0)
{
    ImGui::SetNextWindowSize({UI::WindowWidth - padding, UI::WindowHeight});
    ImGui::SetNextWindowPos({(UI::WindowWidth - (UI::WindowWidth - padding)) / 2, 0});

    ImGui::Begin(name, 0,
                 (ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize) &
                     ~ImGuiWindowFlags_NoScrollbar);
}

// Activate the system software keyboard for a focused field on Switch.
// Desktop and browser previews use their normal keyboard input. Apply uppercase
// after swkbd returns because its result bypasses ImGui character filtering.
static inline bool InputTextWithKeyboard(const char *id, char *buf, size_t bufSize, const char *headerText,
                                         bool uppercase, ImGuiInputTextFlags flags = 0)
{
    bool changed = ImGui::InputText(id, buf, bufSize, flags);

    // Every other focusable control in this app (buttons, the back arrow)
    // draws its own accent-colored outline on hover/focus - plain
    // InputText() draws none, which reads as "the dpad/stick can't reach
    // this field at all" even when nav focus has genuinely moved onto it
    // (it has; ImGuiConfigFlags_NavEnableGamepad is on and InputText is a
    // perfectly normal navigable item - it just never got this app's own
    // highlight treatment). Captured now, drawn after the swkbd handling
    // below so it doesn't depend on what ClearActiveID() does to focus.
    const bool focused = ImGui::IsItemFocused();
    const bool selected = ImGui::IsItemHovered() || focused;
    const ImVec2 fieldPos = ImGui::GetItemRectMin(), fieldSize = ImGui::GetItemRectSize();

    // IsItemActivated() alone is not enough: this vendored ImGui's own
    // InputTextEx only starts text-edit mode from nav-activation when the
    // nav input source is a real keyboard (imgui_widgets.cpp -
    // user_nav_input_start explicitly requires
    // ImGuiInputSource_NavKeyboard) - a gamepad's Activate/A button on a
    // nav-focused InputText does NOT set IsItemActivated() at all in this
    // version, on any platform. D-pad/stick movement correctly lands nav
    // focus on the field either way (that part already worked); pressing
    // A on it just silently did nothing. Checked directly instead: this
    // field is nav-focused AND Activate was just pressed this frame.
    const bool gamepadOpen = focused && ImGui::IsNavInputPressed(ImGuiNavInput_Activate, ImGuiInputReadMode_Pressed);
    if (ImGui::IsItemActivated() || gamepadOpen)
    {
        SwkbdConfig kbd;
        if (R_SUCCEEDED(swkbdCreate(&kbd, 0)))
        {
            swkbdConfigMakePresetDefault(&kbd);
            swkbdConfigSetType(&kbd, SwkbdType_QWERTY);
            swkbdConfigSetHeaderText(&kbd, headerText);
            swkbdConfigSetInitialText(&kbd, buf);
            swkbdConfigSetStringLenMax(&kbd, (u32)(bufSize - 1));

            char result[64] = {};
            if (R_SUCCEEDED(swkbdShow(&kbd, result, sizeof(result))))
            {
                if (uppercase)
                    for (char *p = result; *p; p++)
                        *p = (char)toupper((unsigned char)*p);

                const size_t copyLen = std::min(std::strlen(result), bufSize - 1);
                std::memset(buf, 0, bufSize);
                std::memcpy(buf, result, copyLen);
                changed = true;
            }

            swkbdClose(&kbd);
        }

        // Forces InputText() to re-read buf fresh next frame instead of
        // continuing to edit its own cached copy from before swkbd ran.
        ImGui::ClearActiveID();
    }

    // Matches style.FrameRounding (UI.cpp) and the Connect button's own
    // accent color (SceneHost.cpp) so a highlighted field reads as part of
    // the same visual language as every other focusable control.
    if (selected)
        ImGui::GetWindowDrawList()->AddRect(fieldPos, fieldPos + fieldSize, IM_COL32(107, 194, 255, 255), 12.0f,
                                            ImDrawCornerFlags_All, 2.0f);

    return changed;
}

template <typename T, size_t N> static inline int ImGuiCenterButtons(T (&&buttons)[N])
{
    const auto str = [&buttons](size_t i) -> const char * {
        if constexpr (std::is_same<T, std::string>())
            return buttons[i].c_str();
        else if constexpr (std::is_same<T, std::string_view>())
            return buttons[i].data();
        else if constexpr (std::is_same<T, const char *>())
            return buttons[i];
    };

    const ImGuiStyle &style = GImGui->Style;
    auto win = ImGui::GetWindowWidth();
    float TotX = 0;
    for (size_t i = 0; i < N; i++)
    {
        auto sz = ImGui::CalcTextSize(str(i), nullptr, false, win);
        TotX += ImGui::CalcItemSize({}, sz.x + style.FramePadding.x * 2.0f, sz.y + style.FramePadding.y * 2.0f).x;
    }
    ImGui::SetCursorPosX((win / 2 - TotX / 2) - (N - 1) * style.FramePadding.x * 2);
    int res = -1;
    for (size_t i = 0; i < N; i++)
    {
        if (ImGui::Button(str(i)))
            res = i;
        if (i != N - 1)
            ImGui::SameLine();
    }
    return res;
}
