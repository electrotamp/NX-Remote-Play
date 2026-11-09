#include "Scenes.hpp"
#include "Common.hpp"
#include "../Network/RendezvousClient.hpp"
#include "../Network/ConnectorMedia.hpp"
#include "../Network/LogExport.hpp"
#include "../Network/UpdateCheck.hpp"
#include "../ipc.h"
#include "../HostProtocol/server_config.h"
#include "../Platform/fs.hpp"

#include <cstdlib>
#include <cstdio>
#include <cfloat>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

// PadState/HidNpadButton_*/padGetStickPos for RenderConnectorPlayback()'s
// controller-passthrough combo (real Switch NRO build only, no libnx build
// for either preview).
#include <switch.h>

namespace
{
enum class Page
{
    Home,
    Connect,
    Host,
    Settings,
    SystemPatches,
    ConnectorPlayback,
    Server,
    ExportLogs,
    Updates
};

Page currentPage = Page::Home;
char connectSessionId[6] = {};
char connectPassword[6] = {};
int settingsFocus = 0;
bool settingsUpHeld = false;
bool settingsDownHeld = false;
bool settingsActivateHeld = false;
bool settingsFocusDirty = false;
bool settingsControllerMode = false;
bool serverPageFresh = true;
// Host screen's IPC session to the sysmodule - held open for as long as the
// Host screen is on-screen instead of reconnecting every single frame (see
// RenderHost()/CloseHostingConnection() below). A fresh smGetService() every
// frame was hammering the local IPC server at ~60 connects/sec (visible as
// nonstop "client connected"/"client sent Close" spam in a UDP debug log)
// and starving the render loop enough to visibly lag hover/focus updates.
bool g_hostingConnected = false;
// The footer shows what the SYSMODULE is doing (NRO and sysmodule must communicate directly and
// reflect the same server/state). -1 = sysmodule not reachable over IPC, else the HostingState value it reports.
int g_footerRegState = -1;
double g_footerLastPoll = -1000.0;
Image::Img hostThisSwitchIcon;
Image::Img connectJoyConIcon;
Image::Img appLogo;

struct SettingItem
{
    const char *label;
    const char *value;
};
struct SettingGroup
{
    const char *name;
    const SettingItem *items;
    int count;
};

// The server Settings row opens Page::Server (RenderServer).
// Its value text is the CURRENT server ("host:port"), kept in this buffer: filled from the
// sysmodule at startup (InitHost) and updated when Save succeeds on the Server page.
char g_serverDisplay[64] = "...";
constexpr SettingItem SettingsConnection[] = {
    {"Rendezvous server", g_serverDisplay},
};
constexpr SettingItem SettingsActions[] = {
    {"System patches", ">"},
    {"Export logs", ">"},
};
// The Updates row shows the latest update check's result (refreshed each frame by RenderSettings).
char g_updateDisplay[64] = "Not checked";
constexpr SettingItem SettingsAbout[] = {
    {"Updates", g_updateDisplay},
};
constexpr SettingGroup SettingsGroups[] = {
    {"Connection", SettingsConnection, sizeof(SettingsConnection) / sizeof(SettingItem)},
    {"Actions", SettingsActions, sizeof(SettingsActions) / sizeof(SettingItem)},
    {"About", SettingsAbout, sizeof(SettingsAbout) / sizeof(SettingItem)},
};
constexpr int SettingsGroupCount = sizeof(SettingsGroups) / sizeof(SettingGroup);

// Row heights and gaps are doubled right alongside the text (96/72 vs the original
// 48/36, etc.) - keeping the exact same ratio to font size as before, so a much
// bigger label still sits inside a proportionally bigger row instead of overflowing it.
constexpr float SettingsParentHeight = 76.8f, SettingsDetailHeight = 57.6f, SettingsHeaderGap = 12.8f,
                SettingsRowGap = 6.4f, SettingsGroupGap = 19.2f;
constexpr float SettingsGroupNameSize = 46.4f, SettingsItemTextSize = 35.2f;

int SettingsTotalCount()
{
    int total = 0;
    for (int i = 0; i < SettingsGroupCount; ++i)
        total += SettingsGroups[i].count;
    return total;
}

float SettingsLayoutHeight()
{
    float y = 0.0f;
    for (int g = 0; g < SettingsGroupCount; ++g)
        y += SettingsParentHeight + SettingsHeaderGap +
             SettingsGroups[g].count * (SettingsDetailHeight + SettingsRowGap) + SettingsGroupGap;
    return y;
}

// Finds the top Y of the control at the given flattened index, walking the same
// layout the render loop below uses.
float SettingsRowY(int targetControlIndex)
{
    float y = 0.0f;
    int idx = 0;
    for (int g = 0; g < SettingsGroupCount; ++g)
    {
        y += SettingsParentHeight + SettingsHeaderGap;
        for (int o = 0; o < SettingsGroups[g].count; ++o)
        {
            if (idx == targetControlIndex)
                return y;
            y += SettingsDetailHeight + SettingsRowGap;
            ++idx;
        }
        y += SettingsGroupGap;
    }
    return y;
}

// The system patches ship as plain files in
// the NX-Remote-Play package; the status line reports whether any are on the SD card.
constexpr const char *SystemPatchesDescription =
    "System patches that allow most games that normally cannot be streamed to be streamed with NX-Remote-Play. A few games are reported to crash with them. They are installed with the NX-Remote-Play files and take effect after a reboot. To turn them off, remove the patch files from the folder below.";
constexpr const char *SystemPatchesSdcardPath = "Sdcard path: /atmosphere/exefs_patches/am/";

// Number of patch files on the SD card, counted when the screen is opened.
int systemPatchFileCount = 0;
void RefreshSystemPatchStatus()
{
    systemPatchFileCount = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(SDMC "/atmosphere/exefs_patches/am/", ec), end; !ec && it != end;
         it.increment(ec))
        if (it->path().extension() == ".ips")
            ++systemPatchFileCount;
}

constexpr ImU32 Color(float r, float g, float b, float a = 1.0f)
{
    return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255),
                    static_cast<int>(a * 255));
}

ImVec2 TextSize(const char *text, float size, bool bold = false)
{
    ImFont *font = bold ? UI::BoldTypeface() : ImGui::GetFont();
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void Text(ImDrawList *draw, ImVec2 pos, float size, ImU32 color, const char *value, bool bold = false)
{
    ImFont *font = bold ? UI::BoldTypeface() : ImGui::GetFont();
    draw->AddText(font, size, pos, color, value);
}

ImVec4 Blend(const ImVec4 &topLeft, const ImVec4 &topRight, const ImVec4 &bottomRight, const ImVec4 &bottomLeft,
             float x, float y)
{
    const float invX = 1.0f - x;
    const float invY = 1.0f - y;
    return {
        topLeft.x * invX * invY + topRight.x * x * invY + bottomRight.x * x * y + bottomLeft.x * invX * y,
        topLeft.y * invX * invY + topRight.y * x * invY + bottomRight.y * x * y + bottomLeft.y * invX * y,
        topLeft.z * invX * invY + topRight.z * x * invY + bottomRight.z * x * y + bottomLeft.z * invX * y,
        topLeft.w * invX * invY + topRight.w * x * invY + bottomRight.w * x * y + bottomLeft.w * invX * y,
    };
}

void RoundedGradient(ImDrawList *draw, ImVec2 min, ImVec2 max, float radius, const ImVec4 &topLeft,
                     const ImVec4 &topRight, const ImVec4 &bottomRight, const ImVec4 &bottomLeft)
{
    // Match the desktop-client card tessellation so rounded gradient corners
    // remain smooth on the Switch, browser, and native preview renderers.
    constexpr int segments = 256;
    constexpr float pi = 3.14159265f;
    ImVec2 points[segments * 4 + 4];
    int count = 0;
    const ImVec2 centers[] = {{min.x + radius, min.y + radius},
                              {max.x - radius, min.y + radius},
                              {max.x - radius, max.y - radius},
                              {min.x + radius, max.y - radius}};
    for (int corner = 0; corner < 4; ++corner)
    {
        for (int i = 0; i <= segments; ++i)
        {
            if (corner && i == 0)
                continue;
            const float angle = pi + corner * pi * .5f + i * pi / (2 * segments);
            points[count++] = centers[corner] + ImVec2(std::cos(angle), std::sin(angle)) * radius;
        }
    }
    const ImVec2 center = (min + max) * .5f;
    draw->PrimReserve(count * 3, count + 1);
    const unsigned int centerIndex = draw->_VtxCurrentIdx;
    draw->PrimWriteVtx(center, draw->_Data->TexUvWhitePixel,
                       Color(Blend(topLeft, topRight, bottomRight, bottomLeft, .5f, .5f).x,
                             Blend(topLeft, topRight, bottomRight, bottomLeft, .5f, .5f).y,
                             Blend(topLeft, topRight, bottomRight, bottomLeft, .5f, .5f).z,
                             Blend(topLeft, topRight, bottomRight, bottomLeft, .5f, .5f).w));
    for (int i = 0; i < count; ++i)
    {
        const float x = (points[i].x - min.x) / (max.x - min.x);
        const float y = (points[i].y - min.y) / (max.y - min.y);
        const ImVec4 color = Blend(topLeft, topRight, bottomRight, bottomLeft, x, y);
        draw->PrimWriteVtx(points[i], draw->_Data->TexUvWhitePixel, Color(color.x, color.y, color.z, color.w));
    }
    for (int i = 0; i < count; ++i)
    {
        draw->PrimWriteIdx(centerIndex);
        draw->PrimWriteIdx(centerIndex + 1 + i);
        draw->PrimWriteIdx(centerIndex + 1 + ((i + 1) % count));
    }
}

void RoundedOutline(ImDrawList *draw, ImVec2 min, ImVec2 max, float radius, ImU32 color, float thickness)
{
    constexpr int segments = 256;
    constexpr float pi = 3.14159265f;
    ImVec2 points[segments * 4 + 4];
    int count = 0;
    const ImVec2 centers[] = {{min.x + radius, min.y + radius},
                              {max.x - radius, min.y + radius},
                              {max.x - radius, max.y - radius},
                              {min.x + radius, max.y - radius}};
    for (int corner = 0; corner < 4; ++corner)
        for (int i = 0; i <= segments; ++i)
        {
            if (corner && i == 0)
                continue;
            const float angle = pi + corner * pi * .5f + i * pi / (2 * segments);
            points[count++] = centers[corner] + ImVec2(std::cos(angle), std::sin(angle)) * radius;
        }
    draw->AddPolyline(points, count, color, true, thickness);
}

void DrawBackground()
{
    const ImVec2 topLeft = ImGui::GetWindowPos();
    const ImVec2 bottomRight = topLeft + ImGui::GetWindowSize();
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(topLeft, bottomRight, Color(.035f, .025f, .085f), Color(.025f, .075f, .15f),
                                  Color(.015f, .025f, .065f), Color(.10f, .018f, .09f));
    draw->AddCircleFilled(topLeft + ImVec2(150, 90), 245, Color(.08f, .32f, .52f, .16f), 64);
    draw->AddCircleFilled(topLeft + ImVec2(1125, 120), 290, Color(.25f, .10f, .40f, .14f), 64);
    draw->AddCircleFilled(topLeft + ImVec2(170, 710), 300, Color(.10f, .34f, .40f, .14f), 64);
}

void DrawBrand()
{
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    // Real app logo (user-supplied logo, staged via
    // romfs/app_logo.png), replacing the old two-outlined-rectangles mark,
    // and the subtitle ("Connect. Play. Control.") dropped entirely -
    //  design request, matching the same change made to the
    // Android client and the desktop client (DesktopClientScene.cpp).
    const ImVec2 logoSize(39.6f, 44);
    const ImVec2 logoPos = origin + ImVec2(54, 40);
    draw->AddImage(appLogo, logoPos, logoPos + logoSize);
    const ImVec2 labelSize = TextSize("NX Remote Play", 48, true);
    Text(draw, origin + ImVec2(116, 40 + logoSize.y / 2 - labelSize.y / 2), 48, Color(1, 1, 1), "NX Remote Play", true);
}

bool BackButton()
{
    auto *draw = ImGui::GetWindowDrawList();
    ImGui::SetCursorPos({47, 31});
    const bool pressed = ImGui::InvisibleButton("back", {54, 54});
    const ImVec2 p = ImGui::GetItemRectMin();
    const bool settingsPage = currentPage == Page::Settings;
    const bool selected =
        (settingsPage && settingsControllerMode)
            ? settingsFocus == -1
            : (ImGui::IsItemHovered() || (settingsPage ? settingsFocus == -1 : ImGui::IsItemFocused()));
    draw->AddCircleFilled(p + ImVec2(27, 27), 27, selected ? Color(.19f, .28f, .43f, 1) : Color(.05f, .12f, .23f, .92f),
                          256);
    if (selected)
        draw->AddCircle(p + ImVec2(27, 27), 27, Color(.42f, .76f, 1), 256, 2);
    draw->AddLine(p + ImVec2(30, 18), p + ImVec2(21, 27), Color(.95f, .97f, 1), 3);
    draw->AddLine(p + ImVec2(21, 27), p + ImVec2(30, 36), Color(.95f, .97f, 1), 3);
    return settingsPage ? ImGui::IsItemClicked(0) : pressed;
}

int UpdateSettingsController()
{
    ImGuiIO &io = ImGui::GetIO();
    const int previousFocus = settingsFocus;
    // Read the physical stick state directly.  The setting list owns navigation,
    // so it must not depend on ImGui's transient nav-input bookkeeping.
    const float leftY = g_gamepad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
    const float rightY = g_gamepad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
    const bool up = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] || leftY < -0.20f || rightY < -0.20f;
    const bool down = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] || leftY > 0.20f || rightY > 0.20f;
    const bool activate = io.NavInputs[ImGuiNavInput_Activate] > 0.5f;
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
        settingsControllerMode = false;
    if (up && !settingsUpHeld)
    {
        settingsControllerMode = true;
        if (settingsFocus > -1)
            --settingsFocus;
    }
    if (down && !settingsDownHeld)
    {
        settingsControllerMode = true;
        if (settingsFocus < SettingsTotalCount() - 1)
            ++settingsFocus;
    }
    settingsUpHeld = up;
    settingsDownHeld = down;
    if (settingsFocus != previousFocus)
        settingsFocusDirty = true;
    const int activated = activate && !settingsActivateHeld ? settingsFocus : -2;
    settingsActivateHeld = activate;
    return activated;
}

