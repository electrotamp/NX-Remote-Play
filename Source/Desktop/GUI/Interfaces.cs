using NxRemotePlay.Client.GUI.Components;
using SDL2;
using NxRemotePlay.Client.App;
using NxRemotePlay.Client.Core;

namespace NxRemotePlay.Client.GUI
{
    public abstract class View(ClientApp Owner)
    {
        public FramerateCapOptions RenderMode = FramerateCapOptions.Adaptive();
        protected readonly ClientApp Owner = Owner;

        public abstract void Draw();

        public virtual void ResolutionChanged() { }

        public virtual void DrawDebug() { }

        public virtual void RawDraw()
        {
            Program.SdlCtx.ClearScreen();
        }

        public virtual void BackPressed()
        {
            Owner.PopView();
        }

        public virtual void Created() { }
        public virtual void EnterForeground() { }
        public virtual void Destroy() { }
        public virtual void LeaveForeground() { }

        public virtual void OnKeyPressed(SDL.SDL_Keysym key) { }

        // A view that does its own controller navigation (Settings, with its two levels) says so; it then receives the controller as
        // explicit actions instead of ImGui navigation keys.
        public virtual bool WantsPadActions => false;
        public virtual void OnPadAction(PadAction action) { }

        // Fired when the app is backgrounded (Android/iOS) or the window is minimized
        // (desktop) - see SDLContext.PumpEvents. Views may override this notification.
        public virtual void OnAppBackgrounded() { }
    }
}
