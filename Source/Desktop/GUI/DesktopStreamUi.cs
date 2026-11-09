using ImGuiNET;
using NxRemotePlay.Client.Platform;
using NxRemotePlay.Client.Platform.Specific.Win;
using System;
using System.Collections.Generic;
using System.IO;
using System.Numerics;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.GUI;

// The stream screen: top bar, framed viewport that fills the remaining space, session panel, and a bottom bar that carries
// the local/public addresses and the live stream numbers. Laid out in window pixels (not the 1280x720 design grid) so the
// viewport grows and shrinks with the window; `u` scales type and controls to the window size. Hide UI leaves only a
// semi-transparent, movable button that brings the interface back.
internal sealed partial class DesktopView
{
    float u = 1;
    bool showPanel = true;
    bool streamSettings;                          // Settings drawn over the stream (Back returns to the stream)
    Vector2 floatPos = new(-1, -1), floatStart;
    bool floatMoved;
    readonly List<Toast> toasts = new();
    readonly record struct Toast(string Text, string Detail, uint Accent, double Until, string? Group = null);
    double recordingStart;
    string recordingPath = "";
    Task<string?>? folderPick;
    Action<string>? folderApply;
    bool panelVisible;                            // the session panel is on screen this frame (screenshot / recording notices go there)
    (string Text, string Detail, uint Accent, double Until) sidebarNotice;   // the card under Disconnect
    (string Text, string Detail, uint Accent, double Until, string? Group) topNotice;   // USB events while streaming: the space in the top bar

    static readonly uint BgDark = Color(.030f, .045f, .105f), PanelFill = Color(.065f, .095f, .175f, .98f),
        EdgeLine = Color(.30f, .40f, .62f, .55f), AccentBlue = Color(.12f, .62f, .98f), Soft = Color(.88f, .92f, 1f),
        Good = Color(.20f, .85f, .40f), Warn = Color(1f, .72f, .25f), Bad = Color(1f, .35f, .38f), Dim = Color(.62f, .69f, .84f);

    // ---- text helpers (window pixels) ----
    // Sizes passed in are readable-pixel sizes; this font's letters are about half its nominal size, so scale up.
    const float TextScale = 1.34f;
    void Tx(string s, float x, float y, float size, bool bold = false, uint? col = null)
    {
        if (string.IsNullOrEmpty(s)) return;
        draw.AddText(bold ? Owner.DesktopBold : Owner.DesktopFont, size * u * TextScale, new Vector2(x, y), col ?? White, s);
    }
    float Tw(string s, float size, bool bold = false) => Measure(s, size, bold).X * u * TextScale;
    // Vertically centred on cy.
    void TxC(string s, float x, float cy, float size, bool bold = false, uint? col = null) => Tx(s, x, cy - size * u * TextScale * .5f, size, bold, col);
    string Fit(string s, float size, bool bold, float maxPx)
    {
        if (string.IsNullOrEmpty(s) || Tw(s, size, bold) <= maxPx) return s;
        while (s.Length > 1 && Tw(s + "…", size, bold) > maxPx) s = s[..^1];
        return s + "…";
    }
    string Sens(string s) => Program.Options.StreamerMode && !string.IsNullOrEmpty(s) ? "••••••" : s;
    string Hidden(string s)
    {
        if (!Program.Options.StreamerMode || string.IsNullOrEmpty(s)) return s;
        if (usbSource?.SessionId is { Length: > 0 } id) s = s.Replace(id, "••••••");
        return s;
    }