void DrawNavigationBrand()
{
    const ImVec2 labelSize = TextSize("NX Remote Play", 48, true);
    Text(ImGui::GetWindowDrawList(), ImGui::GetWindowPos() + ImVec2(117, 58 - labelSize.y / 2), 48, Color(1, 1, 1),
         "NX Remote Play", true);
}

void Footer(bool showSettings)
{
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    if (showSettings)
    {
        const char *label = "Settings";
        const ImVec2 labelSize = TextSize(label, 36, true);
        // A close-fitting capsule retains a comfortable target without giving the
        // left footer a pill that outweighs the right-hand status controls.
        const ImVec2 buttonSize = ImVec2(labelSize.x + 24, 42);
        ImGui::SetCursorPos({71, 662});
        const bool pressed = ImGui::InvisibleButton("settings", buttonSize);
        const ImVec2 p = ImGui::GetItemRectMin();
        const bool selected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        draw->AddRectFilled(p, p + buttonSize, selected ? Color(.19f, .28f, .43f, 1) : Color(.04f, .12f, .23f, .72f),
                            21);
        if (selected)
            draw->AddRect(p, p + buttonSize, Color(.42f, .76f, 1), 21, ImDrawCornerFlags_All, 2);
        Text(draw, p + ImVec2(12, (42 - labelSize.y) / 2), 36, selected ? Color(.70f, .88f, 1) : Color(.96f, .97f, 1),
             label, true);
        if (pressed)
            currentPage = Page::Settings;
    }
    // Rendezvous-server reachability status (, replacing the old
    // hardcoded "Desktop mockup" placeholder) - added directly after a real
    // support case where "Connect" appeared to silently do nothing and took a
    // long manual log-diving session to trace to the PC's Rendezvous.exe
    // simply not running. "Rendezvous" (not "Relay") deliberately matches
    // this project's own protocol/namespace naming (RendezvousServer.cs,
    // Network::Rendezvous) - it's genuinely just a signaling handshake, never
    // a media relay, so "relay" would be actively misleading here.
    // Source of truth = the sysmodule's own registration state over IPC (every 2 s, short session; while the Host
    // page holds its long session that page keeps g_footerRegState current). Only when the sysmodule cannot be
    // reached does the NRO fall back to its own independent probe of the server.
    if (!g_hostingConnected && (ImGui::GetTime() - g_footerLastPoll) >= 2.0)
    {
        g_footerLastPoll = ImGui::GetTime();
        int st = -1;
        if (R_SUCCEEDED(NxRemotePlayConnect()))
        {
            HostingSnapshot fs = {};
            if (R_SUCCEEDED(NxRemotePlayGetHostingState(&fs)))
                st = (int)fs.state;
            NxRemotePlayClose();
        }
        g_footerRegState = st;
    }
    bool serverReachable;
    const char *status;
    if (g_footerRegState < 0)
    {
        serverReachable = Network::Rendezvous::IsServerReachable();
        status = serverReachable ? "Rendezvous connected" : "Rendezvous offline";
    }
    else if (g_footerRegState == HostingState_Registered)
    {
        serverReachable = true;
        status = "Rendezvous connected";
    }
    else if (g_footerRegState == HostingState_Disabled)
    {
        serverReachable = false;
        status = "Hosting off";
    }
    else if (g_footerRegState == HostingState_Error)
    {
        serverReachable = false;
        status = "Rendezvous offline";
    }
    else
    {
        serverReachable = false;
        status = "Connecting...";
    }
    const char *hints = "B Back    A Select";
    constexpr float statusFontSize = 24.0f;
    const ImVec2 hintSize = TextSize(hints, 32);
    const ImVec2 statusSize = TextSize(status, statusFontSize);
    constexpr float footerCenterY = 683.0f;
    constexpr float statusStartX = 868.0f;
    const ImU32 dotColor = serverReachable ? Color(.12f, .88f, .64f) : Color(.94f, .27f, .27f);
    draw->AddCircleFilled(origin + ImVec2(846, footerCenterY), 8, dotColor);
    Text(draw, origin + ImVec2(statusStartX, footerCenterY - statusSize.y / 2), statusFontSize, Color(.86f, .89f, .96f),
         status);
    // Right-aligned to 1230 normally, same as before - but never allowed to
    // start before the status text actually ends (plus a small gap), so a
    // longer status string can never overlap it regardless of exact font
    // metrics on real hardware (deliberately not trusted to just "probably
    // fit" without being able to see it rendered before this ships).
    const float hintsX = std::max(1230.0f - hintSize.x, statusStartX + statusSize.x + 24.0f);
    Text(draw, origin + ImVec2(hintsX, footerCenterY - 16), 32, Color(.86f, .89f, .96f), hints);
}

