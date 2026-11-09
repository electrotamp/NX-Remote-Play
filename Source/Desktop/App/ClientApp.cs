namespace NxRemotePlay.Client.App;

using ImGuiNET;
using SDL2;
using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.GUI;
using NxRemotePlay.Client.GUI.Components;
using NxRemotePlay.Client.Platform;
using System;
using System.Collections.Generic;
using System.Diagnostics;

public class ClientApp : IApplicationInstance
{

    public float UiScale { get; private set; }

    SDLContext sdlCtx => Program.SdlCtx;

    public ClientApp()
    {
        ShowDebugInfo = Program.Options.Debug.GuiDebug;
    }

    // Fonts are loaded at double the resolution to reduce blurriness when scaling on high DPI
    const int FontMultiplier = 2;
    const int FontTextSize = 30 * FontMultiplier;
    public ImFontPtr FontText { get; private set; }
    public ImFontPtr DesktopFont { get; private set; }
    public ImFontPtr DesktopBold { get; private set; }
    // Controller navigation of the interface (follows whichever controller is in use)
    internal PadNavigator? Pads { get; private set; }

    public bool IsPortrait { get; private set; }

    // Special input state
    public bool ShiftDown { get; private set; }

    public event Action OnExit;

    // View state management
    Stack<View> Views = new();
    View? CurrentView = null;
    ImGuiStyle DefaultStyle;
    bool PendingViewChanges;
    FramerateCap Cap = new();

    // Async actions that must take place on the main thread outside of the rendering loop
    List<Action> PendingActions = new();

    // Debug window state
    public bool ShowDebugInfo = false;
    bool ShowImguiDemo;

    unsafe void BackupDefaultStyle()
    {
        DefaultStyle = *ImGui.GetStyle().NativePtr;
    }

    unsafe void RestoreDefaultStyle()
    {
        *ImGui.GetStyle().NativePtr = DefaultStyle;
    }

    // Private view API, has immediate effect
    void HandlePopView()
    {
        PendingViewChanges = false;

        CurrentView?.LeaveForeground();
        CurrentView?.Destroy();
        CurrentView = null;
        if (Views.Count > 0)
        {
            CurrentView = Views.Pop();
            Cap.SetMode(CurrentView.RenderMode);
            CurrentView?.ResolutionChanged();
            CurrentView?.EnterForeground();
        }
    }

    void HandlePushView(View v)
    {
        PendingViewChanges = false;

        if (CurrentView != null)
            Views.Push(CurrentView);

        CurrentView?.LeaveForeground();

        CurrentView = v;
        Cap.SetMode(CurrentView.RenderMode);
        CurrentView?.Created();
        CurrentView?.ResolutionChanged();
        CurrentView?.EnterForeground();
    }

    // Post an action to be handled in the main thread
    public void PostAction(Action act)
    {
        lock (PendingActions)
        {
            PendingActions.Add(act);
        }
    }

    private void ExecutePendingActions()
    {
        Action[] actions;
        lock (PendingActions)
        {
            actions = PendingActions.ToArray();
            PendingActions.Clear();
        }
        // Actions may await a worker that posts back to the UI. Never hold the
        // queue lock while invoking them or cancellation/teardown can deadlock.
        foreach (var action in actions) action();
    }

    public void PopView()
    {
        if (PendingViewChanges)
            throw new Exception("A view action is already scheduled");

        PendingViewChanges = true;
        PostAction(HandlePopView);
    }

    void DebugWindow()
    {
        ImGui.Begin("Info");
        ImGui.Text($"FPS: {ImGui.GetIO().Framerate} Cap {Cap.CapMode}");
        ImGui.Text(sdlCtx.GetDebugInfo());
        ImGui.Text($"scale: {UiScale} mode: {(IsPortrait ? "Portrait" : "Landscape")} stack: {Views.Count}");
        ImGui.Checkbox("Show imgui demo", ref ShowImguiDemo);
        CurrentView?.DrawDebug();
        ImGui.End();

        if (ShowImguiDemo)
            ImGui.ShowDemoWindow();
    }

    private void UpdateSize()
    {
        var w = sdlCtx.WindowSize.X;
        var h = sdlCtx.WindowSize.Y;

        IsPortrait = w / (float)h < 1.3;

        // Apply scale so the biggest dimension matches 1280
        var oldscale = UiScale;
        var biggest = Math.Max(w, h);
        UiScale = biggest / 1280f;
        if (UiScale != oldscale)
        {
            RestoreDefaultStyle();
            ImGui.GetStyle().ScaleAllSizes(UiScale);
            ImGui.GetIO().FontGlobalScale = UiScale / 2;
        }

        CurrentView?.ResolutionChanged();
    }