    // ---- small vector glyphs ----
    void Glyph(string kind, Vector2 c, float s, uint col)
    {
        float r = s / 2, t = Math.Max(1.6f, s * .09f);
        Vector2 P(float x, float y) => c + new Vector2(x, y) * r;
        switch (kind)
        {
            case "full":
                foreach (var (sx, sy) in new[] { (-1, -1), (1, -1), (1, 1), (-1, 1) })
                {
                    draw.PathLineTo(P(sx * .85f, sy * .35f)); draw.PathLineTo(P(sx * .85f, sy * .85f)); draw.PathLineTo(P(sx * .35f, sy * .85f));
                    draw.PathStroke(col, ImDrawFlags.None, t);
                }
                break;
            case "cam":
                draw.AddRect(P(-.9f, -.5f), P(.9f, .7f), col, r * .25f, ImDrawFlags.RoundCornersAll, t);
                draw.AddRectFilled(P(-.35f, -.78f), P(.35f, -.5f), col, r * .08f);
                draw.AddCircle(P(0, .1f), r * .38f, col, 20, t);
                break;
            case "rec":
                draw.AddCircle(c, r * .88f, col, 28, t);
                draw.AddCircleFilled(c, r * .52f, col, 24);
                break;
            case "recfill": draw.AddCircleFilled(c, r * .8f, col, 24); break;
            case "stop":
                draw.AddCircle(c, r * .88f, col, 28, t);
                draw.AddRectFilled(P(-.4f, -.4f), P(.4f, .4f), col, r * .08f);
                break;
            case "rot":
                draw.PathArcTo(c, r * .72f, -MathF.PI * .1f, MathF.PI * 1.45f, 28); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.AddTriangleFilled(P(.95f, -.18f), P(.45f, -.28f), P(.82f, -.72f), col);
                break;
            case "home":
                draw.PathLineTo(P(-.85f, -.05f)); draw.PathLineTo(P(0, -.85f)); draw.PathLineTo(P(.85f, -.05f)); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.PathLineTo(P(-.6f, -.15f)); draw.PathLineTo(P(-.6f, .8f)); draw.PathLineTo(P(.6f, .8f)); draw.PathLineTo(P(.6f, -.15f)); draw.PathStroke(col, ImDrawFlags.None, t);
                break;
            case "power":
                draw.PathArcTo(c, r * .72f, -MathF.PI * .32f, MathF.PI * 1.32f, 28); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.AddLine(P(0, -.95f), P(0, -.1f), col, t);
                break;
            case "eye":
            case "eyeoff":
                draw.PathLineTo(P(-.95f, 0)); draw.PathBezierCubicCurveTo(P(-.5f, -.75f), P(.5f, -.75f), P(.95f, 0), 16);
                draw.PathBezierCubicCurveTo(P(.5f, .75f), P(-.5f, .75f), P(-.95f, 0), 16); draw.PathStroke(col, ImDrawFlags.Closed, t);
                draw.AddCircleFilled(c, r * .28f, col, 16);
                if (kind == "eyeoff") draw.AddLine(P(-.8f, .8f), P(.8f, -.8f), col, t * 1.2f);
                break;
            case "kbd":
                draw.AddRect(P(-.95f, -.55f), P(.95f, .55f), col, r * .22f, ImDrawFlags.RoundCornersAll, t);
                for (int i = 0; i < 4; i++) { draw.AddCircleFilled(P(-.6f + i * .4f, -.22f), r * .085f, col, 8); draw.AddCircleFilled(P(-.4f + i * .4f, .02f), r * .085f, col, 8); }
                draw.AddLine(P(-.5f, .3f), P(.5f, .3f), col, t);
                break;
            case "live":
                draw.AddCircleFilled(c, r * .28f, col, 16);
                draw.PathArcTo(c, r * .62f, -MathF.PI * .3f, MathF.PI * .3f, 10); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.PathArcTo(c, r * .62f, MathF.PI * .7f, MathF.PI * 1.3f, 10); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.PathArcTo(c, r * 1.0f, -MathF.PI * .3f, MathF.PI * .3f, 12); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.PathArcTo(c, r * 1.0f, MathF.PI * .7f, MathF.PI * 1.3f, 12); draw.PathStroke(col, ImDrawFlags.None, t);
                break;
            case "qr":
                foreach (var (qx, qy) in new[] { (-.9f, -.9f), (.1f, -.9f), (-.9f, .1f) })
                {
                    draw.AddRect(P(qx, qy), P(qx + .8f, qy + .8f), col, 0, ImDrawFlags.None, t);
                    draw.AddRectFilled(P(qx + .26f, qy + .26f), P(qx + .54f, qy + .54f), col);
                }
                draw.AddRectFilled(P(.18f, .18f), P(.42f, .42f), col); draw.AddRectFilled(P(.62f, .18f), P(.9f, .42f), col);
                draw.AddRectFilled(P(.18f, .62f), P(.42f, .9f), col); draw.AddRectFilled(P(.62f, .62f), P(.9f, .9f), col);
                break;
            case "gear":
                for (int i = 0; i < 8; i++)
                {
                    float a = i * MathF.PI / 4;
                    draw.AddLine(c + new Vector2(MathF.Cos(a), MathF.Sin(a)) * r * .62f, c + new Vector2(MathF.Cos(a), MathF.Sin(a)) * r * .92f, col, t * 1.5f);
                }
                draw.AddCircle(c, r * .6f, col, 24, t); draw.AddCircle(c, r * .24f, col, 16, t);
                break;
            case "menu":
                for (int i = -1; i <= 1; i++) draw.AddLine(P(-.8f, i * .55f), P(.8f, i * .55f), col, t * 1.2f);
                break;
            case "copy":
                draw.AddRect(P(-.35f, -.35f), P(.8f, .8f), col, r * .15f, ImDrawFlags.RoundCornersAll, t);
                draw.PathLineTo(P(-.35f, .35f)); draw.PathLineTo(P(-.8f, .35f)); draw.PathLineTo(P(-.8f, -.8f)); draw.PathLineTo(P(.35f, -.8f)); draw.PathLineTo(P(.35f, -.35f));
                draw.PathStroke(col, ImDrawFlags.None, t);
                break;
            case "video":
                draw.AddRect(P(-.9f, -.5f), P(.3f, .5f), col, r * .2f, ImDrawFlags.RoundCornersAll, t);
                draw.AddTriangleFilled(P(.45f, -.05f), P(.95f, -.5f), P(.95f, .5f), col);
                break;
            case "gauge":
                draw.PathArcTo(c + new Vector2(0, r * .2f), r * .82f, MathF.PI, MathF.PI * 2, 24); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.AddLine(P(0, .2f), P(.5f, -.35f), col, t);
                draw.AddCircleFilled(P(0, .2f), r * .12f, col, 12);
                break;
            case "vol":
                draw.AddTriangleFilled(P(-.2f, -.5f), P(-.2f, .5f), P(-.85f, 0), col);
                draw.AddRectFilled(P(-.85f, -.3f), P(-.4f, .3f), col);
                draw.PathArcTo(P(-.05f, 0), r * .55f, -MathF.PI * .28f, MathF.PI * .28f, 10); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.PathArcTo(P(-.05f, 0), r * .95f, -MathF.PI * .28f, MathF.PI * .28f, 12); draw.PathStroke(col, ImDrawFlags.None, t);
                break;
            case "usb":
                draw.AddLine(P(-.9f, 0), P(.7f, 0), col, t);
                draw.AddCircleFilled(P(-.9f, 0), r * .18f, col, 12);
                draw.AddTriangleFilled(P(.65f, -.22f), P(.65f, .22f), P(.98f, 0), col);
                draw.PathLineTo(P(-.3f, 0)); draw.PathLineTo(P(0, -.55f)); draw.PathLineTo(P(.3f, -.55f)); draw.PathStroke(col, ImDrawFlags.None, t);
                draw.PathLineTo(P(-.1f, 0)); draw.PathLineTo(P(.15f, .5f)); draw.PathLineTo(P(.4f, .5f)); draw.PathStroke(col, ImDrawFlags.None, t);
                break;
            default:
                Icon(draw, kind, c, s, col);
                break;
        }
    }

    // ---- button / pill ----
    bool Ui(string id, float x, float y, float w, float h, string label, string glyph = "", string hint = "",
            bool active = false, bool danger = false, float fs = 19, bool iconOnly = false, uint? glyphColor = null, uint? fill = null)
    {
        ImGui.SetCursorScreenPos(new Vector2(x, y));
        bool pressed = ImGui.InvisibleButton(id, new Vector2(w, h));
        bool hot = ImGui.IsItemHovered(), focused = ImGui.IsItemFocused();
        uint back = fill ?? (danger ? (hot ? Color(.42f, .12f, .17f) : Color(.30f, .10f, .14f))
            : active ? (hot ? Color(.14f, .50f, .95f) : Color(.11f, .42f, .86f))
            : hot ? Color(.17f, .24f, .40f) : Color(.105f, .15f, .27f));
        float round = Math.Min(h * .30f, 13 * u);
        var min = new Vector2(x, y); var max = new Vector2(x + w, y + h);
        draw.AddRectFilled(min, max, back, round);
        draw.AddRect(min, max, danger ? Color(.85f, .30f, .36f, .8f) : active ? Color(.45f, .78f, 1f, .9f) : EdgeLine, round, ImDrawFlags.RoundCornersAll, 1.3f * u);
        if (focused) draw.AddRect(min - new Vector2(2 * u), max + new Vector2(2 * u), Color(.42f, .76f, 1), round + 2 * u, ImDrawFlags.RoundCornersAll, 2.4f * u);
        uint ink = glyphColor ?? (danger ? Color(1f, .62f, .64f) : White);
        float cy = y + h / 2, gs = Math.Min(h * .5f, 24 * u);
        if (iconOnly && glyph.Length > 0) { Glyph(glyph, new Vector2(x + w / 2, cy), gs, ink); return pressed; }
        float tx = x + 14 * u;
        if (glyph.Length > 0) { Glyph(glyph, new Vector2(tx + gs / 2, cy), gs, ink); tx += gs + 10 * u; }
        float hintW = hint.Length > 0 ? Tw(hint, fs - 3) + 12 * u : 0;
        TxC(Fit(label, fs, true, w - (tx - x) - hintW - 12 * u), tx, cy, fs, true, danger ? Color(1f, .78f, .8f) : White);
        if (hint.Length > 0) TxC(hint, x + w - 14 * u - Tw(hint, fs - 3), cy, fs - 3, false, Dim);
        return pressed;
    }