bool HomeCard(const char *id, ImVec2 pos, ImVec2 size, ImU32 left, ImU32 right, const char *firstTitleLine,
              const char *secondTitleLine, bool host, ImVec2 iconNativeSize, float titleFontSize,
              float sharedIconCenterY, float sharedTitleTopY)
{
    ImGui::SetCursorPos(pos);
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("card", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const ImVec2 cardPos = ImGui::GetItemRectMin();
    ImGui::PopID();

    auto *draw = ImGui::GetWindowDrawList();
    const ImU32 border = (hovered || focused) ? Color(.42f, .76f, 1.0f) : Color(.72f, .80f, 1.0f, .35f);
    const ImVec4 leftColor = host ? ImVec4(.25f, .66f, .58f, 1) : ImVec4(.08f, .55f, 1, 1);
    const ImVec4 rightColor = host ? ImVec4(.43f, .20f, .78f, 1) : ImVec4(.20f, .25f, .92f, 1);
    RoundedGradient(draw, cardPos, cardPos + size, 28, leftColor, rightColor, rightColor, leftColor);
    RoundedOutline(draw, cardPos, cardPos + size, 28, border, (hovered || focused) ? 4 : 2);

    const ImVec2 firstTitleSize = TextSize(firstTitleLine, titleFontSize, true);
    const ImVec2 secondTitleSize = secondTitleLine ? TextSize(secondTitleLine, titleFontSize, true) : ImVec2(0, 0);
    constexpr float titleLineGap = 4.0f;

    // Both home-card icons are scaled (preserving their own art's aspect ratio) to fit
    // inside the same square footprint, so neither icon reads as bigger than the other.
    constexpr float iconBoxSize = 82.0f;
    const float iconLongestSide = iconNativeSize.x > iconNativeSize.y ? iconNativeSize.x : iconNativeSize.y;
    const ImVec2 iconSize = iconNativeSize * (iconBoxSize / iconLongestSide);

    // Icon and title rows share one Y position across both home cards (anchored to the
    // Connect card's own layout) so they line up even though icon art differs per card.
    const ImVec2 iconPos = cardPos + ImVec2((size.x - iconSize.x) / 2, sharedIconCenterY - iconSize.y / 2);

    if (host)
    {
        draw->AddImage(hostThisSwitchIcon, iconPos, iconPos + iconSize);
    }
    else
    {
        draw->AddImage(connectJoyConIcon, iconPos, iconPos + iconSize);
    }
    const float firstTitleY = sharedTitleTopY;
    Text(draw, cardPos + ImVec2((size.x - firstTitleSize.x) / 2, firstTitleY), titleFontSize, Color(1, 1, 1),
         firstTitleLine, true);
    if (secondTitleLine)
        Text(draw, cardPos + ImVec2((size.x - secondTitleSize.x) / 2, firstTitleY + firstTitleSize.y + titleLineGap),
             titleFontSize, Color(1, 1, 1), secondTitleLine, true);
    return pressed;
}

void DrawPanel(ImVec2 pos, ImVec2 size)
{
    auto *parentDraw = ImGui::GetWindowDrawList();
    const ImVec2 panelPos = ImGui::GetWindowPos() + pos;
    parentDraw->AddRectFilled(panelPos, panelPos + size, Color(.10f, .14f, .23f, .94f), 28);
    parentDraw->AddRect(panelPos, panelPos + size, Color(.40f, .48f, .67f, .45f), 28, ImDrawCornerFlags_All, 2);
    ImGui::SetCursorPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    // NavFlattened is required for D-pad/gamepad focus to cross from the main
    // window into this child at all - without it, ImGui::IsItemFocused() on
    // any widget inside (Connect's Connect button, Host's Refresh/Turn Off/
    // Turn On Hosting buttons) never becomes true via real ImGui Nav, since a
    // plain BeginChild() is its own separate nav scope that directional input
    // doesn't automatically enter. Safe here specifically because this panel
    // is always sized to fit its content exactly (no overflow/scrolling) -
    // the flag's own doc comment requires that.
    ImGui::BeginChild("Panel", size, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NavFlattened);
}

void EndPanel()
{
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RenderHome()
{
    DrawBrand();
    // Both home cards share one title font size, one icon square footprint, and one
    // icon-row/title-row Y position (all anchored to the Connect card's own layout)
    // so the two cards read as visually even despite using different icon art.
    constexpr float cardHeight = 350.0f, iconTitleGap = 21.0f, titleLineGap = 4.0f;
    constexpr float titleFontSize = 74.0f;
    constexpr float iconBoxSize = 82.0f;
    const ImVec2 connectFirstTitleSize = TextSize("Connect To", titleFontSize, true);
    const ImVec2 connectSecondTitleSize = TextSize("A Switch", titleFontSize, true);
    const float connectTitleHeight = connectFirstTitleSize.y + titleLineGap + connectSecondTitleSize.y;
    const float connectGroupTop = (cardHeight - (iconBoxSize + iconTitleGap + connectTitleHeight)) / 2;
    const float sharedIconCenterY = connectGroupTop + iconBoxSize / 2;
    const float sharedTitleTopY = connectGroupTop + iconBoxSize + iconTitleGap;

    if (HomeCard("Connect", {260, 210}, {340, 350}, Color(.08f, .55f, 1), Color(.20f, .25f, .92f), "Connect To",
                 "A Switch", false, ImVec2(80, 69), titleFontSize, sharedIconCenterY, sharedTitleTopY))
        currentPage = Page::Connect;
    if (HomeCard("Host", {680, 210}, {340, 350}, Color(.25f, .66f, .58f), Color(.43f, .20f, .78f), "Host This",
                 "Switch", true, ImVec2(82, 55.6f), titleFontSize, sharedIconCenterY, sharedTitleTopY))
        currentPage = Page::Host;
    Footer(true);
}

// Draws `text` centered within [x, x+width] at the top-left `y`. If it fits on one
// line at `size` it's centered as a single line; otherwise it falls back to
// left-aligned wrapping (so a longer translation degrades gracefully instead of
// rendering off-center). Returns the vertical space it consumed.
float CenteredParagraph(ImDrawList *draw, ImVec2 origin, float x, float y, float width, float size, ImU32 color,
                        const char *text)
{
    const ImVec2 lineSize = TextSize(text, size, false);
    if (lineSize.x <= width)
    {
        Text(draw, origin + ImVec2(x + (width - lineSize.x) / 2.0f, y), size, color, text);
        return lineSize.y;
    }
    ImGui::SetCursorPos({x, y});
    ImFont *font = ImGui::GetFont();
    const float savedScale = font->Scale;
    font->Scale = size / 46.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    const float consumed = ImGui::GetCursorPosY() - y;
    font->Scale = savedScale;
    return consumed;
}

const std::string LastJoinConfigPath = std::string(SDMC) + "/config/nxremoteplay/last_join.txt";

// Last-used Connector Session ID/Password (, QoL requirement: don't
// make the user retype the same code every time - matters for repeat testing
// and for anyone who joins the same console regularly). Same "never let a
// failed SD read/write take the render loop down" philosophy as
// ReadPersistedHostingEnabledFromSd/WritePersistedHostingEnabledToSd above -
// this is a convenience feature, not something that should ever be able to
// crash the app if the file is missing or the SD card hiccups. Purely a
// UI-side convenience file; the Connector networking code itself
// (RendezvousClient.cpp) never reads it.
void ReadLastJoinFromSd()
{
    try
    {
        if (!fs::Exists(LastJoinConfigPath))
            return;
        auto bytes = fs::OpenFile(LastJoinConfigPath);
        std::string text(bytes.begin(), bytes.end());

        auto extract = [&](const char *key, char *out, size_t outSize) {
            auto pos = text.find(key);
            if (pos == std::string::npos)
                return;
            pos += strlen(key);
            auto end = text.find_first_of("\r\n", pos);
            std::string value = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (value.size() < outSize)
                strcpy(out, value.c_str());
        };
        extract("id=", connectSessionId, sizeof(connectSessionId));
        extract("password=", connectPassword, sizeof(connectPassword));
    }
    catch (...)
    {
    }
}

void WriteLastJoinToSd()
{
    try
    {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "id=%s\npassword=%s\n", connectSessionId, connectPassword);
        fs::CreateDir(SDMC "/config/nxremoteplay/");
        fs::WriteFile(LastJoinConfigPath, std::vector<u8>(buf, buf + n));
    }
    catch (...)
    {
    }
}

void RenderConnect()
{
    // Loaded once, lazily - not at static-init time, so this stays consistent
    // with the rest of this file's "no real I/O until the screen is actually
    // used" convention (e.g. RenderHost's own IPC connection).
    static bool loadedLastJoin = false;
    if (!loadedLastJoin)
    {
        loadedLastJoin = true;
        ReadLastJoinFromSd();
    }

    // Register the media callbacks before starting the connector.
    ConnectorMedia::EnsureVideoCallbackRegistered();
    ConnectorMedia::EnsureStillFrameCallbackRegistered();

    // Poll() itself now called once from scenes::Host() - see its own comment.
    if (Network::Rendezvous::Connector::GetState() == Network::Rendezvous::State::Connected)
    {
        currentPage = Page::ConnectorPlayback;
        return;
    }

    if (BackButton())
        currentPage = Page::Home;
    DrawNavigationBrand();
    // Sized to this screen's own content height + ScreenPanel::ContentPadding (20px)
    // on top AND bottom, rather than the taller shared ScreenPanel::Height - that
    // left a big empty strip below the button once the content itself was tightened.
    // Measured via temporary printf against the actual centered/30px-gap layout below
    // (contentBottom=351 -> 351+20=371); if the copy or layout above changes enough to
    // shift where the button lands, remeasure and update this constant to match.
    // Grows while a status/error line is shown below the button - that line used to fall outside the
    // fixed 371-unit panel and was clipped away, so failures never appeared on screen.
    const float panelHeight =
        371.0f + (Network::Rendezvous::Connector::GetState() != Network::Rendezvous::State::Idle ? 110.0f : 0.0f);
    // Vertically centered on the 720px canvas (not pinned to ScreenPanel::PosY) now
    // that each screen has its own panel height - a shared fixed Y would leave
    // different amounts of space below each differently-sized box.
    const float panelY = (UI::WindowHeight - panelHeight) / 2.0f;
    DrawPanel({ScreenPanel::PosX, panelY}, {ScreenPanel::Width, panelHeight});

    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 panelOrigin = ImGui::GetWindowPos();
    constexpr float contentX = ScreenPanel::ContentPadding, fieldWidth = ScreenPanel::ContentWidth;
    constexpr float titleSize = 46.0f * 1.3f;

    float y = contentX;
    {
        const char *title = "Connect to another Switch";
        const ImVec2 titleTextSize = TextSize(title, titleSize, true);
        Text(draw, panelOrigin + ImVec2(contentX + (fieldWidth - titleTextSize.x) / 2.0f, y), titleSize, Color(1, 1, 1),
             title, true);
        y += titleTextSize.y + 8.0f;
    }

    // Renders at the same smaller "screen caption" size already used for the Settings
    // screen's subtitle. Centered like every other text on this screen; falls back to
    // left-aligned wrapping if a translation makes it too long for one line.
    y += CenteredParagraph(draw, panelOrigin, contentX, y, fieldWidth, 32.0f, Color(.72f, .78f, .92f),
                           "Enter the Session ID and Password shared by the host.");

    // Session ID and Password sit side by side instead of stacked, so the two-line
    // wrapped subtext above doesn't push the Connect button down into the footer.
    // The typed code also gets a noticeably wider inset than the project's default
    // 14px horizontal frame padding, and the field labels are drawn at the same small
    // "eyebrow" size as the subtext above (rather than the full 46px body size),
    // centered over their own column instead of left-aligned.
    // Everything is laid out from the actual cursor position, so this still holds if a
    // translation wraps the subtext to a different number of lines.
    constexpr float labelSize = 28.0f, labelGap = 8.0f, columnGap = 24.0f;
    const ImVec2 fieldPadding(20, 10);
    const float columnWidth = (fieldWidth - columnGap) / 2.0f;
    const float column2X = contentX + columnWidth + columnGap;

    y += 24.0f;
    {
        const ImVec2 label1Size = TextSize("SESSION ID", labelSize, true);
        const ImVec2 label2Size = TextSize("PASSWORD", labelSize, true);
        Text(draw, panelOrigin + ImVec2(contentX + (columnWidth - label1Size.x) / 2.0f, y), labelSize,
             Color(.55f, .60f, .70f), "SESSION ID", true);
        Text(draw, panelOrigin + ImVec2(column2X + (columnWidth - label2Size.x) / 2.0f, y), labelSize,
             Color(.55f, .60f, .70f), "PASSWORD", true);
        y += label1Size.y + labelGap;
    }

    ImGui::SetCursorPos({contentX, y});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, fieldPadding);
    ImGui::PushItemWidth(columnWidth);
    InputTextWithKeyboard("##session", connectSessionId, sizeof(connectSessionId), "Session ID", true,
                          ImGuiInputTextFlags_CharsUppercase);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar();
    const float fieldBottom = ImGui::GetCursorPosY();

    ImGui::SetCursorPos({column2X, y});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, fieldPadding);
    ImGui::PushItemWidth(columnWidth);
    InputTextWithKeyboard("##password", connectPassword, sizeof(connectPassword), "Password", true,
                          ImGuiInputTextFlags_CharsUppercase);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar();

    y = fieldBottom + 30.0f;
    ImGui::SetCursorPos({contentX, y});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 32);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.08f, .43f, .96f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.19f, .28f, .43f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.12f, .32f, .64f, 1));
    const bool connectPressed = ImGui::Button("Connect", {fieldWidth, 66});
    const bool connectSelected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 connectPos = ImGui::GetItemRectMin(), connectSize = ImGui::GetItemRectSize();
    if (connectSelected)
        ImGui::GetWindowDrawList()->AddRect(connectPos, connectPos + connectSize, Color(.42f, .76f, 1), 32,
                                            ImDrawCornerFlags_All, 2);
    if (connectPressed)
    {
        WriteLastJoinToSd();
        Network::Rendezvous::Connector::Connect(connectSessionId, connectPassword);
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    using Network::Rendezvous::State;
    const State connectState = Network::Rendezvous::Connector::GetState();
    if (connectState != State::Idle)
    {
        y = ImGui::GetCursorPosY() + 8.0f;
        std::string message;
        switch (connectState)
        {
        // The live step text (same wording as the Android app: "Requesting the console wake
        // up...", "Waiting for the console to answer (attempt N)...", ...); full detail is in
        // /config/nxremoteplay/nro_log.txt.
        case State::Requesting:
        case State::Paired:
        case State::Handshaking: {
            message = Network::Rendezvous::Connector::GetStatusText();
            if (message.empty())
                message = "Connecting...";
            break;
        }
        // State::Connected is handled above (transitions straight to
        // Page::ConnectorPlayback before this switch is ever reached) -
        // no case needed here.
        case State::Error:
            message = Network::Rendezvous::Connector::GetErrorMessage();
            break;
        default:
            break;
        }
        CenteredParagraph(draw, panelOrigin, contentX, y, fieldWidth, 32.0f,
                          connectState == State::Error ? Color(.95f, .55f, .55f) : Color(.72f, .78f, .92f),
                          message.c_str());
    }
    EndPanel();
    Footer(false);
}

// Playback fills the viewport once the connector reaches Connected.
// Hold L+R+ZL+ZR to switch between local menu navigation and remote-controller input.
// In passthrough mode B is forwarded instead of activating Back. Raw libnx Npad state
// preserves the native button bits and stick axes; previews do not forward controller input.
constexpr u64 PassthroughComboMask = HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR;
constexpr double PassthroughComboHoldSeconds = 0.6;
// HOME shortcut (, from the  proposal): hold - and + together to send HOME to the console.
// Works with passthrough on or off; while both are held neither is forwarded to the game.
constexpr u64 HomeComboMask = HidNpadButton_Minus | HidNpadButton_Plus;
constexpr double HomeComboHoldSeconds = 0.5;

// Hoisted to file scope ( bugfix) so scenes::Host's own global
// Cancel/Back handler below can see it - it used to be a plain local static
// inside RenderConnectorPlayback(), which meant B still exited this screen to
// Home the instant it was pressed while passthrough was engaged, because that
// global handler runs BEFORE the per-screen switch and had no way to know
// passthrough was active. Hardware-confirmed real bug  (user: "when
// I push b, in the nro it goes back" during an otherwise-working passthrough
// session) - RenderConnectorPlayback()'s own `if (!passthroughMode &&
// BackButton())` guard further down was correct but never got a chance to
// matter, since currentPage had already been flipped to Home before this
// function's body ran that frame.
static bool passthroughMode = false;

// ------------------------------------------------------------------------------------------------
// Stream menu (, user design). A draggable hamburger circle replaces the old corner "back"
// arrow on this screen. Tap it (or select it with the controller) to open a small menu: Home (sends HOME
// to the console), Passthrough on/off, Exit (ends the session, back to Home). It fades to semi-transparent
// when idle, its position is remembered (/config/nxremoteplay/menu_pos.txt), and a GREEN ring means passthrough
// is on. Passthrough rules, as requested: with passthrough OFF the controller can select the circle and
// its items like any other button; with passthrough ON it is semi-transparent and the controller cannot
// select it (every button belongs to the console then) - touch works in both modes. The old
// always-visible "hold L+R+ZL+ZR" badge and the status notes now live in this menu / a short toast.
// ------------------------------------------------------------------------------------------------
static bool g_streamMenuOpen = false; // file scope: scenes::Host()'s global B handler must not exit while open

namespace
{
constexpr float StreamMenuCircle = 54.0f;
constexpr double StreamMenuFadeDelay = 3.0;
constexpr float StreamMenuIdleAlpha = 0.35f;
const std::string StreamMenuPosPath = std::string(SDMC) + "/config/nxremoteplay/menu_pos.txt";

struct StreamMenuState
{
    float fx = 0.97f, fy = 0.05f; // fractions (0..1) of the free travel area; default near the top-right
    bool loaded = false, dragged = false;
    double lastActive = 0.0, toastUntil = 0.0;
    float alpha = 1.0f;
    std::string toast;
} g_sm;

float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Clears any controller-navigation focus / pending activation so no gamepad button can land on (or press)
// anything on the playback screen. The screen has no controller-navigable widgets; touch is unaffected.
void DropNavFocus()
{
    ImGuiContext &g = *GImGui;
    g.NavId = 0;
    g.NavActivateId = g.NavActivateDownId = g.NavActivatePressedId = g.NavInputId = 0;
    g.NavInitRequest = g.NavMoveRequest = false;
    g.NavDisableHighlight = true;
}

void StreamMenuLoadPos()
{
    try
    {
        if (!fs::Exists(StreamMenuPosPath))
            return;
        auto bytes = fs::OpenFile(StreamMenuPosPath);
        std::string s(bytes.begin(), bytes.end());
        float x = 0, y = 0;
        if (sscanf(s.c_str(), "%f %f", &x, &y) == 2)
        {
            g_sm.fx = Clamp01(x);
            g_sm.fy = Clamp01(y);
        }
    }
    catch (...)
    {
    }
}

void StreamMenuSavePos()
{
    try
    {
        fs::CreateDir(SDMC "/config/nxremoteplay/");
        char buf[48];
        const int n = snprintf(buf, sizeof(buf), "%.4f %.4f\n", g_sm.fx, g_sm.fy);
        fs::WriteFile(StreamMenuPosPath, std::vector<u8>(buf, buf + (n > 0 ? n : 0)));
    }
    catch (...)
    {
    }
}

void StreamMenuToast(const std::string &text, double seconds)
{
    g_sm.toast = text;
    g_sm.toastUntil = ImGui::GetTime() + seconds;
}
} // namespace