    unsafe void UnsafeImguiInitialization()
    {
        ImGui.GetIO().NativePtr->IniFilename = null;
    }

    public void Initialize()
    {
        Program.DebugLog("Initializing app");

        ImGui.CreateContext();

        UnsafeImguiInitialization();

        ImGui.GetIO().ConfigFlags |= ImGuiConfigFlags.NavEnableKeyboard;
        ImGui.GetIO().NavVisible = true;
        ApplyImguiSettings();

        InitializeFonts();

        BackupDefaultStyle();
    }

    ImVector GetFontRanges()
    {
        var baseRange = ImGui.GetIO().Fonts.GetGlyphRangesDefault();

        var ptr = ImFontGlyphRangesBuilderPtr.Create();

        if (baseRange != 0)
            ptr.AddRanges(baseRange);

        foreach (var s in Program.Strings.GetAllStringForFontBuilding())
            ptr.AddText(s);

        ptr.BuildRanges(out var imvec);
        ptr.Destroy();
        return imvec;
    }

    internal void InitializeFonts()
    {
        ImGui.GetIO().Fonts.Flags |= ImFontAtlasFlags.NoPowerOfTwoHeight;
        // The default atlas range stops at Latin-1, so the bullet, ellipsis and typographic dashes/quotes the interface uses came out as "?".
        // They are added explicitly; the range data must stay alive until the atlas is built below.
        ImVector desktopRanges = default;
        var assets = System.IO.Path.Combine(AppContext.BaseDirectory, "runtimes", "Desktop");
        var builder = ImFontGlyphRangesBuilderPtr.Create();
        builder.AddRanges(ImGui.GetIO().Fonts.GetGlyphRangesDefault());
        foreach (char c in "–—‘’“”•…‹›") builder.AddChar(c);
        builder.BuildRanges(out desktopRanges);
        builder.Destroy();
        DesktopFont = ImGui.GetIO().Fonts.AddFontFromFileTTF(System.IO.Path.Combine(assets, "Baloo2-SemiBold.ttf"), 64, default, desktopRanges.Data);
        DesktopBold = ImGui.GetIO().Fonts.AddFontFromFileTTF(System.IO.Path.Combine(assets, "Baloo2-ExtraBold.ttf"), 96, default, desktopRanges.Data);
        var fontData = Resources.ReadResource(Resources.MainFont);

        unsafe
        {
            var fontRange = GetFontRanges();

            fixed (byte* fontPtr = fontData)
            {
                FontText = ImGui.GetIO().Fonts.AddFontFromMemoryTTF(fontPtr, fontData.Length, FontTextSize, null, fontRange.Data);

                // fontPtr and FontRange must be kept pinned until the atlases have actually been built or else bad things will happen
                ImGui.GetIO().Fonts.Build();
            }

            fontRange.Free();
        }
        if (desktopRanges.Data != IntPtr.Zero) desktopRanges.Free();
        unsafe { ImGui.GetIO().NativePtr->FontDefault = FontText.NativePtr; }
    }

    // One interface frame (everything up to, not including, presenting it).
    bool inFrame;
    void RenderFrame()
    {
        inFrame = true;
        try
        {
            ImGuiSDL2Impl.Renderer_NewFrame();
            ImGuiSDL2Impl.SDL2_NewFrame();
            ImGui.NewFrame();

            CurrentView.Draw();

            if (ShowDebugInfo)
                DebugWindow();

            ImGui.Render();

            CurrentView.RawDraw();

            ImGuiSDL2Impl.RenderDrawData(ImGui.GetDrawData());
        }
        finally { inFrame = false; }
    }

    SDL.SDL_EventFilter? liveResizeWatch;
    int liveResizeThread;
    // SDL event watch: runs on whichever thread produced the event, synchronously, even inside Windows' resize loop.
    int OnWindowEvent(IntPtr userdata, IntPtr sdlEvent)
    {
        try
        {
            if (inFrame || CurrentView is null || Environment.CurrentManagedThreadId != liveResizeThread) return 0;
            // SDL_WindowEvent layout: type (4), timestamp (4), windowID (4), event (1).
            if (System.Runtime.InteropServices.Marshal.ReadInt32(sdlEvent) != (int)SDL.SDL_EventType.SDL_WINDOWEVENT) return 0;
            var kind = (SDL.SDL_WindowEventID)System.Runtime.InteropServices.Marshal.ReadByte(sdlEvent, 12);
            if (kind is not (SDL.SDL_WindowEventID.SDL_WINDOWEVENT_SIZE_CHANGED or SDL.SDL_WindowEventID.SDL_WINDOWEVENT_RESIZED or SDL.SDL_WindowEventID.SDL_WINDOWEVENT_EXPOSED)) return 0;
            if (sdlCtx.RefreshSize()) UpdateSize();
            RenderFrame();
            sdlCtx.Render();
        }
        catch (Exception ex) { Program.DebugLog("Live resize frame failed: " + ex.Message); }
        return 0;
    }