    // ---- notices ----
    // One look for every notice: the pop-ups, the card under Disconnect (screenshot / recording) and the error cards: a dark rounded card with an
    // accent ring and dot, a bold line and a dim detail line.
    // sidebar: the card under Disconnect when the session panel is showing (screenshot / recording); otherwise a pop-up so it is never missed.
    // group: one notice per kind of event; a new one replaces the older one instead of stacking beside it.
    // top: while the stream screen is up, show it in the top bar (between the title and the buttons) instead of as a pop-up.
    void Notify(string text, string detail = "", uint? accent = null, double seconds = 4, bool sidebar = false, string? group = null, bool top = false)
    {
        double now = ImGui.GetTime();
        uint col = accent ?? AccentBlue;
        if (sidebar && panelVisible) { sidebarNotice = (text, detail, col, now + 12); return; }
        if (group is not null)
        {
            toasts.RemoveAll(t => t.Group == group);
            if (topNotice.Group == group) topNotice = default;
        }
        if (top && player is not null && !streamSettings && !hideText) { topNotice = (text, detail, col, now + seconds, group); return; }
        toasts.RemoveAll(t => t.Text == text && t.Detail == detail);
        toasts.Add(new Toast(text, detail, col, now + seconds, group));
        if (toasts.Count > 3) toasts.RemoveAt(0);
    }
    (float W, float H) NoticeSize(string text, string detail, float maxW, float title = 23, float sub = 15, float minW = 320)
    {
        float k = title / 23f;
        string d = Fit(Hidden(detail), sub, false, maxW - 70 * u * k);
        float w = Math.Min(maxW, Math.Max(Math.Max(Tw(text, title, true), Tw(d, sub)) + 70 * u * k, minW * u));
        return (w, (d.Length > 0 ? 66 : 50) * u * k);
    }
    void DrawNotice(float x, float y, float w, float h, string text, string detail, uint accent, float fade = 1, float title = 23, float sub = 15)
    {
        float k = title / 23f;
        string t = Fit(Hidden(text), title, true, w - 70 * u * k), d = Fit(Hidden(detail), sub, false, w - 70 * u * k);
        draw.AddRectFilled(new Vector2(x, y), new Vector2(x + w, y + h), Color(.05f, .08f, .16f, .95f * fade), 14 * u * k);
        draw.AddRect(new Vector2(x, y), new Vector2(x + w, y + h), (accent & 0x00FFFFFFu) | ((uint)(230 * fade) << 24), 14 * u * k, ImDrawFlags.RoundCornersAll, 2 * u);
        draw.AddCircleFilled(new Vector2(x + 26 * u * k, y + h / 2), 7 * u * k, accent, 20);
        Tx(t, x + 46 * u * k, y + h * (d.Length > 0 ? .12f : .24f), title, true);
        if (d.Length > 0) Tx(d, x + 46 * u * k, y + h * .576f, sub, false, Dim);
    }
    // "USB unavailable. Check the folder." -> ("USB unavailable", "Check the folder.")
    static (string, string) SplitNotice(string text)
    {
        foreach (var sep in new[] { ". ", ": " })
        {
            int i = text.IndexOf(sep, StringComparison.Ordinal);
            if (i >= 6 && i < text.Length - sep.Length) return (text[..i], text[(i + sep.Length)..]);
        }
        return (text.TrimEnd('.'), "");
    }
    void DrawToasts(Vector2 size, float top)
    {
        double now = ImGui.GetTime();
        toasts.RemoveAll(t => t.Until < now);
        float y = top + 12 * u;
        foreach (var t in toasts)
        {
            var (w, h) = NoticeSize(t.Text, t.Detail, size.X * .9f);
            double left = t.Until - now; float fade = (float)Math.Clamp(left / .4, 0, 1);
            DrawNotice((size.X - w) / 2, y, w, h, t.Text, t.Detail, t.Accent, fade);
            y += h + 8 * u;
        }
    }

    // ---- folder choice for the recordings / screenshots paths ----
    void PickFolder(string title, string? current, Action<string> apply)
    {
        if (folderPick is not null) return;
        folderApply = apply;
        folderPick = WinFolderDialog.Pick(title, current);
    }
    void PollFolderPick()
    {
        if (folderPick?.IsCompleted != true) return;
        var done = folderPick; folderPick = null;
        string? chosen = null;
        try { chosen = done.GetAwaiter().GetResult(); } catch (Exception) { }
        if (!string.IsNullOrEmpty(chosen))
        {
            folderApply?.Invoke(chosen);
            SaveSettings();
            Notify("Folder saved", chosen, Good);
        }
        folderApply = null;
    }
    static string ShortPath(string? p) => string.IsNullOrEmpty(p) ? "Not set" : p.Length > 20 ? "…" + p[^19..] : p;

    // ---- controller button glyphs: drawn for the controller that is connected (same designs as the Android app) ----
    enum PadStyle { Xbox, PlayStation, Nintendo }
    enum PadButton { South, East, West, North }
    PadStyle CurrentPadStyle()
    {
        string name = (Owner.Pads?.ActiveName ?? "").ToLowerInvariant();
        ushort vendor = Owner.Pads?.ActiveVendor ?? 0;
        if (vendor == 0x054c || name.Contains("dualsense") || name.Contains("dualshock") || name.Contains("playstation")) return PadStyle.PlayStation;
        if (vendor == 0x057e || name.Contains("nintendo") || name.Contains("pro controller") || name.Contains("joy-con")) return PadStyle.Nintendo;
        return PadStyle.Xbox;
    }
    // Centre c and diameter d in window pixels. Buttons are named by position: South is the bottom one.
    void PadGlyph(PadButton button, Vector2 c, float d)
    {
        var style = CurrentPadStyle();
        float r = d / 2, t = Math.Max(1.5f, d * .085f);
        Vector2 P(float x, float y) => c + new Vector2(x, y) * r;
        if (style != PadStyle.PlayStation)
        {
            string letter = style == PadStyle.Nintendo
                ? button switch { PadButton.South => "B", PadButton.East => "A", PadButton.West => "Y", _ => "X" }
                : button switch { PadButton.South => "A", PadButton.East => "B", PadButton.West => "X", _ => "Y" };
            uint fill = style == PadStyle.Nintendo ? Color(.27f, .29f, .35f)
                : button switch { PadButton.South => Color(.26f, .63f, .28f), PadButton.East => Color(.90f, .22f, .21f), PadButton.West => Color(.12f, .53f, .90f), _ => Color(.99f, .85f, .21f) };
            uint ink = style == PadStyle.Xbox && button == PadButton.North ? Color(.20f, .17f, 0f) : White;
            draw.AddCircleFilled(c, r, fill, 28);
            float px = d * .74f;
            ImGui.PushFont(Owner.DesktopBold);
            var m = ImGui.CalcTextSize(letter) * (px / ImGui.GetFontSize());
            ImGui.PopFont();
            draw.AddText(Owner.DesktopBold, px, c - m / 2 + new Vector2(0, d * .03f), ink, letter);
            return;
        }
        draw.AddCircleFilled(c, r, Color(.165f, .188f, .259f), 28);
        switch (button)
        {
            case PadButton.South: { uint col = Color(.486f, .706f, 1f); draw.AddLine(P(-.42f, -.42f), P(.42f, .42f), col, t * 1.2f); draw.AddLine(P(.42f, -.42f), P(-.42f, .42f), col, t * 1.2f); break; }
            case PadButton.East: draw.AddCircle(c, r * .46f, Color(1f, .42f, .48f), 24, t * 1.2f); break;
            case PadButton.West: draw.AddRect(P(-.4f, -.4f), P(.4f, .4f), Color(.89f, .54f, .89f), 0, ImDrawFlags.None, t * 1.2f); break;
            default: draw.AddTriangle(P(0, -.5f), P(.48f, .36f), P(-.48f, .36f), Color(.30f, .84f, .64f), t * 1.2f); break;
        }
    }