static void RenderStreamMenu()
{
    auto *draw = ImGui::GetWindowDrawList();
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float W = (float)UI::WindowWidth, H = (float)UI::WindowHeight;
    const float S = StreamMenuCircle;
    const double now = ImGui::GetTime();
    if (!g_sm.loaded)
    {
        StreamMenuLoadPos();
        g_sm.loaded = true;
        g_sm.lastActive = now;
    }
    const float maxX = W - S, maxY = H - S;

    //  (user rule): the circle and everything in its menu are TOUCH-ONLY, in every mode. No controller
    // button may focus, highlight, open, close or activate them (they used to be controller-selectable with
    // passthrough off, and a focus left over from that state still let A open the menu once passthrough was on).
    // The circle and the items are always created NoNav, and DropNavFocus() clears any focus that is already set.
    DropNavFocus();

    // ---- the circle (an InvisibleButton: touch/mouse only)
    ImGui::SetCursorPos({g_sm.fx * maxX, g_sm.fy * maxY});
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_NoNavDefaultFocus, true);
    const bool pressed = ImGui::InvisibleButton("streammenu", {S, S});
    ImGui::PopItemFlag();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(),
               focused = false; // never controller-focused
    const bool deactivated = ImGui::IsItemDeactivated();
    const ImVec2 cmin = ImGui::GetItemRectMin(); // absolute coordinates
    if (ImGui::IsItemActivated())
        g_sm.dragged = false;
    if (active && ImGui::IsMouseDragging(0, 8.0f))
    {
        g_sm.dragged = true;
        g_sm.fx = Clamp01(g_sm.fx + io.MouseDelta.x / maxX);
        g_sm.fy = Clamp01(g_sm.fy + io.MouseDelta.y / maxY);
    }
    if (deactivated && g_sm.dragged)
        StreamMenuSavePos();
    if (pressed && !g_sm.dragged)
        g_streamMenuOpen = !g_streamMenuOpen;
    if (deactivated)
        g_sm.dragged = false;

    // ---- fade: solid while touched/focused/open/just-toasted, semi-transparent otherwise
    if (hovered || active || focused || g_streamMenuOpen || now < g_sm.toastUntil)
        g_sm.lastActive = now;
    float target = 1.0f;
    if (passthroughMode && !(hovered || active || g_streamMenuOpen))
        target = StreamMenuIdleAlpha;
    else if (now - g_sm.lastActive > StreamMenuFadeDelay)
        target = StreamMenuIdleAlpha;
    g_sm.alpha += (target - g_sm.alpha) * std::min(1.0f, io.DeltaTime * 8.0f);
    const float a = g_sm.alpha;

    // Same look as the Android app's menu circle: dark disc, thin white ring, white three-line icon. Passthrough
    // on keeps the green ring (the only state indicator on the circle).
    const ImVec2 c = cmin + ImVec2(S / 2, S / 2);
    draw->AddCircleFilled(c, S / 2, Color(.078f, .102f, .157f, .85f * a), 64);
    if (passthroughMode)
        draw->AddCircle(c, S / 2, Color(.35f, .95f, .55f, a), 64, 4);
    else
        draw->AddCircle(c, S / 2, Color(1.f, 1.f, 1.f, .7f * a), 64, 2);
    for (int i = -1; i <= 1; i++)
        draw->AddLine(c + ImVec2(-12, i * 7.2f), c + ImVec2(12, i * 7.2f), Color(1.f, 1.f, 1.f, a), 3);

    // ---- toast next to the circle (passthrough toggled by the combo, etc.)
    if (!g_streamMenuOpen && now < g_sm.toastUntil)
    {
        const float fs = 26.0f;
        const ImVec2 ts = TextSize(g_sm.toast.c_str(), fs, true);
        float tx = c.x - ts.x / 2;
        tx = std::max(origin.x + 18, std::min(origin.x + W - ts.x - 18, tx));
        const float ty = (cmin.y - origin.y + S + 12 + ts.y + 12 <= H) ? cmin.y + S + 14 : cmin.y - 14 - ts.y;
        draw->AddRectFilled(ImVec2(tx - 12, ty - 6), ImVec2(tx + ts.x + 12, ty + ts.y + 6), Color(0, 0, 0, .6f), 12);
        Text(draw, ImVec2(tx, ty), fs, passthroughMode ? Color(.35f, .95f, .55f) : Color(1, 1, 1), g_sm.toast.c_str(),
             true);
    }

    if (!g_streamMenuOpen)
        return;

    // ---- the open menu
    constexpr float PW = 360, RH = 58, PAD = 10, HINT_H = 98;
    constexpr int rows = 3;
    const float PH = PAD + rows * RH + HINT_H + PAD;
    float px = cmin.x + S / 2 - PW / 2;
    px = std::max(origin.x + 12, std::min(origin.x + W - PW - 12, px));
    float py = (cmin.y - origin.y + S + 10 + PH <= H) ? cmin.y + S + 10 : cmin.y - 10 - PH;
    py = std::max(origin.y + 10, py);
    const ImVec2 pmin(px, py), pmax(px + PW, py + PH);
    // Panel colours and corner radius as in the Android menu.
    draw->AddRectFilled(pmin, pmax, Color(.071f, .102f, .2f, .95f), 16);
    draw->AddRect(pmin, pmax, Color(.4f, .478f, .671f, .45f), 16, ImDrawCornerFlags_All, 1.5f);

    // A tap anywhere else (not the panel, not the circle) just closes the menu.
    if (ImGui::IsMouseClicked(0))
    {
        const ImVec2 m = io.MousePos;
        const bool inPanel = m.x >= pmin.x && m.x <= pmax.x && m.y >= pmin.y && m.y <= pmax.y;
        if (!inPanel && !hovered)
            g_streamMenuOpen = false;
    }

    auto item = [&](int idx, const char *id, const char *label, bool green) -> bool {
        ImGui::SetCursorScreenPos({px + PAD, py + PAD + idx * RH});
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_NoNavDefaultFocus, true);
        const bool clicked = ImGui::InvisibleButton(id, {PW - 2 * PAD, RH - 6});
        ImGui::PopItemFlag();
        const ImVec2 imin = ImGui::GetItemRectMin(), imax = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered())
            draw->AddRectFilled(imin, imax, Color(1.f, 1.f, 1.f, .12f), 12);
        const ImVec2 ts = TextSize(label, 30, true);
        Text(draw, ImVec2(imin.x + 18, imin.y + ((imax.y - imin.y) - ts.y) / 2), 30,
             green ? Color(.35f, .95f, .55f) : Color(1, 1, 1), label, true);
        return clicked;
    };

    int row = 0;
    if (item(row++, "menuhome", "Home", false))
    {
        Network::Rendezvous::Connector::SendHomeTap();
        g_streamMenuOpen = false;
    }
    if (item(row++, "menupass", passthroughMode ? "Passthrough: ON" : "Passthrough: OFF", passthroughMode))
    {
        passthroughMode = !passthroughMode;
        Network::Rendezvous::Connector::Note(passthroughMode ? "Passthrough ON (menu)" : "Passthrough OFF (menu)");
        StreamMenuToast(passthroughMode ? "Passthrough ON" : "Passthrough OFF", 2.0);
        g_streamMenuOpen = false;
    }
    if (item(row++, "menuexit", "Exit", false))
    {
        g_streamMenuOpen = false;
        Network::Rendezvous::Connector::Disconnect(); // the punched connection would otherwise stay up
        currentPage = Page::Home;
    }

    const float hy = py + PAD + rows * RH + 6;
    Text(draw, ImVec2(px + PAD + 6, hy), 22, Color(.78f, .84f, .96f), "Hold L+R+ZL+ZR to turn passthrough on/off",
         true);
    Text(draw, ImVec2(px + PAD + 6, hy + 56), 22, Color(.78f, .84f, .96f), "Hold - and + for HOME", true);
    Text(draw, ImVec2(px + PAD + 6, hy + 28), 22, passthroughMode ? Color(.35f, .95f, .55f) : Color(.78f, .84f, .96f),
         passthroughMode ? "Passthrough on: your buttons control the console"
                         : "Passthrough off: your buttons control this app",
         true);
}

void RenderConnectorPlayback()
{
    // Poll() itself now called once from scenes::Host() - see its own comment.
    using Network::Rendezvous::State;
    const State connectorState = Network::Rendezvous::Connector::GetState();

    static PadState pad;
    static bool padInited = false;
    // -1 = combo not currently being timed; comboArmed requires a full
    // release before the combo can fire again, so holding past the threshold
    // doesn't toggle repeatedly for as long as it's held.
    static double comboHeldSince = -1.0;
    static bool comboArmed = true;

    static bool sessionSeen = false;
    if (connectorState == State::Connected)
    {
        if (!padInited)
        {
            padInitializeDefault(&pad);
            padInited = true;
        }
        if (!sessionSeen)
        {
            // Passthrough starts ON for every new session; the menu or the L+R+ZL+ZR hold turns it off.
            sessionSeen = true;
            passthroughMode = true;
            Network::Rendezvous::Connector::Note("Passthrough ON (joined)");
            StreamMenuToast("Passthrough ON", 2.0);
        }

        padUpdate(&pad);
        const u64 heldButtons = padGetButtons(&pad);
        const HidAnalogStickState leftStick = padGetStickPos(&pad, 0);
        const HidAnalogStickState rightStick = padGetStickPos(&pad, 1);

        const bool comboHeld = (heldButtons & PassthroughComboMask) == PassthroughComboMask;
        const double now = ImGui::GetTime();
        if (comboHeld && comboArmed)
        {
            if (comboHeldSince < 0.0)
            {
                Network::Rendezvous::Connector::Note("Passthrough combo detected - hold for the toggle");
                StreamMenuToast("Keep holding to switch passthrough...", 1.2);
            }
            if (comboHeldSince < 0.0)
                comboHeldSince = now;
            if (now - comboHeldSince >= PassthroughComboHoldSeconds)
            {
                passthroughMode = !passthroughMode;
                Network::Rendezvous::Connector::Note(passthroughMode ? "Passthrough ON (combo held)"
                                                                     : "Passthrough OFF (combo held)");
                StreamMenuToast(passthroughMode ? "Passthrough ON" : "Passthrough OFF", 2.0);
                comboArmed = false;
                comboHeldSince = -1.0;
            }
        }
        else if (!comboHeld)
        {
            comboHeldSince = -1.0;
            comboArmed = true;
        }

        // HOME shortcut: - + + held together for HomeComboHoldSeconds sends one HOME tap (re-arms after both are
        // released).
        static double homeHeldSince = -1.0;
        static bool homeArmed = true;
        const bool homeComboHeld = (heldButtons & HomeComboMask) == HomeComboMask;
        if (homeComboHeld && homeArmed)
        {
            if (homeHeldSince < 0.0)
                homeHeldSince = now;
            if (now - homeHeldSince >= HomeComboHoldSeconds)
            {
                Network::Rendezvous::Connector::Note("HOME shortcut (- + + held) - sending HOME");
                Network::Rendezvous::Connector::SendHomeTap();
                StreamMenuToast("HOME", 1.2);
                homeArmed = false;
                homeHeldSince = -1.0;
            }
        }
        else if (!homeComboHeld)
        {
            homeHeldSince = -1.0;
            homeArmed = true;
        }

        // Stuck-button fix : the host keeps the LAST state it was sent until a new
        // one arrives, so leaving passthrough (or the combo itself, which is L+R+ZL+ZR) used to
        // leave those buttons held on the host forever. While the combo is fully held its buttons
        // are not forwarded, and turning passthrough off sends one explicit "everything released".
        static bool wasPassthrough = false;
        if (passthroughMode)
        {
            // No touchscreen input from this screen - a Connector Switch's own
            // touchscreen isn't a meaningful input source for a game running
            // on a different, remote console. touchActive stays false; the
            // The host applies touch coordinates only while touch is active.
            u64 forwarded = comboHeld ? (heldButtons & ~PassthroughComboMask) : heldButtons;
            if (homeComboHeld)
                forwarded &= ~HomeComboMask;
            Network::Rendezvous::Connector::SendInputState(false, 0.0f, 0.0f, forwarded, leftStick.x, leftStick.y,
                                                           rightStick.x, rightStick.y);
        }
        else if (wasPassthrough)
        {
            Network::Rendezvous::Connector::SendInputState(false, 0.0f, 0.0f, 0, 0, 0, 0, 0);
        }
        wasPassthrough = passthroughMode;
    }
    else
    {
        // Not connected - passthrough is meaningless and a lingering mode
        // from a previous connection on this screen must not carry over
        // silently into a fresh one.
        passthroughMode = false;
        sessionSeen = false;
        comboHeldSince = -1.0;
        comboArmed = true;
    }

    // No SetupMainWindow()/ImGui::End() here - scenes::Host() already opened
    // the "Remote Link" window and calls End() once after this returns,
    // same as every other Render*() page function in this file.

    // No forced redirect back to Page::Connect on anything other than State::Connected: a connection
    // that drops while this screen is showing displays that fact instead of silently vanishing.
    if (connectorState == State::Connected && ConnectorMedia::ShowVideo())
    {
        ImGui::SetCursorPos({0, 0});
        ConnectorMedia::GetVideoTexture().Draw({UI::WindowWidth, UI::WindowHeight});
    }
    // HOME/unfocused fallback (, reintroduced JPEG fallback): the
    // Host has no live H.264 to send while its own app isn't foregrounded, so
    // there's never a "first video frame" to wait for in that state - without
    // this, the screen just stays on the placeholder text below indefinitely,
    // making it impossible to see (or, with passthrough engaged, navigate)
    // the Host's own Home menu at all. Deliberately still behind the
    // State::Connected check above with live video winning whenever
    // HasFrame() is true - a still is always safe to fall back to, but never
    // preferred over real video.
    else if (connectorState == State::Connected && ConnectorMedia::ShowStill())
    {
        ImGui::SetCursorPos({0, 0});
        ConnectorMedia::DrawStillFrame({UI::WindowWidth, UI::WindowHeight});
    }
    else
    {
        ImGui::NewLine();
        CenterText(connectorState == State::Connected ? "Connected. Waiting for the first video frame..."
                   : connectorState == State::Error   ? Network::Rendezvous::Connector::GetErrorMessage()
                                                      : std::string("Not connected."));
    }

    // (: the bottom-right mode badge moved into the stream menu's hint lines / toast.)

    // the corner back arrow was replaced by the draggable stream menu circle (Home / Passthrough /
    // Exit, see RenderStreamMenu above). It is drawn after the video so the full-screen image can't paint over it,
    // and it stays on screen (semi-transparent) in passthrough mode, touch-only.
    RenderStreamMenu();
}

