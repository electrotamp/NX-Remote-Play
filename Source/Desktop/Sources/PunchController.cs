using System;
using System.Buffers.Binary;
using System.Threading.Tasks;
using System.Collections.Generic;
using System.Linq;
using NxRemotePlay.Client.Core;
using static SDL2.SDL;

namespace NxRemotePlay.Client.Sources;

// One desktop gamepad controls the same single Switch slot as the Android client.
internal sealed class PunchController(IInputSink source) : IDisposable
{
    nint controller;
    readonly List<nint> available = new();
    Task? sending;
    double nextScan;
    double homeUntil;
    bool touchPressed;
    ushort touchX, touchY;
    public void Touch(bool pressed, ushort x, ushort y) { touchPressed = pressed; touchX = x; touchY = y; }
    public void TapHome(double now) => homeUntil = now + .15;
    readonly GuideButtonLogic guide = new();
    public GuideButtonMode GuideMode;
    // The Guide button gave the controller to the app: nothing but the Guide button reaches the Switch while this is true.
    public bool AppControl => guide.AppControl;
    public string Name => controller == 0 ? "No gamepad connected" : SDL_GameControllerName(controller);
    public void Poll(double now, bool enabled = true)
    {
        if (enabled && now >= nextScan)
        {
            nextScan = now + 1;
            foreach (var detached in available.Where(p => SDL_GameControllerGetAttached(p) == SDL_bool.SDL_FALSE).ToArray())
            {
                available.Remove(detached); SDL_GameControllerClose(detached);
                if (controller == detached) controller = 0;
            }
            for (int i = 0; i < SDL_NumJoysticks(); i++)
                if (SDL_IsGameController(i) == SDL_bool.SDL_TRUE)
                {
                    int id = SDL_JoystickGetDeviceInstanceID(i);
                    if (available.Any(p => SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id)) continue;
                    var pad = SDL_GameControllerOpen(i);
                    if (pad != 0) available.Add(pad);
                }
        }
        bool Active(nint pad)
        {
            if (pad == 0 || SDL_GameControllerGetAttached(pad) == SDL_bool.SDL_FALSE) return false;
            for (int b = 0; b < (int)SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_MAX; b++)
                if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b) != 0) return true;
            for (int a = 0; a < 4; a++)
                if (Math.Abs((int)SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)a)) > 5000) return true;
            return SDL_GameControllerGetAxis(pad, SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000 ||
                SDL_GameControllerGetAxis(pad, SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        }
        // Keep a held controller stable. When idle, follow the first controller
        // with activity; never combine multiple physical controllers in one slot.
        if (!Active(controller))
        {
            var active = available.FirstOrDefault(Active);
            if (active != 0) controller = active;
            else if (controller == 0 && available.Count > 0) controller = available[0];
        }
        if (sending is { IsCompleted: false }) return;
        // Observe faults; the stream reader is responsible for reporting a lost session.
        if (sending?.IsFaulted == true) _ = sending.Exception;
        ulong keys = 0;
        short lx = 0, ly = 0, rx = 0, ry = 0;
        bool home = now < homeUntil;
        if (enabled && controller != 0 && SDL_GameControllerGetAttached(controller) == SDL_bool.SDL_TRUE)
        {
            var (guideHome, guideTap) = guide.Update(GuideMode, SDL_GameControllerGetButton(controller, SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_GUIDE) != 0, now);
            home |= guideHome;
            if (guideTap) { homeUntil = now + .15; home = true; }
            if (!guide.AppControl) ReadPad();
        }
        else guide.Reset();
        void ReadPad()
        {
            void Key(SDL_GameControllerButton button, int bit) { if (SDL_GameControllerGetButton(controller, button) != 0) keys |= 1UL << bit; }
            // Nintendo face-button positions, following the existing desktop player.
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_B, 0);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_A, 1);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_Y, 2);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_X, 3);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_LEFTSTICK, 4);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_RIGHTSTICK, 5);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 6);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 7);
            if (SDL_GameControllerGetAxis(controller, SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) keys |= 1UL << 8;
            if (SDL_GameControllerGetAxis(controller, SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) keys |= 1UL << 9;
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_START, 10);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_BACK, 11);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_LEFT, 12);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_UP, 13);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 14);
            Key(SDL_GameControllerButton.SDL_CONTROLLER_BUTTON_DPAD_DOWN, 15);
            short Axis(SDL_GameControllerAxis axis, bool invert = false)
            {
                int value = SDL_GameControllerGetAxis(controller, axis);
                if (Math.Abs(value) < 5000) return 0;
                return (short)Math.Clamp(invert ? -value : value, short.MinValue, short.MaxValue);
            }
            lx = Axis(SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_LEFTX); ly = Axis(SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_LEFTY, true);
            rx = Axis(SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_RIGHTX); ry = Axis(SDL_GameControllerAxis.SDL_CONTROLLER_AXIS_RIGHTY, true);
        }
        if (!enabled)
        {
            // "disconnect controller" per client. With the controller off this client sends no pad packets at all, not even
            // neutral refreshes, so the console's pad for it is released after 1 s of silence and the client no longer holds a player place
            // (it used to hold one for as long as it was connected, pushing the Lite's own controls to Player 3). HOME and the touchscreen still work.
            bool needs = home || homeWasOn || touchPressed || touchWasPressed;
            homeWasOn = home; touchWasPressed = touchPressed;
            if (!needs) return;
            sending = SendExtras(home, TouchPacket(touchPressed, touchX, touchY));
            return;
        }
        homeWasOn = home; touchWasPressed = touchPressed;
        sending = Send(keys, lx, ly, rx, ry, home, TouchPacket(touchPressed, touchX, touchY));
    }
    internal static byte[] Packet(ulong keys, short lx, short ly, short rx, short ry)
    {
        var packet = new byte[17]; packet[0] = 2;
        BinaryPrimitives.WriteUInt64LittleEndian(packet.AsSpan(1), keys);
        BinaryPrimitives.WriteInt16LittleEndian(packet.AsSpan(9), lx);
        BinaryPrimitives.WriteInt16LittleEndian(packet.AsSpan(11), ly);
        BinaryPrimitives.WriteInt16LittleEndian(packet.AsSpan(13), rx);
        BinaryPrimitives.WriteInt16LittleEndian(packet.AsSpan(15), ry);
        return packet;
    }
    internal static byte[] TouchPacket(bool pressed, ushort x, ushort y)
    {
        var packet = new byte[5]; packet[0] = pressed ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(1), x);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(3), y); return packet;
    }
    bool homeWasOn, touchWasPressed;
    async Task SendExtras(bool home, byte[] touch)
    {
        await source.SendInput(new byte[] { 3, home ? (byte)1 : (byte)0, 0, 0, 0 });
        await source.SendInput(touch);
    }
    async Task Send(ulong keys, short lx, short ly, short rx, short ry, bool home, byte[] touch)
    {
        await source.SendInput(Packet(keys, lx, ly, rx, ry));
        await source.SendInput(new byte[] { 3, home ? (byte)1 : (byte)0, 0, 0, 0 });
        await source.SendInput(touch);
    }
    public void Dispose()
    {
        foreach (var pad in available) SDL_GameControllerClose(pad);
        available.Clear();
        controller = 0;
        if (sending is not null) { try { sending.GetAwaiter().GetResult(); } catch { } }
    }
}
