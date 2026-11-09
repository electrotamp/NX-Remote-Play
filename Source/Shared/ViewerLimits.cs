namespace NXRemotePlay.Protocol;

// At most this many remote viewers watch one host at
// once, native and browser together. The local desktop player is separate and always allowed.
// Every layer that admits a viewer (rendezvous broker, desktop forwarders, Switch firmware's own
// constant) must agree on this number; lower it here and in the firmware if a hardware test shows
// the Switch cannot carry it unbridged.
public static class ViewerLimits
{
    public const int MaxRemoteViewers = 3;
}