    // ---- the screen ----
    void StreamScreen(Vector2 size)
    {
        float W = size.X, H = size.Y;
        u = Math.Clamp(Math.Min(W / 1280f, H / 720f), .72f, 1.6f);
        float designW = W / u;
        bool ui = !hideText;
        bool compact = designW < 1060;
        bool panelOn = ui && showPanel && designW >= 900;
        panelVisible = panelOn;
        float topH = ui ? 58 * u : 0, botH = ui ? 62 * u : 0, m = ui ? 14 * u : 0, panelW = panelOn ? 292 * u : 0;
        float vx = m, vy = topH + (ui ? 10 * u : 0), vw = W - m * 2 - (panelOn ? panelW + m : 0), vh = H - vy - botH - (ui ? 10 * u : 0);
        if (!ui) { vx = 0; vy = 0; vw = W; vh = H; }
        if (player is null) return;
        if (ui) player.SetViewport((int)vx, (int)vy, (int)vw, (int)vh); else player.ClearViewport();
        var vmin = new Vector2(vx, vy); var vmax = new Vector2(vx + vw, vy + vh);

        // backgrounds around the viewport only (the picture itself is drawn beneath the interface)
        if (ui)
        {
            draw.AddRectFilled(Vector2.Zero, new Vector2(W, vy), BgDark);
            draw.AddRectFilled(new Vector2(0, vmax.Y), new Vector2(W, H), BgDark);
            draw.AddRectFilled(new Vector2(0, vy), new Vector2(vx, vmax.Y), BgDark);
            draw.AddRectFilled(new Vector2(vmax.X, vy), new Vector2(W, vmax.Y), BgDark);
        }
        if (!player.HasDecodedFrame)
        {
            draw.AddRectFilledMultiColor(vmin, vmax, Color(.035f, .025f, .085f), Color(.025f, .075f, .15f), Color(.015f, .025f, .065f), Color(.10f, .018f, .09f));
            if (ui) { string wait = player.Manager.HasVideo ? "Waiting for video..." : "Audio connected"; Tx(wait, vmin.X + (vw - Tw(wait, 32, true)) / 2, vmin.Y + vh / 2 - 18 * u, 32, true, Soft); }
        }
        if (ui)
        {
            draw.AddRect(vmin - new Vector2(1 * u), vmax + new Vector2(1 * u), Color(.30f, .40f, .62f, .35f), 2 * u, ImDrawFlags.RoundCornersAll, 1.2f * u);
            TopBar(W, topH, compact);
            if (panelOn) SidePanel(W - panelW - m, vy - 1 * u, panelW, vmax.Y - vy + 1 * u);
            // The Disconnect button ends the session in the middle of this frame; the bottom bar needs the player.
            if (player is null) return;
            BottomBar(W, H, botH, designW);
        }
        else if (appControlOn) QuickSidebar(size);
        else if (!Program.SdlCtx.IsFullscreen) FloatingShow(size);   // full screen has no interface at all (H or the controller's Guide button bring it up)

        // received frame rate over the picture (also with the interface hidden / fullscreen)
        if (Program.Options.ShowFpsOverlay)
        {
            string fps = $"{FpsNow():F0} FPS";
            float pw = Tw(fps, 20, true) + 50 * u;
            draw.AddRectFilled(vmin + new Vector2(10 * u), vmin + new Vector2(10 * u + pw, 10 * u + 34 * u), Color(.02f, .03f, .07f, .72f), 10 * u);
            Glyph("gauge", vmin + new Vector2(28 * u, 27 * u), 18 * u, Good);
            TxC(fps, vmin.X + 44 * u, vmin.Y + 27 * u, 20, true);
        }
        if (ui && error.Length > 0)
        {
            // Centred over the bottom of the picture, in the same card style as the pop-ups (quieter than the old red box).
            var (et, ed) = SplitNotice(error);
            var (ew, eh) = NoticeSize(et, ed, vw - 40 * u, 19, 14, 280);
            DrawNotice(vmin.X + (vw - ew) / 2, vmax.Y - eh - 16 * u, ew, eh, et, ed, Warn, 1, 19, 14);
        }
        if (Program.Options.StreamerMode || !ui) qrTag = null;
        if (qrTag is not null) QrCard(vmin, vmax);
        DrawToasts(size, ui ? topH : 0);
    }