// Direct SD-card fallback for the hosting-enabled toggle, independent of any
// live IPC connection to the sysmodule. Reads/writes the exact same file
// (`/config/nxremoteplay/hosting_enabled.txt`) the sysmodule's own
// the host persistence code use - same path,
// same "absent/non-'0' = on" convention - so both sides always agree
// regardless of which one last wrote it. This exists specifically so
// turning hosting on/off from the NRO works even when NxRemotePlay/the sysmodule
// isn't currently reachable (can't reach it -> can't send live IPC -> the
// toggle would otherwise be either invisible or a no-op), per explicit user
// direction: this is meant to be a simple persistent setting, not something
// that only works while a live session happens to be up.
const std::string HostingEnabledConfigPath = std::string(SDMC) + "/config/nxremoteplay/hosting_enabled.txt";

bool ReadPersistedHostingEnabledFromSd()
{
    // fs::Exists() itself (std::filesystem::exists) is not exception-safe in
    // its own right - wrapping it too, not just the OpenFile call below, so
    // nothing in this path can ever throw out of the render loop.
    try
    {
        if (!fs::Exists(HostingEnabledConfigPath))
            return true;
        auto bytes = fs::OpenFile(HostingEnabledConfigPath);
        return bytes.empty() || bytes[0] != '0';
    }
    catch (...)
    {
        return true;
    }
}

void WritePersistedHostingEnabledToSd(bool enabled)
{
    // A failed write here must never take the whole app down mid-frame - an
    // uncaught exception from fs::WriteFile (or CreateDir) would propagate
    // straight out of the ImGui render loop with nothing above it to catch
    // it. Matches this project's own established philosophy for this exact
    // file on the sysmodule side (host persistence: "failures
    // are logged but not fatal, since a failed persist just means the next
    // boot falls back to the last successfully-persisted value").
    try
    {
        fs::CreateDir(SDMC "/config/nxremoteplay/");
        fs::WriteFile(HostingEnabledConfigPath, {static_cast<u8>(enabled ? '1' : '0')});
    }
    catch (...)
    {
    }
}

void RenderHost()
{
    // Reads the sysmodule's own real, live Session ID/Password over local
    // IPC (see NxRemotePlayGetHostingState in ipc.h) instead of the NRO doing its
    // own independent REGISTER networking. The host owns the identity and is
    // on by default, and keeps working after this screen/app closes. Polled
    // over ONE session kept open for as long as this screen is up (see
    // g_hostingConnected above) rather than a fresh connect+close every
    // frame - closed by scenes::Host() the frame after this screen is left.
    //
    // The actual GetHostingState dispatch is throttled to 4Hz (every 250ms),
    // not once per rendered frame (~30-60Hz) - polling every frame was still
    // real IPC traffic even without reconnecting, and made this screen's own
    // UDP debug log spam so heavy it drowned out the one-off events (button
    // presses, state transitions) someone would actually be looking for.
    // State visibly changing within ~5min (SessionHosting's own refresh
    // interval) or ~100ms (a manual refresh/toggle) doesn't need faster
    // polling than this to read as "live" to a human.
    static HostingSnapshot cachedSnapshot = {};
    static bool cachedIpcOk = false;
    static double lastPollTime = -1000.0;
    static int consecutiveFailures = 0;
    // Fallback source of truth for "is hosting enabled" when there's no live
    // IPC session to ask - read directly from the SD card (see
    // ReadPersistedHostingEnabledFromSd above), so the toggle below still has
    // a real answer to show/act on even when NxRemotePlay isn't reachable at all.
    static bool cachedPersistedEnabled = true;
    // Debounce for the toggle button specifically - see SetHostingEnabledEverywhere
    // below for why this exists.
    static double lastToggleActionTime = -1000.0;

    if (!g_hostingConnected)
    {
        g_hostingConnected = R_SUCCEEDED(NxRemotePlayConnect());
        consecutiveFailures = 0;
    }

    const double now = ImGui::GetTime();
    if ((now - lastPollTime) >= 0.25)
    {
        lastPollTime = now;
        if (g_hostingConnected)
        {
            HostingSnapshot freshSnapshot = {};
            const bool pollOk = R_SUCCEEDED(NxRemotePlayGetHostingState(&freshSnapshot));
            if (pollOk)
            {
                consecutiveFailures = 0;
                cachedSnapshot = freshSnapshot;
                cachedIpcOk = true;
                g_footerRegState = (int)freshSnapshot.state;
            }
            else
            {
                // A single dropped/delayed reply shouldn't flash "NxRemotePlay isn't
                // running" at the user or tear down a perfectly good session -
                // keep showing the last known-good snapshot and just retry next
                // poll. Only treat the session as actually dead (and reconnect)
                // after two misses in a row (~500ms of real unavailability, not
                // one transient blip).
                ++consecutiveFailures;
                if (consecutiveFailures >= 2)
                {
                    cachedIpcOk = false;
                    g_footerRegState = -1;
                    NxRemotePlayClose();
                    g_hostingConnected = false;
                }
            }
        }
        if (!g_hostingConnected || !cachedIpcOk)
            cachedPersistedEnabled = ReadPersistedHostingEnabledFromSd();
    }
    const HostingSnapshot &hostingSnapshot = cachedSnapshot;
    const bool hostingIpcOk = g_hostingConnected && cachedIpcOk;
    // Error still means the feature is turned ON (just currently failing to
    // reach the rendezvous server) - only Disabled means the user turned it
    // off. Without a live connection at all, fall back to the persisted SD
    // read above instead of assuming "off" - this is what lets the toggle
    // below still show and act on the right state when NxRemotePlay isn't
    // reachable, not just while erroring against an unreachable rendezvous
    // server specifically.
    const bool hostingEnabled =
        hostingIpcOk ? (hostingSnapshot.state != HostingState_Disabled) : cachedPersistedEnabled;

    if (BackButton())
        currentPage = Page::Home;
    DrawNavigationBrand();
    // Same shared width and centering as RenderConnect. Height is its own value,
    // sized to THIS screen's own content + top/bottom padding, same idea as
    // Connect's - see the comment there for how these constants are derived. Grown
    // from the original 340 to fit the new "Turn Off/On Hosting" button added below
    // Refresh. 460 (not the ~410 this screen's content actually measures at in the
    // WebAssembly preview) is deliberately generous - a real hardware test reported
    // this button clipped at a tighter value despite the WebAssembly preview showing
    // it fitting cleanly, most likely from small font-metric differences between the
    // WASM/FreeType text measurement and the Switch's own font rendering. Re-tighten
    // only after hardware confirms this margin is comfortably more than needed.
    // Derived exactly from the original, already-measured single-button Host
    // panel (340 = 20px top + content + 20px bottom, per the comment history
    // on Connect's identical convention) plus this round's own two additions:
    // a 14px gap and one more 64px-tall button. 340 + 14 + 64 = 418. Not
    // re-measured from scratch - the previous two attempts at this constant
    // (400, then a guessed 460) were both wrong in different ways; this one is
    // arithmetic on a proven baseline, not another guess.
    // Status line under the buttons  - same idea as the Connect screen's: it only
    // exists while there is something to say, and the panel grows to make room for it (a line drawn
    // outside a fixed-height panel is clipped away). Priority: an automatic-renewal notice from the
    // console (~30 s), then the standing "server unreachable" note when it is still showing an ID.
    std::string statusLine;
    ImU32 statusColor = Color(.72f, .78f, .92f);
    if (hostingIpcOk && hostingSnapshot.notice[0] && hostingSnapshot.noticeAgeSec < 30)
    {
        statusLine = hostingSnapshot.notice;
        statusColor = Color(.55f, .90f, .70f);
    }
    else if (hostingIpcOk && hostingSnapshot.state == HostingState_Error && hostingSnapshot.id[0])
    {
        statusLine = "Can't reach the server - this ID only works on your local network for now.";
        statusColor = Color(.95f, .75f, .35f);
    }
    const float panelHeight = 418.0f + (statusLine.empty() ? 0.0f : 110.0f);
    const float panelY = (UI::WindowHeight - panelHeight) / 2.0f;
    DrawPanel({ScreenPanel::PosX, panelY}, {ScreenPanel::Width, panelHeight});

    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 panelOrigin = ImGui::GetWindowPos();
    constexpr float contentX = ScreenPanel::ContentPadding, fieldWidth = ScreenPanel::ContentWidth;
    constexpr float titleSize = 46.0f * 1.3f;

    float y = contentX;
    {
        const char *title = "Your Switch is ready";
        const ImVec2 titleTextSize = TextSize(title, titleSize, true);
        Text(draw, panelOrigin + ImVec2(contentX + (fieldWidth - titleTextSize.x) / 2.0f, y), titleSize, Color(1, 1, 1),
             title, true);
        y += titleTextSize.y + 8.0f;
    }

    y += CenteredParagraph(draw, panelOrigin, contentX, y, fieldWidth, 32.0f, Color(.72f, .78f, .92f),
                           "Share these details with the person you want to connect from.");

    // Same column geometry as Connect's fields; the codes are read-only display text
    // (not an input box) but sized like what would appear inside one (46px, the
    // ambient body font) so the two screens read as visually consistent.
    constexpr float labelSize = 28.0f, labelGap = 8.0f, columnGap = 24.0f, codeSize = 46.0f;
    const float columnWidth = (fieldWidth - columnGap) / 2.0f;
    const float column2X = contentX + columnWidth + columnGap;

    y += 24.0f;
    {
        const ImVec2 label1Size = TextSize("SESSION ID", labelSize, true);
        const ImVec2 label2Size = TextSize("PASSWORD", labelSize, true);
        Text(draw, panelOrigin + ImVec2(contentX + (columnWidth - label1Size.x) / 2.0f, y), labelSize,
             Color(.55f, .60f, .70f), "SESSION ID", true);
        Text(draw, panelOrigin + ImVec2(column2X + (columnWidth - label2Size.x) / 2.0f, y), labelSize,
             Color(.55f, .60f, .70f), "PASSWORD", true);
        y += label1Size.y + labelGap;
    }

    {
        // Error stays within the same one-line vertical budget the two codes
        // normally use (a single centered line instead of two columns) so it
        // never grows this screen's hand-measured fixed panel height.
        // An ID is shown in every state that has one - including Error (server unreachable), where it
        // still works for connections on the local network; only "no ID at all" falls to a message.
        if (!hostingIpcOk || hostingSnapshot.state == HostingState_Disabled ||
            (hostingSnapshot.state == HostingState_Error && !hostingSnapshot.id[0]))
        {
            std::string message =
                !hostingIpcOk ? "NX-Remote-Play isn't running"
                : hostingSnapshot.state == HostingState_Disabled
                    ? "Hosting is turned off"
                    : (hostingSnapshot.errorMessage[0] ? std::string(hostingSnapshot.errorMessage) : "Not hosting");
            const ImVec2 msgSize = TextSize(message.c_str(), codeSize, true);
            Text(draw, panelOrigin + ImVec2(contentX + (fieldWidth - msgSize.x) / 2.0f, y), codeSize,
                 Color(.95f, .55f, .55f), message.c_str(), true);
            y += msgSize.y;
        }
        else
        {
            // Explicitly falls back to "..." if Registered but the id/password
            // bytes happen to be empty (a timing edge case around a toggle
            // transition) instead of rendering a blank string - a real gap
            // found by inspection, not yet confirmed as the cause of a
            // reported blank-codes sighting, but a straightforward
            // correctness fix regardless.
            const std::string display1 = hostingSnapshot.id[0] ? hostingSnapshot.id : "...";
            const std::string display2 = hostingSnapshot.password[0] ? hostingSnapshot.password : "...";

            const ImVec2 code1Size = TextSize(display1.c_str(), codeSize, true);
            const ImVec2 code2Size = TextSize(display2.c_str(), codeSize, true);
            Text(draw, panelOrigin + ImVec2(contentX + (columnWidth - code1Size.x) / 2.0f, y), codeSize, Color(1, 1, 1),
                 display1.c_str(), true);
            Text(draw, panelOrigin + ImVec2(column2X + (columnWidth - code2Size.x) / 2.0f, y), codeSize, Color(1, 1, 1),
                 display2.c_str(), true);
            y += (code1Size.y > code2Size.y ? code1Size.y : code2Size.y);
        }
    }

    y += 30.0f;
    // The toggle below is ALWAYS shown and always actionable, live IPC or
    // not - per explicit user direction, hosting on/off is a simple
    // persistent setting, not something that only works while a live
    // session to the sysmodule happens to be up (or while the rendezvous
    // server happens to be reachable, which is a separate concern entirely -
    // see hostingEnabled's own comment above). When there's a live
    // connection the toggle still goes through the real IPC command first
    // (so an already-running sysmodule picks it up immediately); otherwise
    // it falls straight to the direct SD-card write, which is exactly what
    // the sysmodule itself reads back on its own next boot/reconnect.
    // Debounced: a hard 300ms cooldown between toggle actions, regardless of
    // input source, without being perceptible as lag for a single deliberate
    // press. Kept as cheap insurance even though the real bug this project
    // chased for a while (Session Hosting toggle always sending enabled=true
    // no matter which button was pressed) turned out to be unrelated - a
    // sysmodule-side TLS/IPC payload corruption from LOG() calls, fixed in
    // ipc\ipc.c's ParseRequestFromTLS. 's  entry
    // for the full story if this code is ever touched again.
    const auto SetHostingEnabledEverywhere = [&](bool enabled) {
        if (now - lastToggleActionTime < 0.3)
            return;
        lastToggleActionTime = now;
        if (hostingIpcOk && g_hostingConnected)
            NxRemotePlaySetHostingEnabled(enabled);
        else
            WritePersistedHostingEnabledToSd(enabled);
        cachedPersistedEnabled = enabled;
    };

    if (hostingEnabled)
    {
        if (hostingIpcOk)
        {
            // Refreshing the code needs a live session to actually ask for a
            // new one - not shown at all without one, unlike the toggle below.
            ImGui::SetCursorPos({contentX, y});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 32);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.08f, .43f, .96f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.19f, .28f, .43f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.12f, .32f, .64f, 1));
            const bool refreshPressed = ImGui::Button("Refresh ID and Password", {fieldWidth, 64});
            const bool refreshSelected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImVec2 refreshPos = ImGui::GetItemRectMin(), refreshSize = ImGui::GetItemRectSize();
            if (refreshSelected)
                ImGui::GetWindowDrawList()->AddRect(refreshPos, refreshPos + refreshSize, Color(.42f, .76f, 1), 32,
                                                    ImDrawCornerFlags_All, 2);
            if (refreshPressed)
                NxRemotePlayRequestNewCode();
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            y = ImGui::GetCursorPosY() + 14.0f;
        }

        // Muted "secondary" control (Footer's own pill-button colors, reused
        // rather than inventing a new style) below the primary Refresh button -
        // turns the sysmodule's always-on background feature off. Same 64px
        // height as Refresh, not a smaller custom height - the ambient font
        // this project loads is 46px (UI.cpp's FontSmallSize), so a button
        // shorter than roughly that plus its own frame padding clips the label
        // outright; 44 (this screen's first attempt) did exactly that.
        ImGui::SetCursorPos({contentX, y});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 32);
        ImGui::PushStyleColor(ImGuiCol_Button, Color(.04f, .12f, .23f, .72f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(.19f, .28f, .43f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(.12f, .32f, .64f, 1));
        const bool turnOffPressed = ImGui::Button("Turn Off Hosting", {fieldWidth, 64});
        const bool turnOffSelected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const ImVec2 turnOffPos = ImGui::GetItemRectMin(), turnOffSize = ImGui::GetItemRectSize();
        if (turnOffSelected)
            ImGui::GetWindowDrawList()->AddRect(turnOffPos, turnOffPos + turnOffSize, Color(.42f, .76f, 1), 32,
                                                ImDrawCornerFlags_All, 2);
        if (turnOffPressed)
            SetHostingEnabledEverywhere(false);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }
    else
    {
        // Hosting is off (live-confirmed or, with no live session, per the
        // persisted SD read) - one full-size primary button (same size/style
        // Refresh normally occupies) to turn it back on.
        ImGui::SetCursorPos({contentX, y});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 32);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.08f, .43f, .96f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.19f, .28f, .43f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.12f, .32f, .64f, 1));
        const bool turnOnPressed = ImGui::Button("Turn On Hosting", {fieldWidth, 64});
        const bool turnOnSelected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const ImVec2 turnOnPos = ImGui::GetItemRectMin(), turnOnSize = ImGui::GetItemRectSize();
        if (turnOnSelected)
            ImGui::GetWindowDrawList()->AddRect(turnOnPos, turnOnPos + turnOnSize, Color(.42f, .76f, 1), 32,
                                                ImDrawCornerFlags_All, 2);
        if (turnOnPressed)
            SetHostingEnabledEverywhere(true);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    if (!statusLine.empty())
        CenteredParagraph(draw, panelOrigin, contentX, ImGui::GetCursorPosY() + 8.0f, fieldWidth, 30.0f, statusColor,
                          statusLine.c_str());
    EndPanel();
    Footer(false);
}

