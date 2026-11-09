using ImGuiNET;
using System;
using System.Numerics;

namespace NxRemotePlay.Client.GUI;

// Text entry in the app's own style: rounded navy boxes with an accent border, a keyboard button on each box, a settings
// edit card, and an on-screen keyboard (drawn here, driven by a mouse, touch or a controller) for use without a keyboard.
// Pressing A on a box with a controller opens the keyboard by itself. All of it is laid out on the 1280x720 design grid.
internal sealed partial class DesktopView
{
    int kbTarget;                  // 1 = Session ID, 2 = Password, 3 = value of the settings row being edited
    bool kbOpenRequest, kbShift;
    string kbOriginal = "";
    static readonly string[] KbRows = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM.:_" };

    string KbValue
    {
        get => kbTarget == 1 ? id : kbTarget == 2 ? password : editValue;
        set { if (kbTarget == 1) id = value; else if (kbTarget == 2) password = value; else editValue = value; }
    }
    bool KbRestricted => kbTarget is 1 or 2;            // Session ID / password: five or six letters and digits, upper case
    int KbMax => KbRestricted ? 6 : 512;
    string KbTitle => kbTarget == 1 ? "Session ID" : kbTarget == 2 ? "Password" : editLabel;

    void OpenKeyboard(int target)
    {
        kbTarget = target; kbOriginal = KbValue; kbShift = false; kbOpenRequest = true;
    }