    // ---- QR code for the browser address: a phone scans it instead of typing the IP ----
    // The code holds the session link (the same one Open/Copy use, with the join token), so scanning it joins this session without the
    // password: it is a credential. It is never drawn while streamer mode is on or the interface is hidden.
    string? qrTag;                       // "Local" / "Public" while the card is open
    string qrText = "";
    bool[][]? qrModules;
    static bool[][] QrMatrix(string text)
    {
        using var generator = new QRCoder.QRCodeGenerator();
        using var data = generator.CreateQrCode(text, QRCoder.QRCodeGenerator.ECCLevel.M);
        var matrix = data.ModuleMatrix;
        var rows = new bool[matrix.Count][];
        for (int y = 0; y < rows.Length; y++)
        {
            rows[y] = new bool[matrix[y].Count];
            for (int x = 0; x < rows[y].Length; x++) rows[y][x] = matrix[y][x];
        }
        return rows;
    }
    void QrCard(Vector2 vmin, Vector2 vmax)
    {
        bool pub = qrTag == "Public";
        var link = pub ? browser?.PublicLink : browser?.Link;
        if (link is null) { qrTag = null; return; }
        string text = (browser?.SessionLink(link) ?? link).ToString();
        if (qrModules is null || qrText != text) { qrModules = QrMatrix(text); qrText = text; }
        float vw = vmax.X - vmin.X, vh = vmax.Y - vmin.Y;
        float pad = 16 * u, side = Math.Min(Math.Min(300 * u, vw - 2 * pad - 24 * u), vh - 190 * u);
        if (side < 120 * u) side = Math.Max(side, 120 * u);
        float cw = side + 2 * pad, ch = side + 2 * pad + 112 * u;
        float cx = vmin.X + (vw - cw) / 2, cy = vmin.Y + (vh - ch) / 2;
        Card(cx, cy, cw, ch);
        Tx(Fit("Scan with your phone", 20, true, cw - 2 * pad), cx + pad, cy + 12 * u, 20, true);
        Tx(Fit(pub ? "Joins this session from outside your network" : "Joins this session on the same Wi-Fi", 13, false, cw - 2 * pad), cx + pad, cy + 40 * u, 13, false, Dim);
        float qy = cy + 62 * u;
        var rows = qrModules;
        int n = rows.Length;
        float module = MathF.Floor(side / n);
        if (module < 1) module = 1;
        float qs = module * n, qx = cx + (cw - qs) / 2;
        draw.AddRectFilled(new Vector2(qx - 4 * u, qy - 4 * u), new Vector2(qx + qs + 4 * u, qy + qs + 4 * u), White, 6 * u);
        uint ink = Color(0, 0, 0);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (rows[y][x]) draw.AddRectFilled(new Vector2(qx + x * module, qy + y * module), new Vector2(qx + (x + 1) * module, qy + (y + 1) * module), ink);
        TxC(Fit(link.Authority, 16, true, cw - 2 * pad), cx + pad, qy + qs + 22 * u, 16, true);
        if (Ui("qr_close", cx + pad, cy + ch - pad - 38 * u, cw - 2 * pad, 38 * u, "Close", "", fs: 17)) qrTag = null;
    }

    float FpsNow() => usbVideoMeter is not null ? (float)usbVideoMeter.ReceivedFps() : player?.DecodedFps ?? 0;

    void TopBar(float W, float h, bool compact)
    {
        draw.AddLine(new Vector2(0, h), new Vector2(W, h), Color(.30f, .40f, .62f, .40f), 1.5f * u);
        float lh = 40 * u, lw = lh * logo!.Width / logo.Height;
        draw.AddImage(logo.Texture, new Vector2(16 * u, (h - lh) / 2), new Vector2(16 * u + lw, (h + lh) / 2));
        float x = 16 * u + lw + 12 * u;
        TxC("NX Remote Play", x, h / 2, 25, true);
        x += Tw("NX Remote Play", 25, true) + 22 * u;
        draw.AddLine(new Vector2(x, h * .28f), new Vector2(x, h * .72f), EdgeLine, 1.5f * u);
        x += 18 * u;
        if (recorder is not null)
        {
            double secs = ImGui.GetTime() - recordingStart;
            string rec = $"REC {(int)(secs / 60):00}:{(int)(secs % 60):00}";
            float rw = Tw(rec, 18, true) + 44 * u;
            draw.AddRectFilled(new Vector2(x, h / 2 - 16 * u), new Vector2(x + rw, h / 2 + 16 * u), Color(.35f, .08f, .12f, .95f), 16 * u);
            if (((int)(ImGui.GetTime() * 2) & 1) == 0) draw.AddCircleFilled(new Vector2(x + 18 * u, h / 2), 6 * u, Bad, 14);
            TxC(rec, x + 32 * u, h / 2, 18, true, Color(1f, .85f, .85f));
            x += rw + 16 * u;
        }

        // right-hand buttons: Streamer, Hide UI, Settings, Menu (labels drop away on narrow windows)
        float bh = 40 * u, by = (h - bh) / 2, gap = 10 * u, right = W - 16 * u;
        (string id, string label, string glyph, bool on)[] buttons =
        {
            ("menu", "Menu", "menu", showPanel),
            ("settings", "Settings", "gear", false),
            ("hide", "Hide UI", "eyeoff", false),
            ("streamer", "Streamer", "live", Program.Options.StreamerMode),
        };
        float ButtonWidth(string label) => compact ? 44 * u : 20 * u + 24 * u + 8 * u + Tw(label, 19, true) + 8 * u;
        float buttonsLeft = right;
        foreach (var (_, label, _, _) in buttons) buttonsLeft -= ButtonWidth(label) + gap;
        // USB notice: whatever space is left between the title (or the recording pill) and the buttons
        if (ImGui.GetTime() < topNotice.Until && topNotice.Text is not null)
        {
            float room = buttonsLeft - 8 * u - x;
            if (room > 200 * u)
            {
                var (nw, nh) = NoticeSize(topNotice.Text, topNotice.Detail, room, 17, 12.5f, 200);
                float fade = (float)Math.Clamp((topNotice.Until - ImGui.GetTime()) / .4, 0, 1);
                DrawNotice(x, (h - nh) / 2, nw, nh, topNotice.Text, topNotice.Detail, topNotice.Accent, fade, 17, 12.5f);
            }
        }
        foreach (var (id, label, glyph, on) in buttons)
        {
            float bw = ButtonWidth(label);
            right -= bw;
            bool hit = Ui("top_" + id, right, by, bw, bh, label, glyph, active: on, fs: 19, iconOnly: compact);
            right -= gap;
            if (!hit) continue;
            switch (id)
            {
                case "menu": showPanel = !showPanel; break;
                case "settings": streamSettings = true; page = Page.Settings; break;
                case "hide": hideText = true; break;
                case "streamer": ToggleStreamer(); break;
            }
        }
    }

    void ToggleStreamer()
    {
        Program.Options.StreamerMode = !Program.Options.StreamerMode; SaveSettings();
        Notify(Program.Options.StreamerMode ? "Streamer mode on" : "Streamer mode off",
            Program.Options.StreamerMode ? "Session ID, addresses and links are hidden" : "", Program.Options.StreamerMode ? Good : AccentBlue);
    }
    // Left / right on the focused volume control (keyboard or controller) change it in 5% steps; a ring shows it has the focus.
    void VolumeByKeys(NxRemotePlay.Client.Targets.Player.BaseAudioPlayerTarget audio)
    {
        if (!ImGui.IsItemFocused()) return;
        draw.AddRect(ImGui.GetItemRectMin(), ImGui.GetItemRectMax(), Color(.42f, .76f, 1), 10 * u, ImDrawFlags.RoundCornersAll, 2 * u);
        if (ImGui.IsKeyPressed(ImGuiKey.LeftArrow)) audio.Volume = Math.Max(0f, audio.Volume - .05f);
        else if (ImGui.IsKeyPressed(ImGuiKey.RightArrow)) audio.Volume = Math.Min(1f, audio.Volume + .05f);
    }