// Shared server file: /config/nxremoteplay/server.txt is the ONE source of truth for the server that the
// sysmodule AND this NRO use. The NRO is the only writer; the sysmodule re-reads the file every cycle and applies it,
// and the NRO applies the same file to its own connections (RendezvousClient::LoadServerConfig). Parsing and
// validation are the identical server_config.h in both programs (checked by Tools/Test-Compatibility.py).
static bool LoadServerConfigFile(ServerConfig &cfg)
{
    ServerCfg_Defaults(&cfg);
    try
    {
        const std::string path = std::string(SDMC) + SERVERCFG_PATH;
        if (!fs::Exists(path))
            return false;
        auto bytes = fs::OpenFile(path);
        std::string text(bytes.begin(), bytes.end());
        return ServerCfg_Parse(text.c_str(), &cfg);
    }
    catch (...)
    {
        return false;
    }
}

static bool SaveServerConfigFile(const ServerConfig &cfg)
{
    char buf[256];
    const size_t n = ServerCfg_Format(&cfg, buf, sizeof(buf));
    if (n == 0)
        return false;
    try
    {
        fs::CreateDir(SDMC "/config/nxremoteplay/");
        fs::WriteFile(std::string(SDMC) + SERVERCFG_PATH, std::vector<u8>(buf, buf + n));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// Validates "host[:port]" (IP or DNS name, kept as typed, port optional -> 7921) into "host:port".
static bool NormalizeServerText(const char *text, std::string &out)
{
    char o[SERVERCFG_TEXT_MAX];
    if (!ServerCfg_Normalize(text, o, sizeof(o)))
        return false;
    out = o;
    return true;
}

// Rendezvous server entry. Shows the server the sysmodule currently
// registers with (read over IPC) in an editable field (system keyboard); Save sends it to the
// sysmodule, which validates it (IPv4 host:port), persists it and re-registers there with the
// same Session ID/Password. The phone app must be set to the same server in its own Settings.
// Later this becomes a "Default | Custom" choice; for now it is just the custom box.
void RenderServer()
{
    static char serverText[64] = "";
    static char customText[64] = ""; // the user's own server, remembered while the toggle is ON
    static bool useDefault = true;   // toggle: ON = default server, OFF = custom text
    static bool loaded = false;
    static bool ipcOk = false;
    static int saveResult = 0; // 0 none, 1 saved, -1 rejected/failed
    if (serverPageFresh)
    {
        loaded = false;
        saveResult = 0;
    }

    if (!loaded)
    {
        loaded = true;
        ipcOk = false;
        // The shared file is the truth (what both programs apply); the sysmodule is only asked whether it is alive.
        ServerConfig fileCfg;
        const bool haveFile = LoadServerConfigFile(fileCfg);
        useDefault = fileCfg.useDefault;
        strncpy(customText, fileCfg.customServer, sizeof(customText) - 1);
        customText[sizeof(customText) - 1] = 0;
        strncpy(serverText, useDefault ? fileCfg.defaultServer : fileCfg.customServer, sizeof(serverText) - 1);
        serverText[sizeof(serverText) - 1] = 0;
        if (R_SUCCEEDED(NxRemotePlayConnect()))
        {
            char cur[64] = {};
            ipcOk = R_SUCCEEDED(NxRemotePlayGetServer(cur));
            if (ipcOk && !haveFile)
            {
                // No file yet (older install): start from whatever the sysmodule is using.
                strncpy(serverText, cur, sizeof(serverText) - 1);
                serverText[sizeof(serverText) - 1] = 0;
                useDefault = strcasecmp(cur, SERVERCFG_BUILTIN_DEFAULT) == 0;
                if (!useDefault)
                {
                    strncpy(customText, cur, sizeof(customText) - 1);
                    customText[sizeof(customText) - 1] = 0;
                }
            }
            NxRemotePlayClose();
        }
    }

    if (BackButton())
        currentPage = Page::Settings;
    DrawNavigationBrand();
    constexpr float panelHeight = 540.0f;
    const float panelY = (UI::WindowHeight - panelHeight) / 2.0f;
    DrawPanel({ScreenPanel::PosX, panelY}, {ScreenPanel::Width, panelHeight});

    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 panelOrigin = ImGui::GetWindowPos();
    constexpr float contentX = ScreenPanel::ContentPadding, fieldWidth = ScreenPanel::ContentWidth;
    constexpr float titleSize = 46.0f * 1.3f;

    float y = contentX;
    {
        const char *title = "Rendezvous server";
        const ImVec2 titleTextSize = TextSize(title, titleSize, true);
        Text(draw, panelOrigin + ImVec2(contentX + (fieldWidth - titleTextSize.x) / 2.0f, y), titleSize, Color(1, 1, 1),
             title, true);
        y += titleTextSize.y + 8.0f;
    }
    y += CenteredParagraph(
        draw, panelOrigin, contentX, y, fieldWidth, 32.0f, Color(.72f, .78f, .92f),
        ipcOk
            ? "Use the default server, or turn it off and enter your own: an IP address or a name, with an optional :port (7921 if none). Set the phone app to the same server."
            : "NX-Remote-Play isn't running - the server can't be read or changed right now.");

    y += 16.0f;
    ImGui::SetCursorPos({contentX, y});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 32);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.16f, .22f, .34f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.19f, .28f, .43f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.12f, .32f, .64f, 1));
    const std::string toggleLabel = std::string("Use default server: ") + (useDefault ? "ON" : "OFF");
    if (serverPageFresh)
    {
        ImGui::SetKeyboardFocusHere();
        serverPageFresh = false;
    }
    const bool togglePressed = ImGui::Button(toggleLabel.c_str(), {fieldWidth, 66});
    const bool toggleSelected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 togglePos = ImGui::GetItemRectMin(), toggleSize = ImGui::GetItemRectSize();
    if (toggleSelected)
        ImGui::GetWindowDrawList()->AddRect(togglePos, togglePos + toggleSize, Color(.42f, .76f, 1), 32,
                                            ImDrawCornerFlags_All, 2);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    if (togglePressed)
    {
        useDefault = !useDefault;
        saveResult = 0;
        ServerConfig toggleCfg;
        LoadServerConfigFile(toggleCfg);
        const char *shown = useDefault ? toggleCfg.defaultServer : customText;
        strncpy(serverText, shown, sizeof(serverText) - 1);
        serverText[sizeof(serverText) - 1] = 0;
    }
    y = ImGui::GetCursorPosY() + 16.0f;

    if (useDefault)
    {
        ServerConfig showCfg;
        LoadServerConfigFile(showCfg);
        const ImVec2 sz = TextSize(showCfg.defaultServer, 34.0f, true);
        Text(draw, panelOrigin + ImVec2(contentX + (fieldWidth - sz.x) / 2.0f, y), 34.0f, Color(.55f, .90f, .70f),
             showCfg.defaultServer, true);
        y += sz.y + 8.0f;
    }
    else
    {
        const ImVec2 labelSize = TextSize("SERVER (IP OR NAME[:PORT])", 28.0f, true);
        Text(draw, panelOrigin + ImVec2(contentX + (fieldWidth - labelSize.x) / 2.0f, y), 28.0f,
             Color(.55f, .60f, .70f), "SERVER (IP OR NAME[:PORT])", true);
        y += labelSize.y + 8.0f;

        ImGui::SetCursorPos({contentX, y});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20, 10));
        ImGui::PushItemWidth(fieldWidth);
        if (InputTextWithKeyboard("##server", serverText, sizeof(serverText), "Server (IP or name[:port])", false))
        {
            strncpy(customText, serverText, sizeof(customText) - 1);
            customText[sizeof(customText) - 1] = 0;
        }
        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
        y = ImGui::GetCursorPosY();
    }

    y += 30.0f;
    ImGui::SetCursorPos({contentX, y});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 32);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.08f, .43f, .96f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.19f, .28f, .43f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.12f, .32f, .64f, 1));
    const bool savePressed = ImGui::Button("Save", {fieldWidth, 66});
    const bool saveSelected = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 savePos = ImGui::GetItemRectMin(), saveSize = ImGui::GetItemRectSize();
    if (saveSelected)
        ImGui::GetWindowDrawList()->AddRect(savePos, savePos + saveSize, Color(.42f, .76f, 1), 32,
                                            ImDrawCornerFlags_All, 2);
    if (savePressed)
    {
        saveResult = -1;
        ServerConfig cfg;
        LoadServerConfigFile(cfg); // keeps the file's "default=" line
        std::string customNorm;
        const bool customValid = NormalizeServerText(serverText, customNorm);
        bool ok = true;
        if (!useDefault)
        {
            ok = customValid;
            if (ok)
            {
                strncpy(cfg.customServer, customNorm.c_str(), sizeof(cfg.customServer) - 1);
                cfg.customServer[sizeof(cfg.customServer) - 1] = 0;
            }
        }
        else
        {
            std::string remembered;
            if (NormalizeServerText(customText, remembered))
            {
                strncpy(cfg.customServer, remembered.c_str(), sizeof(cfg.customServer) - 1);
                cfg.customServer[sizeof(cfg.customServer) - 1] = 0;
            }
        }
        if (ok)
        {
            cfg.useDefault = useDefault;
            // 1) the shared file (the only place the choice is stored); 2) tell the running sysmodule to apply it
            // right now (it would also notice the file by itself within one cycle).
            const bool fileSaved = SaveServerConfigFile(cfg);
            const std::string effective = ServerCfg_Effective(&cfg);
            if (R_SUCCEEDED(NxRemotePlayConnect()))
            {
                char out[64] = {};
                strncpy(out, effective.c_str(), sizeof(out) - 1);
                NxRemotePlaySetServer(out);
                NxRemotePlayClose();
            }

            if (fileSaved)
            {
                saveResult = 1;
                strncpy(g_serverDisplay, effective.c_str(), sizeof(g_serverDisplay) - 1);
                g_serverDisplay[sizeof(g_serverDisplay) - 1] = 0;
                strncpy(serverText, (useDefault ? cfg.defaultServer : cfg.customServer), sizeof(serverText) - 1);
                serverText[sizeof(serverText) - 1] = 0;
                // The NRO's own server use (footer fallback probe, joining) follows the same file immediately.
                const auto colon = effective.rfind(':');
                Network::Rendezvous::SetProbeServer(effective.substr(0, colon),
                                                    (uint16_t)std::atoi(effective.c_str() + colon + 1));
                currentPage = Page::Settings;
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    if (saveResult != 0)
    {
        y = ImGui::GetCursorPosY() + 8.0f;
        CenteredParagraph(
            draw, panelOrigin, contentX, y, fieldWidth, 32.0f,
            saveResult > 0 ? Color(.55f, .90f, .70f) : Color(.95f, .55f, .55f),
            saveResult > 0
                ? "Saved. Re-registering with the new server."
                : "Not saved - enter an IP address or a name, with an optional :port, like play.example.com or 203.0.113.5:7921.");
    }
    EndPanel();
    Footer(false);
}

// Export logs: one button writes LogExport::OutputPath on the SD card; the result stays on screen.
bool g_exportDone = false, g_exportOk = false;
std::string g_exportSummary;

void OpenExportLogs()
{
    g_exportDone = false;
    currentPage = Page::ExportLogs;
}

void RenderExportLogs()
{
    if (BackButton())
        currentPage = Page::Settings;
    DrawNavigationBrand();
    constexpr float panelHeight = 533.0f;
    constexpr float panelY = (UI::WindowHeight - panelHeight) / 2.0f;
    DrawPanel({ScreenPanel::PosX, panelY}, {ScreenPanel::Width, panelHeight});
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    constexpr float contentX = ScreenPanel::ContentPadding, fieldWidth = ScreenPanel::ContentWidth;
    constexpr float titleSize = 46.0f * 1.3f;

    float y = contentX;
    {
        const ImVec2 titleTextSize = TextSize("Export logs", titleSize, true);
        Text(draw, origin + ImVec2(contentX + (fieldWidth - titleTextSize.x) / 2.0f, y), titleSize, Color(1, 1, 1),
             "Export logs", true);
        y += titleTextSize.y + 20.0f;
    }

    ImGui::SetCursorPos({contentX, y});
    {
        ImFont *font = ImGui::GetFont();
        const float savedScale = font->Scale;
        font->Scale = 22.0f / 46.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fieldWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.78f, .82f, .92f, 1));
        ImGui::TextWrapped("Collects this console's logs into one text file on the SD card. Send that file with a "
                           "description of what went wrong. It holds no passwords or Session IDs; the developer and "
                           "connection logs contain network addresses, so look through it before posting it publicly.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        y = ImGui::GetCursorPosY() + 24.0f;
        font->Scale = savedScale;
    }

    {
        const ImVec2 pathSize = TextSize(LogExport::OutputPath, 22, true);
        Text(draw, origin + ImVec2(contentX, y), 22, Color(.82f, .85f, .90f, 1), LogExport::OutputPath, true);
        y += pathSize.y + 16.0f;
    }

    if (g_exportDone)
    {
        const char *headline = g_exportOk ? "Saved to the SD card root." : "Export failed.";
        const ImVec2 sz = TextSize(headline, 24, true);
        Text(draw, origin + ImVec2(contentX, y), 24, g_exportOk ? Color(.3f, .85f, .3f, 1) : Color(.95f, .3f, .3f, 1),
             headline, true);
        y += sz.y + 8.0f;
        const ImVec2 detail = TextSize(g_exportSummary.c_str(), 22, true);
        Text(draw, origin + ImVec2(contentX, y), 22, Color(.82f, .85f, .90f, 1), g_exportSummary.c_str(), true);
        y += detail.y + 16.0f;
    }

    {
        ImFont *font = ImGui::GetFont();
        const float savedScale = font->Scale;
        font->Scale = 18.0f / 46.0f;
        float rowX = contentX;
        const char *labels[] = {"Create log file", "Go back"};
        float rowHeight = 0.0f;
        for (int i = 0; i < 2; ++i)
        {
            const ImVec2 textSz = ImGui::CalcTextSize(labels[i]);
            const ImVec2 btnSz = textSz + ImVec2(24, 14);
            ImGui::SetCursorPos({rowX, y});
            if (ImGui::Button(labels[i], btnSz))
            {
                if (i == 0)
                {
                    g_exportOk = LogExport::Create(g_exportSummary);
                    g_exportDone = true;
                }
                else
                    currentPage = Page::Settings;
            }
            rowX += btnSz.x + 10.0f;
            rowHeight = btnSz.y;
        }
        y += rowHeight;
        font->Scale = savedScale;
    }
    EndPanel();
    Footer(false);
}

// Updates: the latest check's result, the permanent releases link and a Check now button. The same page is the
// once-per-start announcement when a newer release is found (it then returns to Home).
bool g_updatesAnnounce = false, g_updatesAnnounced = false;

void OpenUpdates(bool announce)
{
    g_updatesAnnounce = announce;
    currentPage = Page::Updates;
}

void CloseUpdates()
{
    currentPage = g_updatesAnnounce ? Page::Home : Page::Settings;
}

void RenderUpdates()
{
    using Network::UpdateCheck::State;
    if (BackButton())
        CloseUpdates();
    DrawNavigationBrand();
    constexpr float panelHeight = 533.0f;
    constexpr float panelY = (UI::WindowHeight - panelHeight) / 2.0f;
    DrawPanel({ScreenPanel::PosX, panelY}, {ScreenPanel::Width, panelHeight});
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    constexpr float contentX = ScreenPanel::ContentPadding, fieldWidth = ScreenPanel::ContentWidth;
    constexpr float titleSize = 46.0f * 1.3f;
    const State state = Network::UpdateCheck::Current();

    float y = contentX;
    {
        const char *title = g_updatesAnnounce ? "Update available" : "Updates";
        const ImVec2 titleTextSize = TextSize(title, titleSize, true);
        Text(draw, origin + ImVec2(contentX + (fieldWidth - titleTextSize.x) / 2.0f, y), titleSize, Color(1, 1, 1),
             title, true);
        y += titleTextSize.y + 24.0f;
    }

    const std::string installed = std::string("Installed version: ") + NXRP_APP_VERSION;
    Text(draw, origin + ImVec2(contentX, y), 24, Color(.82f, .85f, .90f, 1), installed.c_str(), true);
    y += TextSize(installed.c_str(), 24, true).y + 12.0f;

    {
        std::string line;
        ImU32 color = Color(.82f, .85f, .90f, 1);
        switch (state)
        {
        case State::Available:
            line = "Version " + Network::UpdateCheck::Latest() + " is available.";
            color = Color(.3f, .85f, .3f, 1);
            break;
        case State::UpToDate:
            line = "This is the newest version.";
            break;
        case State::Failed:
            line = "Could not reach GitHub to check for updates.";
            color = Color(.95f, .3f, .3f, 1);
            break;
        default:
            line = Network::UpdateCheck::StatusText();
            break;
        }
        Text(draw, origin + ImVec2(contentX, y), 26, color, line.c_str(), true);
        y += TextSize(line.c_str(), 26, true).y + 24.0f;
    }

    {
        ImFont *font = ImGui::GetFont();
        const float savedScale = font->Scale;
        font->Scale = 22.0f / 46.0f;
        ImGui::SetCursorPos({contentX, y});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fieldWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.78f, .82f, .92f, 1));
        ImGui::TextWrapped("Every version is on the releases page. Open it on a phone or computer to download an "
                           "update, then copy its Switch SD Card Files to the SD card.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        y = ImGui::GetCursorPosY() + 16.0f;
        font->Scale = savedScale;
    }

    Text(draw, origin + ImVec2(contentX, y), 24, Color(.42f, .76f, 1, 1), Network::UpdateCheck::ReleasesPage, true);
    y += TextSize(Network::UpdateCheck::ReleasesPage, 24, true).y + 24.0f;

    {
        ImFont *font = ImGui::GetFont();
        const float savedScale = font->Scale;
        font->Scale = 18.0f / 46.0f;
        float rowX = contentX;
        const char *labels[] = {"Check now", g_updatesAnnounce ? "Later" : "Go back"};
        for (int i = 0; i < 2; ++i)
        {
            const ImVec2 btnSz = ImGui::CalcTextSize(labels[i]) + ImVec2(24, 14);
            ImGui::SetCursorPos({rowX, y});
            if (ImGui::Button(labels[i], btnSz))
            {
                if (i == 1)
                    CloseUpdates();
                else if (state != State::Checking)
                    Network::UpdateCheck::Start();
            }
            rowX += btnSz.x + 10.0f;
        }
        font->Scale = savedScale;
    }
    EndPanel();
    Footer(false);
}