    internal void PushMainview()
    {
        HandlePushView(new DesktopView(this));
    }

    internal void ApplyImguiSettings()
    {
        // The Guide button can hand the controller to the interface even when "Control this app with a gamepad" is off.
        bool padDrivesApp = Program.Options.ControllerInput || Program.AppControlActive;
        sdlCtx.AcceptControllerInput = padDrivesApp;
        sdlCtx.DebugPrintSdlEvents = Program.Options.Debug.SDLEvents;

        // ImGui's own gamepad backend only listens to joystick 0 (often not the controller in use); PadNavigator does the job instead,
        // so it stays off. The controller reaches the interface when padDrivesApp and it is not forwarded to the Switch.
        ImGui.GetIO().ConfigFlags &= ~ImGuiConfigFlags.NavEnableGamepad;

        ShowDebugInfo = Program.Options.Debug.GuiDebug;
    }

    public void Entrypoint()
    {
        sdlCtx.CreateWindow(null);

        ImGuiSDL2Impl.InitForSDLRenderer(sdlCtx.WindowHandle, sdlCtx.RendererHandle);
        ImGuiSDL2Impl.Init(sdlCtx.RendererHandle);

        sdlCtx.UsingImgui = true;

        UpdateSize();

        Pads = new PadNavigator();
        Program.PadNavigatorOwnsBack = true;
        PushMainview();

        // While the window border is being dragged Windows runs its own modal loop and our loop does not get to draw, so the
        // picture stayed frozen until the mouse was released. The resize events are delivered inside that loop: draw from them.
        liveResizeThread = Environment.CurrentManagedThreadId;
        liveResizeWatch = OnWindowEvent;
        SDL.SDL_AddEventWatch(liveResizeWatch, IntPtr.Zero);

        while (true)
        {
            ExecutePendingActions();

            if (CurrentView is null)
                break;

            GuiMessage msg = GuiMessage.None;
            while ((msg = sdlCtx.PumpEvents(out var evt)) != GuiMessage.None)
            {
                Cap.OnEvent(true);

                if (msg == GuiMessage.Resize)
                    UpdateSize();
                else if (msg == GuiMessage.BackButton)
                {
                    CurrentView.BackPressed();
                    // Don't pass this event to the imgui backend so it will not lose focus when we return to the previous page
                    continue;
                }
                else if (msg == GuiMessage.KeyDown && evt.key.keysym.scancode is SDL.SDL_Scancode.SDL_SCANCODE_LSHIFT or SDL.SDL_Scancode.SDL_SCANCODE_RSHIFT)
                    ShiftDown = true;
                else if (msg == GuiMessage.KeyUp && evt.key.keysym.scancode is SDL.SDL_Scancode.SDL_SCANCODE_LSHIFT or SDL.SDL_Scancode.SDL_SCANCODE_RSHIFT)
                    ShiftDown = false;
                else if (msg == GuiMessage.KeyUp)
                    CurrentView.OnKeyPressed(evt.key.keysym);
                else if (msg == GuiMessage.Quit)
                    goto break_main_loop;
                else if (msg == GuiMessage.AppBackgrounded)
                    CurrentView.OnAppBackgrounded();

                ImGuiSDL2Impl.ProcessEvent(in evt);

            }

            if (Cap.Cap())
                continue;

            if (Pads is not null)
            {
                bool drivesApp = (Program.Options.ControllerInput || Program.AppControlActive) && !Program.ControllerInputForwardingActive;
                if (Pads.Poll(ImGui.GetTime(), drivesApp, CurrentView)) Cap.OnEvent(true);
            }
            RenderFrame();

            sdlCtx.Render();
        }
    break_main_loop:

        if (liveResizeWatch is not null) { SDL.SDL_DelEventWatch(liveResizeWatch, IntPtr.Zero); liveResizeWatch = null; }
        while (CurrentView != null)
        {
            HandlePopView();
        }

        OnExit?.Invoke();

        ImGuiSDL2Impl.Renderer_Shutdown();
        ImGuiSDL2Impl.SDL2_Shutdown();
        sdlCtx.DestroyWindow();
    }
}