    // The controller has been handed to the interface while the interface itself is hidden (full screen, or Hide UI): everything the
    // interface offers, as one side panel the controller can drive. It goes away when the controller goes back to the Switch.
    void QuickSidebar(Vector2 size)
    {
        float W = size.X, H = size.Y;
        u = Math.Clamp(Math.Min(W / 1280f, H / 720f), .72f, 1.6f);
        float pw = Math.Min(340 * u, W * .6f), x = W - pw - 12 * u, y = 12 * u, h = H - 24 * u, pad = 14 * u, bw = pw - pad * 2;
        Card(x, y, pw, h);
        Tx(Fit("Controller: moving the app", 19, true, bw), x + pad, y + 10 * u, 19, true);
        Tx(Fit("Press Guide to give it back to the Switch", 13, false, bw), x + pad, y + 36 * u, 13, false, Dim);

        var local = browser?.Link;
        var pub = browser?.PublicLink;
        var rows = new List<(string Id, string Label, string Glyph, string Hint, bool Active, bool Danger, Action Do)>
        {
            ("show", "Show interface", "eye", "H", false, false, () => hideText = false),
            ("streamer", "Streamer", "live", Program.Options.StreamerMode ? "On" : "Off", Program.Options.StreamerMode, false, ToggleStreamer),
            ("settings", "Settings", "gear", "", false, false, () => { streamSettings = true; page = Page.Settings; }),
            ("full", Program.SdlCtx.IsFullscreen ? "Exit Fullscreen" : "Fullscreen", "full", Program.SdlCtx.IsFullscreen ? "Esc" : "F", false, false, () => Program.SdlCtx.SetFullScreen(!Program.SdlCtx.IsFullscreen)),
            ("shot", "Screenshot", "cam", "S", false, false, Screenshot),
            ("rec", recorder is null ? "Record" : "Stop Recording", recorder is null ? "rec" : "stop", "R", false, recorder is not null, ToggleRecording),
            ("rot", "Rotate View", "rot", "T", false, false, Rotate),
            ("home", "Home", "home", "", false, false, () => controller?.TapHome(ImGui.GetTime())),
            ("fps", "FPS overlay", "gauge", Program.Options.ShowFpsOverlay ? "On" : "Off", Program.Options.ShowFpsOverlay, false, () => { Program.Options.ShowFpsOverlay = !Program.Options.ShowFpsOverlay; SaveSettings(); }),
            ("volume", "", "vol", "", false, false, () => { }),
        };
        if (local is not null)
        {
            rows.Add(("openlocal", "Open Local link", "ext", "", false, false, () => SystemUtil.OpenURL((browser?.SessionLink(local) ?? local).ToString())));
            rows.Add(("copylocal", "Copy Local link", "copy", "", false, false, () => { ImGui.SetClipboardText((browser?.SessionLink(local) ?? local).ToString()); Notify("Local link copied", "Joins without a password for this USB session", Good, 3); }));
        }
        if (pub is not null)
        {
            rows.Add(("openpublic", "Open Public link", "ext", "", false, false, () => SystemUtil.OpenURL((browser?.SessionLink(pub) ?? pub).ToString())));
            rows.Add(("copypublic", "Copy Public link", "copy", "", false, false, () => { ImGui.SetClipboardText((browser?.SessionLink(pub) ?? pub).ToString()); Notify("Public link copied", "Joins without a password for this USB session", Good, 3); }));
        }
        rows.Add(("disc", "Disconnect", "power", "", false, true, UserDisconnect));

        float top = y + 62 * u, footer = 34 * u, gap = 4 * u;
        float rowH = Math.Clamp((y + h - footer - top) / rows.Count - gap, 22 * u, 40 * u);
        float ry = top;
        foreach (var row in rows)
        {
            if (row.Id == "volume" && player!.Manager.AudioTarget is { } audio)
            {
                float cyv = ry + rowH / 2, sx = x + pad + 38 * u, sw = bw - 38 * u - 58 * u;
                Glyph("vol", new Vector2(x + pad + 12 * u, cyv), 22 * u, Soft);
                ImGui.SetCursorScreenPos(new Vector2(sx - 10 * u, ry));
                ImGui.InvisibleButton("##qvolume", new Vector2(sw + 20 * u, rowH));
                if (ImGui.IsItemActive()) audio.Volume = Math.Clamp((ImGui.GetIO().MousePos.X - sx) / sw, 0f, 1f);
                VolumeByKeys(audio);
                float level = Math.Clamp(audio.Volume, 0f, 1f);
                draw.AddRectFilled(new Vector2(sx, cyv - 3 * u), new Vector2(sx + sw, cyv + 3 * u), Color(.13f, .19f, .33f), 3 * u);
                draw.AddRectFilled(new Vector2(sx, cyv - 3 * u), new Vector2(sx + sw * level, cyv + 3 * u), AccentBlue, 3 * u);
                draw.AddCircleFilled(new Vector2(sx + sw * level, cyv), 8 * u, White, 20);
                string vtxt = (int)(level * 100) + "%";
                TxC(vtxt, x + pw - pad - Tw(vtxt, 16, true), cyv, 16, true, Soft);
                ry += rowH + gap;
                continue;
            }
            if (row.Id == "volume") continue;
            if (Ui("qs_" + row.Id, x + pad, ry, bw, rowH, row.Label, row.Glyph, row.Hint, active: row.Active, danger: row.Danger, fs: 17))
            {
                row.Do();
                if (player is null) return;   // Disconnect ended the session in the middle of the frame
            }
            ry += rowH + gap;
        }
        // state and live numbers
        string state = (player!.HasDecodedFrame ? "Connected" : "Connecting") + $"  \u2022  {FpsNow():F0} FPS";
        if (usbVideoMeter is not null) { var r = usbVideoMeter.Snapshot(); if (r.Seconds >= 1) state += $"  \u2022  {r.RecentMbps:F1} Mb/s"; }
        Tx(Fit(state, 14, false, bw), x + pad, y + h - 26 * u, 14, false, Dim);
    }

    void Card(float x, float y, float w, float h)
    {
        draw.AddRectFilled(new Vector2(x, y), new Vector2(x + w, y + h), PanelFill, 16 * u);
        draw.AddRect(new Vector2(x, y), new Vector2(x + w, y + h), EdgeLine, 16 * u, ImDrawFlags.RoundCornersAll, 1.3f * u);
    }