void RenderSettings()
{
    std::snprintf(g_updateDisplay, sizeof(g_updateDisplay), "%s", Network::UpdateCheck::StatusText());
    const int activated = UpdateSettingsController();
    if (activated == -1)
    {
        currentPage = Page::Home;
        return;
    }
    if (activated == 0)
    {
        serverPageFresh = true;
        currentPage = Page::Server;
        return;
    }
    if (activated == 1)
    {
        RefreshSystemPatchStatus();
        currentPage = Page::SystemPatches;
        return;
    }
    if (activated == 2)
    {
        OpenExportLogs();
        return;
    }
    if (activated == 3)
    {
        OpenUpdates(false);
        return;
    }
    if (BackButton())
        currentPage = Page::Home;
    DrawNavigationBrand();
    // Vertically centered on the 720px canvas, matching Connect/Host's centering
    // rather than pinned to the old fixed top inset.
    constexpr float panelY = (UI::WindowHeight - ScreenPanel::Height) / 2.0f;
    DrawPanel({ScreenPanel::PosX, panelY}, {ScreenPanel::Width, ScreenPanel::Height});
    constexpr float contentX = ScreenPanel::ContentPadding, fieldWidth = ScreenPanel::ContentWidth;
    constexpr float titleSize = 46.0f * 1.3f;
    float listTop;
    {
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 titleTextSize = TextSize("Settings", titleSize, true);
        Text(draw, origin + ImVec2(contentX + (fieldWidth - titleTextSize.x) / 2.0f, contentX), titleSize,
             Color(1, 1, 1), "Settings", true);
        listTop = contentX + titleTextSize.y + 24.0f;
    }

    constexpr float parentHeight = SettingsParentHeight, detailHeight = SettingsDetailHeight, rowWidth = fieldWidth;
    const float contentHeight = SettingsLayoutHeight();
    const float listHeight = ScreenPanel::ContentBottom - listTop;
    ImGui::SetCursorPos({contentX, listTop});
    ImGui::SetNextWindowContentSize({0, contentHeight});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("SettingsList", {rowWidth, listHeight}, false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoNavInputs);
    if (settingsFocusDirty)
    {
        // Keeps the focused row centered in the visible list area - the same fraction
        // of the visible height (171/378) as the original tuning.
        float target = settingsFocus < 0 ? 0.0f : SettingsRowY(settingsFocus) - listHeight * (171.0f / 378.0f);
        const float maxScroll = contentHeight - listHeight;
        if (target < 0.0f)
            target = 0.0f;
        if (target > maxScroll && maxScroll > 0.0f)
            target = maxScroll;
        ImGui::SetWindowScrollY(ImGui::GetCurrentWindow(), target);
        settingsFocusDirty = false;
    }
    float y = 0.0f;
    int controlIndex = 0;
    for (int g = 0; g < SettingsGroupCount; ++g)
    {
        const SettingGroup &group = SettingsGroups[g];
        ImGui::SetCursorPos({0, y});
        ImGui::Dummy({rowWidth, parentHeight});
        const ImVec2 p = ImGui::GetItemRectMin();
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(p, p + ImVec2(rowWidth, parentHeight), Color(.15f, .20f, .30f, .96f), 14);
        draw->AddRect(p, p + ImVec2(rowWidth, parentHeight), Color(.38f, .46f, .58f, .35f), 14, ImDrawCornerFlags_All,
                      1);
        const ImVec2 nameSize = TextSize(group.name, SettingsGroupNameSize, true);
        Text(draw, p + ImVec2(35.2f, (parentHeight - nameSize.y) / 2), SettingsGroupNameSize, Color(1, 1, 1),
             group.name, true);
        y += parentHeight + SettingsHeaderGap;
        for (int option = 0; option < group.count; ++option)
        {
            const SettingItem &item = group.items[option];
            ImGui::SetCursorPos({0, y});
            ImGui::InvisibleButton(item.label, {rowWidth, detailHeight});
            if (ImGui::IsItemClicked())
            {
                settingsControllerMode = false;
                if (settingsFocus != controlIndex)
                {
                    settingsFocus = controlIndex;
                    settingsFocusDirty = true;
                }
                if (&item == &SettingsActions[0])
                {
                    RefreshSystemPatchStatus();
                    currentPage = Page::SystemPatches;
                }
                if (&item == &SettingsActions[1])
                    OpenExportLogs();
                if (&item == &SettingsAbout[0])
                    OpenUpdates(false);
                if (&item == &SettingsConnection[0])
                {
                    serverPageFresh = true;
                    currentPage = Page::Server;
                }
            }
            const ImVec2 detailPos = ImGui::GetItemRectMin();
            const bool selected = ImGui::IsItemHovered() || settingsFocus == controlIndex;
            const ImVec2 detailSize(rowWidth, detailHeight);
            draw->AddRectFilled(detailPos, detailPos + detailSize,
                                selected ? Color(.09f, .17f, .30f, .98f) : Color(.07f, .12f, .22f, .96f), 9);
            if (selected)
                draw->AddRect(detailPos, detailPos + detailSize, Color(.42f, .76f, 1), 9, ImDrawCornerFlags_All, 2);
            const ImVec2 detailLabelSize = TextSize(item.label, SettingsItemTextSize, true);
            const ImVec2 detailValueSize = TextSize(item.value, SettingsItemTextSize, true);
            Text(draw, detailPos + ImVec2(28.8f, (detailHeight - detailLabelSize.y) / 2), SettingsItemTextSize,
                 Color(.78f, .84f, .96f), item.label, true);
            Text(draw,
                 detailPos + ImVec2(detailSize.x - 28.8f - detailValueSize.x, (detailHeight - detailValueSize.y) / 2),
                 SettingsItemTextSize, Color(.82f, .89f, 1), item.value, true);
            y += detailHeight + SettingsRowGap;
            ++controlIndex;
        }
        y += SettingsGroupGap;
    }
    ImGui::SetCursorPos({0, y});
    ImGui::Dummy({1, 1});
    ImGui::EndChild();
    ImGui::PopStyleColor();
    EndPanel();
    Footer(false);
}

