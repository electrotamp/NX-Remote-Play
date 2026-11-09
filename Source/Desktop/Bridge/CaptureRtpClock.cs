namespace NxRemotePlay.Client.Bridge;

// Map the Switch's microsecond capture clock to an RTP clock without losing
// pauses, accumulating per-frame rounding, or overflowing before RTP wraparound.
internal sealed class CaptureRtpClock(uint rate)
{
    bool initialized;
    ulong origin, lastCapture;
    uint initial;
    internal uint At(ulong capture, uint next)
    {
        if (!initialized || capture < lastCapture)
        {
            origin = capture; initial = next; initialized = true;
        }
        lastCapture = capture;
        ulong elapsed = capture - origin;
        return unchecked(initial + (uint)(elapsed / 1000000 * rate + elapsed % 1000000 * rate / 1000000));
    }
}