    void SidePanel(float x, float y, float w, float h)
    {
        float pad = 14 * u, gap = 10 * u;
        // connection card
        string title = usbSession ? (bridge?.Online == true ? "Bridge Mode • " + Sens(usbSource?.SessionId ?? "") : "USB Viewer") : Hidden(status);
        string line1 = browser is not null && browser.HasBrowser ? "Browser connected • Controls allowed automatically"
            : bridge?.Online == true ? (bridge.ViewerCount >= 2 ? $"{bridge.ViewerCount} viewers connected" : bridge.HasNative ? "Native client connected" : "Waiting for a viewer • use your Switch ID/password")
            : bridgeStarting is not null || browserStarting is not null ? "Starting sharing..." : usbSession ? Hidden(status) : "Remote play";
        string pad1 = controller is null ? "No controller target" : !Program.Options.DesktopGamepad ? "Gamepad forwarding off" : appControlOn ? "Controller: moving the app" : (usbSession ? "USB • " : "") + controller.Name;
        float cardH = 118 * u;
        Card(x, y, w, cardH);
        Glyph("plug", new Vector2(x + pad + 17 * u, y + 34 * u), 34 * u, Soft);
        float tx = x + pad + 44 * u + 8 * u, tw = w - (tx - x) - pad;
        Tx(Fit(title, 21, true, tw), tx, y + 12 * u, 21, true);
        Tx(Fit(line1, 15, false, tw), tx, y + 40 * u, 15, false, Dim);
        draw.AddLine(new Vector2(x + pad, y + 70 * u), new Vector2(x + w - pad, y + 70 * u), EdgeLine, 1.2f * u);
        Glyph("pad", new Vector2(x + pad + 14 * u, y + 94 * u), 24 * u, Good);
        TxC(Fit(pad1, 17, false, w - pad * 2 - 40 * u), x + pad + 36 * u, y + 94 * u, 17, false, Soft);

        // session controls
        float cy = y + cardH + gap;
        float noticeH = 50 * u;
        float footerH = 44 * u /*disconnect*/ + 8 * u + noticeH /*screenshot / recording card*/ + gap * 2, sliderH = 44 * u;
        float avail = y + h - cy - footerH - sliderH - 34 * u;
        float rowH = Math.Clamp(avail / 7 - 6 * u, 28 * u, 42 * u), rowGap = 6 * u;
        float controlsH = 34 * u + 7 * (rowH + rowGap) + sliderH + footerH - gap;
        Card(x, cy, w, Math.Min(controlsH, y + h - cy));
        Tx("Session Controls", x + pad, cy + 8 * u, 20, true);
        float ry = cy + 34 * u, bw = w - pad * 2;
        if (Ui("sc_full", x + pad, ry, bw, rowH, Program.SdlCtx.IsFullscreen ? "Exit Fullscreen" : "Fullscreen", "full", Program.SdlCtx.IsFullscreen ? "Esc" : "F", fs: 18)) Program.SdlCtx.SetFullScreen(!Program.SdlCtx.IsFullscreen); ry += rowH + rowGap;
        if (Ui("sc_shot", x + pad, ry, bw, rowH, "Screenshot", "cam", "S", fs: 18)) Screenshot(); ry += rowH + rowGap;
        if (Ui("sc_rec", x + pad, ry, bw, rowH, recorder is null ? "Record" : "Stop Recording", recorder is null ? "rec" : "stop", "R", fs: 18, glyphColor: Bad, active: false, danger: recorder is not null)) ToggleRecording(); ry += rowH + rowGap;
        if (Ui("sc_rot", x + pad, ry, bw, rowH, "Rotate View", "rot", "T", fs: 18)) Rotate(); ry += rowH + rowGap;
        if (Ui("sc_home", x + pad, ry, bw, rowH, "Home", "home", "", fs: 18)) controller?.TapHome(ImGui.GetTime()); ry += rowH + rowGap;
        if (Ui("sc_pad", x + pad, ry, bw, rowH, Program.Options.DesktopGamepad ? "Controller: On" : "Controller: Off", "pad", "", fs: 18, active: Program.Options.DesktopGamepad))
        { ToggleControllerForwarding(); }
        ry += rowH + rowGap;
        if (Ui("sc_fps", x + pad, ry, bw, rowH, "FPS overlay", "gauge", Program.Options.ShowFpsOverlay ? "On" : "Off", fs: 18, active: Program.Options.ShowFpsOverlay))
        { Program.Options.ShowFpsOverlay = !Program.Options.ShowFpsOverlay; SaveSettings(); }
        ry += rowH + rowGap;
        // volume
        if (player!.Manager.AudioTarget is { } audio)
        {
            float cyv = ry + sliderH / 2 - 3 * u, sx = x + pad + 38 * u, sw = bw - 38 * u - 58 * u;
            Glyph("vol", new Vector2(x + pad + 12 * u, cyv), 24 * u, Soft);
            ImGui.SetCursorScreenPos(new Vector2(sx - 10 * u, cyv - 16 * u));
            ImGui.InvisibleButton("##volume", new Vector2(sw + 20 * u, 32 * u));
            if (ImGui.IsItemActive()) audio.Volume = Math.Clamp((ImGui.GetIO().MousePos.X - sx) / sw, 0f, 1f);
            VolumeByKeys(audio);
            float level = Math.Clamp(audio.Volume, 0f, 1f);
            draw.AddRectFilled(new Vector2(sx, cyv - 3 * u), new Vector2(sx + sw, cyv + 3 * u), Color(.13f, .19f, .33f), 3 * u);
            draw.AddRectFilled(new Vector2(sx, cyv - 3 * u), new Vector2(sx + sw * level, cyv + 3 * u), AccentBlue, 3 * u);
            draw.AddCircleFilled(new Vector2(sx + sw * level, cyv), 9 * u, White, 20);
            draw.AddCircle(new Vector2(sx + sw * level, cyv), 9 * u, AccentBlue, 20, 2.4f * u);
            string vtxt = (int)(level * 100) + "%";
            TxC(vtxt, x + w - pad - Tw(vtxt, 17, true), cyv, 17, true, Soft);
        }
        ry += sliderH;
        if (Ui("sc_disc", x + pad, ry, bw, 44 * u, "Disconnect", "power", "", danger: true, fs: 19)) { UserDisconnect(); return; }
        // Screenshot / recording notice: under Disconnect, in the pop-up style (it used to be one line of small text).
        string nTitle = "", nDetail = ""; uint nCol = AccentBlue;
        if (recorder is not null) { nTitle = "Recording"; nDetail = Path.GetFileName(recordingPath); nCol = Bad; }
        else if (ImGui.GetTime() < sidebarNotice.Until)
        {
            (nTitle, nDetail, nCol) = (sidebarNotice.Text, sidebarNotice.Detail, sidebarNotice.Accent);
            if (nDetail.Contains('\\') || nDetail.Contains('/')) nDetail = Path.GetFileName(nDetail);
        }
        if (nTitle.Length > 0) DrawNotice(x + pad, ry + 44 * u + 8 * u, bw, noticeH, nTitle, nDetail, nCol, 1, 18, 13);
    }