void RenderSystemPatches()
{
    if (BackButton())
        currentPage = Page::Settings;
    DrawNavigationBrand();
    // Own panel height, sized to this screen's own content (title + paragraph + status
    // lines + buttons) rather than the Settings screen's shared height - this content
    // isn't in a scrollable list, so it must fit outright.
    constexpr float panelHeight = 533.0f;
    constexpr float panelY = (UI::WindowHeight - panelHeight) / 2.0f;
    DrawPanel({ScreenPanel::PosX, panelY}, {ScreenPanel::Width, panelHeight});
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    constexpr float contentX = ScreenPanel::ContentPadding, fieldWidth = ScreenPanel::ContentWidth;
    constexpr float titleSize = 46.0f * 1.3f;

    float y = contentX;
    {
        const ImVec2 titleTextSize = TextSize("System patches", titleSize, true);
        Text(draw, origin + ImVec2(contentX + (fieldWidth - titleTextSize.x) / 2.0f, y), titleSize, Color(1, 1, 1),
             "System patches", true);
        y += titleTextSize.y + 20.0f;
    }

    ImGui::SetCursorPos({contentX, y});
    {
        ImFont *font = ImGui::GetFont();
        const float savedScale = font->Scale;
        font->Scale = 22.0f / 46.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fieldWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.78f, .82f, .92f, 1));
        ImGui::TextWrapped("%s", SystemPatchesDescription);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        y = ImGui::GetCursorPosY() + 24.0f;
        font->Scale = savedScale;
    }

    {
        const char *statusLabel = "Patches status:";
        const ImVec2 labelSize = TextSize(statusLabel, 24, true);
        Text(draw, origin + ImVec2(contentX, y), 24, Color(1, 1, 1), statusLabel, true);
        if (systemPatchFileCount > 0)
            Text(draw, origin + ImVec2(contentX + labelSize.x + 10.0f, y), 24, Color(.3f, .85f, .3f, 1), "installed",
                 true);
        else
            Text(draw, origin + ImVec2(contentX + labelSize.x + 10.0f, y), 24, Color(.95f, .3f, .3f, 1),
                 "not installed", true);
        y += labelSize.y + 16.0f;
    }

    {
        const ImVec2 pathSize = TextSize(SystemPatchesSdcardPath, 22, true);
        Text(draw, origin + ImVec2(contentX, y), 22, Color(.82f, .85f, .90f, 1), SystemPatchesSdcardPath, true);
        y += pathSize.y + 28.0f;
    }

    {
        // Both buttons sit together in one small row at the bottom, instead
        // of "Uninstall" floating up by the sdcard path - smaller and more compact,
        // which is also most of why this screen no longer needs as tall a panel.
        const char *labels[] = {"Go back"};
        ImFont *font = ImGui::GetFont();
        const float savedScale = font->Scale;
        font->Scale = 18.0f / 46.0f;
        float rowX = contentX;
        for (int i = 0; i < 1; ++i)
        {
            const ImVec2 textSz = ImGui::CalcTextSize(labels[i]);
            const ImVec2 btnSz = textSz + ImVec2(24, 14);
            ImGui::SetCursorPos({rowX, y});
            if (ImGui::Button(labels[i], btnSz))
                currentPage = Page::Settings;
            rowX += btnSz.x + 10.0f;
            y += textSz.y + 14.0f;
        }
        font->Scale = savedScale;
    }
    EndPanel();
    Footer(false);
}
} // namespace

void scenes::InitHost()
{
    Network::Rendezvous::LoadServerConfig();
    Network::UpdateCheck::Start();
    // The sysmodule is the source of truth for which server the console registers with: use
    // it for the footer's reachability probe and the Settings row from the very first frame.
    // (Falls back to the SD config above if NxRemotePlay isn't running.)
    if (R_SUCCEEDED(NxRemotePlayConnect()))
    {
        char cur[64] = {};
        bool applied = false;
        if (R_SUCCEEDED(NxRemotePlayGetServer(cur)) && cur[0])
        {
            // Only trust a complete "host:port" - an old sysmodule build answered ":0" when the
            // wake feature wasn't configured (, .198), which used to replace a good
            // server with nothing and make the footer/Connect fail.
            std::string hp = cur;
            auto colon = hp.rfind(':');
            if (colon != std::string::npos && colon > 0)
            {
                const int probePort = std::atoi(hp.c_str() + colon + 1);
                const std::string host = hp.substr(0, colon);
                if (probePort > 0 && probePort <= 65535 && !host.empty())
                {
                    strncpy(g_serverDisplay, cur, sizeof(g_serverDisplay) - 1);
                    g_serverDisplay[sizeof(g_serverDisplay) - 1] = 0;
                    Network::Rendezvous::SetProbeServer(host, (uint16_t)probePort);
                    applied = true;
                }
            }
        }
        NxRemotePlayClose();
        if (!applied)
        {
            // Fall back to what LoadServerConfig read from the SD card (server.txt or the built-in default).
            std::string host;
            uint16_t port = 0;
            if (Network::Rendezvous::GetServerConfig(host, port))
            {
                const std::string shown = host + ":" + std::to_string(port);
                strncpy(g_serverDisplay, shown.c_str(), sizeof(g_serverDisplay) - 1);
                g_serverDisplay[sizeof(g_serverDisplay) - 1] = 0;
            }
        }
    }
    hostThisSwitchIcon = Image::Img(ASSET("host_this_switch.png"));
    connectJoyConIcon = Image::Img(ASSET("connect_joycon_wireless.png"));
    appLogo = Image::Img(ASSET("app_logo.png"));
}

void scenes::Host()
{
    SetupMainWindow("Remote Link");
    DrawBackground();

    // hoisted from RenderConnect/RenderConnectorPlayback's own
    // duplicate call sites (removed below) - Poll() drives the Rendezvous-
    // server reachability ping (Footer()'s status indicator) among other
    // things, and that needs to run on every screen, not just while actually
    // on the Connect/Playback pages, or the indicator only ever updates when
    // you happen to be looking at the one screen that used to poll it (real
    // user report: "it doesn't update on the main screen or any other
    // screen"). Single call site now - safe to call unconditionally regardless
    // of state or which page is showing.
    Network::Rendezvous::Connector::Poll();

    // Closes RenderHost()'s held-open IPC session the frame after the user
    // actually leaves the Host screen (one frame late is harmless - nothing
    // else needs the local "nxremoteplay" service instantly). Checked before the
    // switch below since currentPage may have just changed to Host itself
    // this same frame (from a Home card press), which must NOT trigger a
    // close.
    static Page previousPage = Page::Home;
    // Leaving the playback screen by any route (including the global B handler below) ends the
    // punched connection.
    if (previousPage == Page::ConnectorPlayback && currentPage != Page::ConnectorPlayback)
    {
        g_streamMenuOpen = false;
        Network::Rendezvous::Connector::Disconnect();
    }
    if (previousPage == Page::Host && currentPage != Page::Host && g_hostingConnected)
    {
        NxRemotePlayClose();
        g_hostingConnected = false;
    }

    // The footer promises "B Back" on every non-Home screen, but nothing ever
    // read the Cancel input to honor it - wire it here rather than per-screen,
    // same edge-triggered pattern as Settings' own Activate/Back handling.
    // SystemPatches returns to Settings (its own "Go back" target), everything else to Home.
    // Excludes ConnectorPlayback while passthroughMode is engaged (
    // bugfix, see passthroughMode's own comment above) - otherwise this handler
    // unconditionally exits the Playback screen on B before
    // RenderConnectorPlayback() ever gets a chance to forward that same B press
    // to the Host instead.
    static bool backHeld = false;
    const bool backPressed = ImGui::GetIO().NavInputs[ImGuiNavInput_Cancel] > 0.5f;
    const bool backSuppressed = (currentPage == Page::ConnectorPlayback) && (passthroughMode || g_streamMenuOpen);
    if (backPressed && !backHeld && currentPage != Page::Home && !backSuppressed)
    {
        if (currentPage == Page::Updates)
            CloseUpdates();
        else
            currentPage = (currentPage == Page::SystemPatches || currentPage == Page::Server || currentPage == Page::ExportLogs)
                              ? Page::Settings
                              : Page::Home;
    }
    backHeld = backPressed;
    // A newer release is announced once per start, on the Home screen (never over a connection or a stream).
    if (!g_updatesAnnounced && currentPage == Page::Home &&
        Network::UpdateCheck::Current() == Network::UpdateCheck::State::Available)
    {
        g_updatesAnnounced = true;
        OpenUpdates(true);
    }
    switch (currentPage)
    {
    case Page::Home:
        RenderHome();
        break;
    case Page::Connect:
        RenderConnect();
        break;
    case Page::Host:
        RenderHost();
        break;
    case Page::Settings:
        RenderSettings();
        break;
    case Page::SystemPatches:
        RenderSystemPatches();
        break;
    case Page::ConnectorPlayback:
        RenderConnectorPlayback();
        break;
    case Page::Server:
        RenderServer();
        break;
    case Page::ExportLogs:
        RenderExportLogs();
        break;
    case Page::Updates:
        RenderUpdates();
        break;
    }
    previousPage = currentPage;
    ImGui::End();
}
