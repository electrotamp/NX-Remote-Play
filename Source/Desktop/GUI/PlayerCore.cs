using ImGuiNET;
using NxRemotePlay.Client.App;
using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.GUI.Components;
using NxRemotePlay.Client.Platform;
using NxRemotePlay.Client.Sources;
using NxRemotePlay.Client.Targets;
using NxRemotePlay.Client.Targets.FileOutput;
using NxRemotePlay.Client.Targets.Player;
using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Threading;
using static SDL2.SDL;

namespace NxRemotePlay.Client.GUI
{
    internal class PlayerCore
    {
        internal readonly AudioPlayer? Audio;
        internal readonly VideoPlayer? Video;
        internal readonly PlayerManager Manager;
        public bool HasDecodedFrame { get; private set; }

        readonly FramerateCounter fps = new();

        SDL_Rect DisplayRect = new SDL_Rect();
        int RotationQuarterTurns = 0;
        // Where the picture may be drawn inside the window (0 width = the whole window, as the legacy player).
        int viewX, viewY, viewW, viewH;
        // Frames decoded and presented per second (updated once a second while drawing).
        public int DecodedFps { get; private set; }

        public PlayerCore(PlayerManager manager, bool legacyLoading = true)
        {
            Manager = manager;

            // SyncHelper is disabled if there is only a single stream
            var sync = new StreamSynchronizationHelper(manager.HasAudio && manager.HasVideo);

            if (manager.HasVideo)
            {
                Video = new(Program.Options.DecoderName);
                Video.Decoder.SyncHelper = sync;
                manager.VideoTarget.UseContext(Video.Decoder);

                // JPEG packets, when supplied by a compatible source, bypass the H.264 decoder.
                manager.OnScreenshotFrame += Video.SubmitScreenshotFrame;

                if (legacyLoading) InitializeLoadingTexture();

                fps.Start();
            }

            if (manager.HasAudio)
            {
                Audio = new(manager.AudioTarget);
                manager.AudioTarget.Volume = Program.Options.DefaultVolume / 100f;
            }

            manager.UseSyncManager(sync);
        }

        public void Start()
        {
            Manager.Begin();
            Audio?.Resume();
        }

        public void Destroy(int maximumWaitMilliseconds = 3000)
        {
            // A USB read can only notice cancellation when it times out, so never wait for it unboundedly.
            // Disposing the manager below closes the device, which ends any read still pending.
            try { Manager.Stop().Wait(Math.Max(0, maximumWaitMilliseconds)); } catch (Exception) { }

            // Dispose of unmanaged resources
            Audio?.Dispose();
            Video?.Dispose();

            Manager.Dispose();
        }

        public void ResolutionChanged()
        {
            var aspectRatio = (double)StreamInfo.VideoWidth / StreamInfo.VideoHeight;
            var portrait = RotationQuarterTurns is 1 or 3;
            if (portrait)
                aspectRatio = 1 / aspectRatio;

            var w = viewW > 0 ? viewW : (int)Program.SdlCtx.WindowSize.X;
            var h = viewH > 0 ? viewH : (int)Program.SdlCtx.WindowSize.Y;
            int originX = viewW > 0 ? viewX : 0, originY = viewH > 0 ? viewY : 0;

            int fittedWidth;
            int fittedHeight;

            if (w >= h * aspectRatio)
            {
                fittedWidth = (int)(h * aspectRatio);
                fittedHeight = h;
            }
            else
            {
                fittedHeight = (int)(w / aspectRatio);
                fittedWidth = w;
            }

            // SDL_RenderCopyEx rotates after scaling, so when rotating by 90°/270° we
            // swap destination width/height to keep the final on-screen bounds correct.
            DisplayRect.w = portrait ? fittedHeight : fittedWidth;
            DisplayRect.h = portrait ? fittedWidth : fittedHeight;
            DisplayRect.x = originX + w / 2 - DisplayRect.w / 2;
            DisplayRect.y = originY + h / 2 - DisplayRect.h / 2;
        }

        // Fits the picture inside this window rectangle (pixels). No-op when unchanged.
        public void SetViewport(int x, int y, int width, int height)
        {
            if (x == viewX && y == viewY && width == viewW && height == viewH) return;
            viewX = x; viewY = y; viewW = Math.Max(0, width); viewH = Math.Max(0, height);
            ResolutionChanged();
        }
        public void ClearViewport() => SetViewport(0, 0, 0, 0);
        // Where the picture actually appears on screen (after rotation), in window pixels.
        public (float X, float Y, float W, float H) ScreenRect => RotationQuarterTurns is 1 or 3
            ? (DisplayRect.x + (DisplayRect.w - DisplayRect.h) / 2f, DisplayRect.y + (DisplayRect.h - DisplayRect.w) / 2f, DisplayRect.h, DisplayRect.w)
            : (DisplayRect.x, DisplayRect.y, DisplayRect.w, DisplayRect.h);

        public void RotateClockwise()
        {
            RotationQuarterTurns = (RotationQuarterTurns + 1) & 3;
            ResolutionChanged();
        }

        // For imgui usage, this function draws the current frame
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public bool DrawAsync(bool renderLoading = true)
        {
            if (Video is null)
                return false;

            if (Video.DecodeFrame())
            {
                HasDecodedFrame = true;
                fps.OnFrame();
            }
            if (fps.GetFps(out var measured)) DecodedFps = measured;

            // Bypass imgui for this
            if (renderLoading || HasDecodedFrame) DrawTexture();

            // Signal we're presenting something to SDL to kick the decoding thread
            // We don't care if we didn't actually decode anything we just do it here
            // to do this on every vsync to avoid arbitrary sleeps on the other side
            Video.Decoder.OnFrameEvent.Set();

            return true;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        void DrawTexture()
        {
            if (Video is null)
                return;

            if (RotationQuarterTurns == 0)
            {
                if (GlUpscaler.TryDraw(Program.SdlCtx.RendererHandle, Video.TargetTexture, DisplayRect, Program.SdlCtx.DpiScale, StreamInfo.VideoWidth, StreamInfo.VideoHeight)) return;
                SDL_RenderCopy(Program.SdlCtx.RendererHandle, Video.TargetTexture, ref Video.TargetTextureSize, ref DisplayRect);
            }
            else
            {
                SDL_RenderCopyEx(Program.SdlCtx.RendererHandle, Video.TargetTexture, ref Video.TargetTextureSize, ref DisplayRect, RotationQuarterTurns * 90, IntPtr.Zero, SDL_RendererFlip.SDL_FLIP_NONE);
            }
        }

        private unsafe void InitializeLoadingTexture()
        {
            Program.SdlCtx.BugCheckThreadId();

            // This hardcodes YUV dats
            if (Video.TargetTextureFormat != SDL_PIXELFORMAT_IYUV)
                return;

            byte[] data = null;

            try
            {
                data = Resources.ReadResource(Resources.LoadingImage);
            }
            catch
            {
                // Don't care
            }

            if (data == null)
            {
                // Hardcoded buffer size for a 1280x720 YUV texture
                data = new byte[0x1517F0];
                // Fill with YUV white
                data.AsSpan(0, 0xE1000).Fill(0xFF);
                data.AsSpan(0xE1000, 0x119400 - 0xE1000).Fill(0x7F);
                data.AsSpan(0x119400).Fill(0x80);
            }

            fixed (byte* ptr = data)
                SDL_UpdateYUVTexture(Video.TargetTexture, ref Video.TargetTextureSize,
                    (nint)ptr, 1280, (nint)(ptr + 0xE1000), 640, (nint)(ptr + 0x119400), 640);
        }
    }

}
