using ImGuiNET;
using NxRemotePlay.Client.GUI;
using System;
using System.Collections.Generic;
using System.Linq;
using static SDL2.SDL;

namespace NxRemotePlay.Client.GUI.Components;

public enum PadAction { Up, Down, Left, Right, Accept, Back }

// Controller navigation for the interface. ImGui's own SDL backend only ever listens to joystick number 0, which on a PC with a
// real controller plus virtual ones (DS4Windows, Steam, an Xbox 360 emulation) is often not the controller in use, so nothing
// happened. This opens every game controller and follows the one that is actually being used (the same rule the streaming
// controller uses). A view that asks for it (WantsPadActions, e.g. Settings with its two levels) receives explicit actions;
// every other view gets the D-pad / left stick / A as keyboard navigation keys and B as Back.
internal sealed class PadNavigator
{
    static readonly ImGuiKey[] Keys = { ImGuiKey.UpArrow, ImGuiKey.DownArrow, ImGuiKey.LeftArrow, ImGuiKey.RightArrow, ImGuiKey.Space, ImGuiKey.Enter };
    static readonly PadAction[] Directions = { PadAction.Up, PadAction.Down, PadAction.Left, PadAction.Right };
    readonly List<nint> pads = new();
    readonly HashSet<ImGuiKey> held = new();
    readonly bool[] dirHeld = new bool[4];
    readonly double[] dirNext = new double[4];
    nint active;
    double nextScan, lastAction = -100;
    bool aDown, backDown;

    public string ActiveName { get; private set; } = "";
    public ushort ActiveVendor { get; private set; }
    // a controller button drove the interface in the last few seconds (rings are only drawn then, not for mouse users)
    public bool RecentlyUsed(double now) => now - lastAction < 10;

    public PadNavigator() { SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER); }

    void Scan()
    {
        foreach (var detached in pads.Where(p => SDL_GameControllerGetAttached(p) == SDL_bool.SDL_FALSE).ToArray())
        {
            pads.Remove(detached); SDL_GameControllerClose(detached);
            if (active == detached) active = 0;
        }
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i) == SDL_bool.SDL_TRUE)
            {
                int id = SDL_JoystickGetDeviceInstanceID(i);
                if (pads.Any(p => SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id)) continue;
                var pad = SDL_GameControllerOpen(i);
                if (pad != 0) pads.Add(pad);
            }
    }

    static bool InUse(nint pad)
    {
        if (pad == 0 || SDL_GameControllerGetAttached(pad) == SDL_bool.SDL_FALSE) return false;
        for (int b = 0; b < (int)SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_MAX; b++)
            if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b) != 0) return true;
        for (int a = 0; a < 4; a++)
            if (Math.Abs((int)SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)a)) > 8000) return true;
        return false;
    }

    // Call once per frame before the interface frame starts. driveApp: the controller currently belongs to the interface.
    // Returns true when it fed the interface something (so a frame is drawn at once).
    public bool Poll(double now, bool driveApp, View? view)
    {
        if (now >= nextScan) { nextScan = now + 1; Scan(); }
        // keep a controller that is being held; otherwise follow the first one with activity
        if (!InUse(active))
        {
            var used = pads.FirstOrDefault(InUse);
            if (used != 0) active = used;
            else if (active == 0 && pads.Count > 0 && ActiveName.Length == 0) active = pads[0];
        }
        bool attached = active != 0 && SDL_GameControllerGetAttached(active) == SDL_bool.SDL_TRUE;
        if (attached)
        {
            ActiveName = SDL_GameControllerName(active) ?? "";
            ActiveVendor = SDL_JoystickGetVendor(SDL_GameControllerGetJoystick(active));
        }

        var dir = new bool[4];   // up, down, left, right
        bool a = false, b = false;
        bool usable = driveApp && attached;
        if (usable)
        {
            bool Button(SDL_GameControllerButton button) => SDL_GameControllerGetButton(active, button) != 0;
            int lx = SDL_GameControllerGetAxis(active, SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(active, SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_LEFTY);
            // hysteresis: a stick that is already pushed stays pushed until it is clearly back near the middle
            int T(int i) => dirHeld[i] ? 10000 : 18000;
            dir[0] = Button(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -T(0);
            dir[1] = Button(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > T(1);
            dir[2] = Button(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -T(2);
            dir[3] = Button(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > T(3);
            a = Button(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_A);
            b = Button(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_B);
        }

        bool fed = false;
        var io = ImGui.GetIO();
        var want = new HashSet<ImGuiKey>();
        if (usable && view is { WantsPadActions: true })
        {
            // explicit actions; directions repeat while held
            for (int i = 0; i < 4; i++)
            {
                if (!dir[i]) continue;
                if (!dirHeld[i]) { view.OnPadAction(Directions[i]); dirNext[i] = now + .4; fed = true; }
                else if (now >= dirNext[i]) { view.OnPadAction(Directions[i]); dirNext[i] = now + .12; fed = true; }
            }
            if (a && !aDown) { view.OnPadAction(PadAction.Accept); fed = true; }
            if (b && !backDown) { view.OnPadAction(PadAction.Back); fed = true; }
        }
        else
        {
            // keyboard-navigation keys: D-pad / stick = arrows, A = Space (buttons, switches) and Enter (text boxes)
            if (dir[0]) want.Add(ImGuiKey.UpArrow);
            if (dir[1]) want.Add(ImGuiKey.DownArrow);
            if (dir[2]) want.Add(ImGuiKey.LeftArrow);
            if (dir[3]) want.Add(ImGuiKey.RightArrow);
            if (a) { want.Add(ImGuiKey.Space); want.Add(ImGuiKey.Enter); }
            if (b && !backDown) { view?.BackPressed(); fed = true; }
        }
        foreach (var key in Keys)
        {
            bool down = want.Contains(key);
            if (down == held.Contains(key)) continue;
            io.AddKeyEvent(key, down);
            if (down) held.Add(key); else held.Remove(key);
            fed = true;
        }
        for (int i = 0; i < 4; i++) dirHeld[i] = dir[i];
        aDown = a; backDown = b;
        if (fed) lastAction = now;
        return fed;
    }
}