    void BottomBar(float W, float H, float h, float designW)
    {
        float y = H - h;
        draw.AddLine(new Vector2(0, y), new Vector2(W, y), EdgeLine, 1.5f * u);
        float cy = y + h / 2, bh = 38 * u, x = 16 * u;
        var local = browser?.Link;
        var pub = browser?.PublicLink;
        void Address(string tag, string glyph, Uri? link)
        {
            if (link is null) return;
            string text = Sens(link.Authority);
            if (x > 24 * u) draw.AddLine(new Vector2(x - 9 * u, cy - 16 * u), new Vector2(x - 9 * u, cy + 16 * u), EdgeLine, 1.2f * u);
            Glyph(glyph, new Vector2(x + 11 * u, cy), 22 * u, Soft);
            Tx(tag, x + 30 * u, cy - 17 * u, 13, false, Dim);
            Tx(text, x + 30 * u, cy - 2 * u, 18, true);
            float tw = Math.Max(Tw(text, 18, true), Tw(tag, 13));
            x += 30 * u + tw + 12 * u;
            if (Ui("open_" + tag, x, cy - bh / 2, 38 * u, bh, "Open", "ext", iconOnly: true)) SystemUtil.OpenURL((browser?.SessionLink(link) ?? link).ToString());
            x += 38 * u + 6 * u;
            if (Ui("copy_" + tag, x, cy - bh / 2, 38 * u, bh, "Copy", "copy", iconOnly: true)) { ImGui.SetClipboardText((browser?.SessionLink(link) ?? link).ToString()); Notify(tag + " link copied", "Joins without a password for this USB session", Good, 3); }
            x += 38 * u + 6 * u;
            // The QR code shows the address, so streamer mode takes the button away (and closes an open card).
            if (!Program.Options.StreamerMode)
            {
                if (Ui("qr_" + tag, x, cy - bh / 2, 38 * u, bh, "QR code", "qr", iconOnly: true, active: qrTag == tag)) qrTag = qrTag == tag ? null : tag;
                x += 38 * u + 6 * u;
            }
            x += 12 * u;
        }
        if (local is not null || pub is not null)
        {
            Address("Local", "wifi", local);
            if (designW >= 760) Address("Public", "globe", pub);
        }
        else { string off = browserStarting is not null ? "Starting browser sharing..." : "Browser sharing off"; TxC(off, x, cy, 17, false, Dim); x += Tw(off, 17) + 24 * u; }
        float addressEnd = x;

        // live numbers, right aligned
        float right = W - 18 * u;
        double fpsNow = FpsNow();
        string fpsText = $"{fpsNow:F0} FPS";
        string rate = "";
        if (usbVideoMeter is not null)
        {
            var r = usbVideoMeter.Snapshot();
            rate = r.Seconds < 1 ? "measuring…" : $"{r.RecentMbps:F1} Mb/s";
            if (designW >= 1150 && r.Seconds >= 1) rate += $"  (avg {r.AverageMbps:F1})";
        }
        void Stat(string glyph, string text)
        {
            if (string.IsNullOrEmpty(text)) return;
            float tw = Tw(text, 18, true);
            right -= tw; TxC(text, right, cy, 18, true);
            right -= 32 * u; Glyph(glyph, new Vector2(right + 12 * u, cy), 22 * u, Dim);
            right -= 12 * u; draw.AddLine(new Vector2(right, cy - 14 * u), new Vector2(right, cy + 14 * u), EdgeLine, 1.2f * u); right -= 18 * u;
        }
        Stat("gauge", fpsText);
        Stat("chip", rate);
        // connection state: the left-most live number, right next to "H.264" (same size, green/amber dot)
        bool reconnecting = status.Contains("reconnecting", StringComparison.OrdinalIgnoreCase);
        string state = reconnecting ? "Reconnecting" : player!.HasDecodedFrame ? "Connected" : "Connecting";
        uint stateColor = reconnecting || !player.HasDecodedFrame ? Warn : Good;
        void StateStat()
        {
            float tw = Tw(state, 18, true);
            if (right - (tw + 62 * u) < addressEnd) return;
            right -= tw; TxC(state, right, cy, 18, true, stateColor);
            right -= 20 * u; draw.AddCircleFilled(new Vector2(right + 7 * u, cy), 6.5f * u, stateColor, 18);
            right -= 12 * u; draw.AddLine(new Vector2(right, cy - 14 * u), new Vector2(right, cy + 14 * u), EdgeLine, 1.2f * u); right -= 18 * u;
        }
        if (designW >= 960) Stat("video", player!.Manager.HasVideo ? "H.264" : "Audio only");
        StateStat();
    }

    // The "show UI" button while the interface is hidden: semi-transparent, draggable, a click brings the UI back.
    void FloatingShow(Vector2 size)
    {
        float d = 54 * u;
        if (floatPos.X < 0) floatPos = new Vector2(size.X - d - 24 * u, 22 * u);
        floatPos = Vector2.Clamp(floatPos, Vector2.Zero, Vector2.Max(Vector2.Zero, size - new Vector2(d)));
        ImGui.SetCursorScreenPos(floatPos);
        ImGui.InvisibleButton("##showui", new Vector2(d));
        bool hot = ImGui.IsItemHovered(), active = ImGui.IsItemActive();
        if (ImGui.IsItemActivated()) { floatStart = ImGui.GetIO().MousePos; floatMoved = false; }
        if (active)
        {
            if ((ImGui.GetIO().MousePos - floatStart).Length() > 6 * u) floatMoved = true;
            if (floatMoved) floatPos += ImGui.GetIO().MouseDelta;
        }
        bool clicked = ImGui.IsItemDeactivated() && !floatMoved && hot;
        float a = hot || active ? .92f : .42f;
        var c = floatPos + new Vector2(d / 2);
        draw.AddCircleFilled(c, d / 2, Color(.05f, .08f, .16f, a * .9f), 36);
        draw.AddCircle(c, d / 2, Color(.12f, .62f, .98f, a), 36, 2.4f * u);
        float ls = d * .56f, lw = ls * logo!.Width / logo.Height;
        draw.AddImage(logo.Texture, c - new Vector2(lw, ls) / 2, c + new Vector2(lw, ls) / 2, Vector2.Zero, Vector2.One, ImGui.ColorConvertFloat4ToU32(new Vector4(1, 1, 1, Math.Min(1, a + .1f))));
        if (hot && !active) { string tip = "Show UI (H) • drag to move"; float tw = Tw(tip, 15); Tx(tip, Math.Clamp(floatPos.X + d / 2 - tw / 2, 6, size.X - tw - 6), floatPos.Y + d + 6 * u, 15, false, Soft); }
        if (clicked) hideText = false;
    }
}
