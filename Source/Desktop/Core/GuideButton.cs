namespace NxRemotePlay.Client.Core;

// What the controller's Guide button (the big centre button) does while streaming.
public enum GuideButtonMode
{
    Home,          // presses HOME on the Switch
    AppControl,    // each press switches the controller between playing on the Switch and moving around this app
    TapOrHold      // a quick tap presses HOME, holding it for half a second does the switch above
}

// The Guide button state machine, separate from SDL so it can be tested: feed it the button state every poll.
internal sealed class GuideButtonLogic
{
    public const double HoldSeconds = .5;
    // true: the controller drives this app's interface and nothing but the Guide button reaches the Switch
    public bool AppControl { get; private set; }
    bool down, fired;
    double since;

    public void Reset() { AppControl = false; down = false; fired = false; }

    // HomeHeld: send HOME while the button is held. HomeTap: send one short HOME press.
    public (bool HomeHeld, bool HomeTap) Update(GuideButtonMode mode, bool pressed, double now)
    {
        bool homeHeld = false, homeTap = false;
        switch (mode)
        {
            case GuideButtonMode.AppControl:
                if (pressed && !down) AppControl = !AppControl;
                break;
            case GuideButtonMode.TapOrHold:
                if (pressed && !down) { since = now; fired = false; }
                if (pressed && !fired && now - since >= HoldSeconds) { AppControl = !AppControl; fired = true; }
                // a tap sends HOME only while the Switch has the controller; while the app has it, nothing reaches the Switch
                if (!pressed && down && !fired && !AppControl) homeTap = true;
                break;
            default:
                AppControl = false;
                homeHeld = pressed;
                break;
        }
        down = pressed;
        return (homeHeld, homeTap);
    }
}