    // One text box (design coordinates). Returns nothing: the value is edited in place.
    void InputBox(string imguiId, ref string value, float x, float y, float width, bool hidden, uint max, int target)
    {
        ImGui.SetCursorScreenPos(At(x, y));
        ImGui.SetNextItemWidth((width - 74) * scale);
        ImGui.PushFont(Owner.DesktopFont);
        ImGui.SetWindowFontScale(30 * scale / (64 * ImGui.GetIO().FontGlobalScale));
        ImGui.PushStyleColor(ImGuiCol.FrameBg, new Vector4(.075f, .115f, .22f, 1));
        ImGui.PushStyleColor(ImGuiCol.FrameBgHovered, new Vector4(.095f, .15f, .28f, 1));
        ImGui.PushStyleColor(ImGuiCol.FrameBgActive, new Vector4(.10f, .17f, .32f, 1));
        ImGui.PushStyleColor(ImGuiCol.Border, new Vector4(.30f, .45f, .72f, .85f));
        ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1, 1, 1, 1));
        ImGui.PushStyleColor(ImGuiCol.TextSelectedBg, new Vector4(.12f, .50f, .95f, .55f));
        ImGui.PushStyleVar(ImGuiStyleVar.FramePadding, new Vector2(18, 12) * scale);
        ImGui.PushStyleVar(ImGuiStyleVar.FrameRounding, 16 * scale);
        ImGui.PushStyleVar(ImGuiStyleVar.FrameBorderSize, 1.6f * scale);
        ImGui.InputText(imguiId, ref value, max, hidden ? ImGuiInputTextFlags.Password : ImGuiInputTextFlags.None);
        bool activated = ImGui.IsItemActivated(), active = ImGui.IsItemActive();
        var rect = ImGui.GetItemRectSize();
        ImGui.PopStyleVar(3); ImGui.PopStyleColor(6);
        ImGui.SetWindowFontScale(1); ImGui.PopFont();
        if (active) draw.AddRect(At(x, y) - new Vector2(2 * scale), At(x, y) + new Vector2(width - 74, 0) * scale + new Vector2(0, rect.Y) + new Vector2(2 * scale), Color(.42f, .76f, 1), 18 * scale, ImDrawFlags.RoundCornersAll, 2.4f * scale);
        // Controller: A on the box opens the on-screen keyboard (the mouse and a keyboard just type).
        if (activated && ImGui.GetIO().NavVisible) OpenKeyboard(target);
        // keyboard button at the right end of the box
        float kx = x + width - 62, kh = rect.Y / scale;
        ImGui.SetCursorScreenPos(At(kx, y));
        bool pressed = ImGui.InvisibleButton(imguiId + "kbd", new Vector2(62, kh) * scale);
        bool hot = ImGui.IsItemHovered() || ImGui.IsItemFocused();
        draw.AddRectFilled(At(kx, y), At(kx + 62, y + kh), hot ? Color(.17f, .24f, .40f) : Color(.105f, .15f, .27f), 16 * scale);
        draw.AddRect(At(kx, y), At(kx + 62, y + kh), hot ? Color(.45f, .78f, 1f, .9f) : Color(.30f, .45f, .72f, .85f), 16 * scale, ImDrawFlags.RoundCornersAll, 1.6f * scale);
        Glyph("kbd", At(kx + 31, y + kh / 2), 30 * scale, White);
        if (pressed) OpenKeyboard(target);
    }

    void Field(string label, ref string value, float x, float y, float width, bool hidden = false, uint max = 128)
    {
        Text(label, x, y, 28, true);
        int target = label == "Session ID" ? 1 : label == "Password" ? 2 : 0;
        InputBox("##" + label, ref value, x, y + 42, width, hidden || target != 0 && Program.Options.StreamerMode, max, target);
    }

    // ---- settings edit card ----
    void EditPopup()
    {
        var size = ImGui.GetIO().DisplaySize;
        ImGui.SetNextWindowPos(Vector2.Zero); ImGui.SetNextWindowSize(size);
        ImGui.PushStyleColor(ImGuiCol.WindowBg, new Vector4(0, 0, 0, 0));
        ImGui.PushStyleVar(ImGuiStyleVar.WindowBorderSize, 0); ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        bool open = ImGui.BeginPopupModal("Edit setting", ImGuiWindowFlags.NoDecoration | ImGuiWindowFlags.NoMove | ImGuiWindowFlags.NoSavedSettings | ImGuiWindowFlags.NoScrollbar);
        ImGui.PopStyleVar(2); ImGui.PopStyleColor();
        if (!open) { Osk(); return; }
        var saved = draw; draw = ImGui.GetWindowDrawList();
        draw.AddRectFilled(At(240, 190), At(1040, 510), Color(.06f, .09f, .17f, .985f), 30 * scale);
        draw.AddRect(At(240, 190), At(1040, 510), Color(.30f, .45f, .72f, .7f), 30 * scale, ImDrawFlags.RoundCornersAll, 2 * scale);
        Text(editLabel, 282, 222, 30, true);
        Text("Type with a keyboard, or press the keyboard button.", 282, 268, 21, false, Muted);
        InputBox("##edit", ref editValue, 282, 312, 716, false, 512, 3);
        if (error.StartsWith("Invalid setting", StringComparison.Ordinal)) Text(error, 282, 386, 21, false, Color(1, .65f, .59f));
        bool kbOpen = ImGui.IsPopupOpen("OnScreenKeyboard");
        if (Button("Save", 282, 418, 220, 62, 30, true, primary: true))
        {
            try { applyEdit?.Invoke(editValue); SaveSettings(); error = ""; ImGui.CloseCurrentPopup(); }
            catch (Exception ex) { error = "Invalid setting: " + ex.Message; }
        }
        if (Button("Cancel", 522, 418, 200, 62, 30)) { error = ""; ImGui.CloseCurrentPopup(); }
        if (!kbOpen && ImGui.IsKeyPressed(ImGuiKey.Escape)) { error = ""; ImGui.CloseCurrentPopup(); }
        Osk();
        draw = saved;
        ImGui.EndPopup();
    }

    // ---- on-screen keyboard ----
    void Osk()
    {
        if (kbOpenRequest) { ImGui.OpenPopup("OnScreenKeyboard"); kbOpenRequest = false; }
        var size = ImGui.GetIO().DisplaySize;
        ImGui.SetNextWindowPos(Vector2.Zero); ImGui.SetNextWindowSize(size);
        ImGui.PushStyleColor(ImGuiCol.WindowBg, new Vector4(0, 0, 0, 0));
        ImGui.PushStyleVar(ImGuiStyleVar.WindowBorderSize, 0); ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        bool open = ImGui.BeginPopupModal("OnScreenKeyboard", ImGuiWindowFlags.NoDecoration | ImGuiWindowFlags.NoMove | ImGuiWindowFlags.NoSavedSettings | ImGuiWindowFlags.NoScrollbar);
        ImGui.PopStyleVar(2); ImGui.PopStyleColor();
        if (!open) return;
        var saved = draw; draw = ImGui.GetWindowDrawList();
        bool restricted = KbRestricted;
        // panel + the text being typed
        draw.AddRectFilled(At(110, 226), At(1170, 704), Color(.05f, .08f, .16f, .99f), 30 * scale);
        draw.AddRect(At(110, 226), At(1170, 704), Color(.30f, .45f, .72f, .7f), 30 * scale, ImDrawFlags.RoundCornersAll, 2 * scale);
        Text(KbTitle, 140, 240, 22, true, Muted);
        Text(restricted ? "Letters and digits only" : "", 1140 - Measure("Letters and digits only", 20).X, 242, 20, false, Muted);
        draw.AddRectFilled(At(140, 276), At(1140, 338), Color(.075f, .115f, .22f), 18 * scale);
        draw.AddRect(At(140, 276), At(1140, 338), Color(.42f, .76f, 1), 18 * scale, ImDrawFlags.RoundCornersAll, 2 * scale);
        string shown = KbValue ?? "";
        if (restricted && Program.Options.StreamerMode) shown = new string('•', shown.Length);
        else if (!restricted && shown.Length > 44) shown = "…" + shown[^43..];
        Text(shown, 162, 307 - Measure("Ag", 32).Y / 2, 32, true);
        if (((int)(ImGui.GetTime() * 2) & 1) == 0)
        {
            float caret = 162 + Measure(shown, 32, true).X + 3;
            draw.AddRectFilled(At(caret, 288), At(caret + 3, 326), White);
        }

        // keys
        const float kw = 82, kh = 60, gap = 8, x0 = 140;
        float y = 354;
        bool Key(string label, float x, float w, bool blue = false, bool disabled = false, bool primary = false)
        {
            bool hit = Button(label, x, y, w, kh, 30, blue, primary);
            if (disabled) draw.AddRectFilled(At(x, y), At(x + w, y + kh), Color(.03f, .05f, .10f, .62f), 24 * scale);
            return hit && !disabled;
        }
        for (int row = 0; row < KbRows.Length; row++)
        {
            string letters = KbRows[row];
            for (int i = 0; i < letters.Length; i++)
            {
                char c = letters[i];
                bool symbol = !char.IsLetterOrDigit(c);
                bool upper = restricted || kbShift;
                string label = char.IsLetter(c) && !upper ? char.ToLowerInvariant(c).ToString() : c.ToString();
                if (Key(label, x0 + i * (kw + gap), kw, false, restricted && symbol, row == 2 && i == 0))
                {
                    string value = KbValue ?? "";
                    if (value.Length < KbMax) KbValue = value + label;
                }
            }
            float wide = x0 + 10 * (kw + gap), wideW = 1140 - wide;
            switch (row)
            {
                case 0: if (Key("Del", wide, wideW)) { string v = KbValue ?? ""; if (v.Length > 0) KbValue = v[..^1]; } break;
                case 1: if (Key("Clear", wide, wideW)) KbValue = ""; break;
                case 2: if (Key("Shift", wide, wideW, kbShift && !restricted, restricted)) kbShift = !kbShift; break;
                case 3:
                    if (Key("Paste", wide, wideW))
                    {
                        string clip = ImGui.GetClipboardText() ?? "";
                        var filtered = new System.Text.StringBuilder();
                        foreach (char c in clip) { if (restricted ? char.IsLetterOrDigit(c) && c < 128 : c >= 32) filtered.Append(restricted ? char.ToUpperInvariant(c) : c); }
                        string value = KbValue ?? "";
                        KbValue = (value + filtered).Substring(0, Math.Min(KbMax, (value + filtered).Length));
                    }
                    break;
            }
            y += kh + gap;
        }
        if (Key("Cancel", 140, 200) | ImGui.IsKeyPressed(ImGuiKey.Escape) | ImGui.IsKeyPressed(ImGuiKey.GamepadFaceRight)) { KbValue = kbOriginal; ImGui.CloseCurrentPopup(); }
        if (Key("Space", 348, 580, false, restricted)) { string v = KbValue ?? ""; if (v.Length < KbMax) KbValue = v + " "; }
        if (Key("Done", 936, 204, true) | ImGui.IsKeyPressed(ImGuiKey.Enter) | ImGui.IsKeyPressed(ImGuiKey.KeypadEnter)) ImGui.CloseCurrentPopup();
        draw = saved;
        ImGui.EndPopup();
    }
}
