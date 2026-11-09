using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NxRemotePlay.Client.Core
{
    public enum UsbLogLevel
    {
        Error,
        Warning,
        Debug,
        None
    }

    public enum SDLScaleMode
    {
        Linear,
        Nearest,
        Best
    }

    // Picture upscaling on the graphics card (GUI/GlUpscaler.cs): Off = the normal stretch, Fsr = AMD FSR 1 (EASU + RCAS), Cubic = Catmull-Rom with optional sharpening
    public enum UpscaleMode
    {
        Off,
        Fsr,
        Cubic
    }

    public enum SDLAudioMode
    {
        Compatible,
        Default,
        Auto
    }

    public class Options
    {
        public bool UncapStreaming;
        public bool UncapGUI;
        // Desktop stream screen: mask the session ID, addresses and links on screen while true.
        public bool StreamerMode;
        // Show the received frame rate over the picture (also in fullscreen / with the interface hidden).
        public bool ShowFpsOverlay;
        public string RecordingsPath = DefaultPlatformVideoPath();
        public string ScreenshotsPath = DefaultPlatformPicturePath();
        public bool HideSerials;

        public bool PlayerHotkeys = true;

        // (Windows only) Capture screenshots to clipboard by default
        public bool Windows_ScreenToClip = false;

        // Ffmpeg options
        public string? DecoderName;

        // SDL options
        public bool ForceSoftwareRenderer;
        public SDLScaleMode RendererScale = SDLScaleMode.Linear;
        public UpscaleMode Upscale = UpscaleMode.Off;
        public int UpscaleStrength = 75;
        public SDLAudioMode AudioPlayerMode = SDLAudioMode.Auto;

        // Sysmodule options
        public StreamingOptions Streaming = new();

        // Debug settings
        public DebugOptions Debug = new();
        public bool UseSIMDAcceleration = true;

        public int DefaultVolume = 60;

        public bool ControllerInput = true;

        // What the controller's Guide button does while streaming (Home on the Switch, or switch between playing and controlling the app).
        public GuideButtonMode GuideButton = GuideButtonMode.Home;

        // Remember connection details for reconnecting. The short per-session password
        // is stored as plain text in the user's local settings file.
        public string LastRelayServer = "";
        // Server selection: true = default server, false = CustomServer. null = never chosen
        // (older settings): derived from LastRelayServer. See ServerAddress.
        public bool? UseDefaultServer = null;
        public string CustomServer = "";
        public string LastRelayId = "";
        public string LastRelayPassword = "";
        public bool DesktopAutoUsb = true;
        public bool DesktopGamepad = true;
        public bool DesktopBrowserSharing = true;
        public int DesktopBrowserPort = 8443;
        public bool DesktopBrowserPublicSharing = true;
        public bool DesktopBrowserRouterMapping = true;
        public int DesktopBrowserMediaPort = 8444;
        public string DesktopBrowserCertificatePath = "";
        public bool DesktopBrowserAutomaticCertificate;
        public bool DesktopBrowserCertificateAgreementAccepted;
        public bool VirtualCamera = true;   // feed the decoded picture to the NX Remote Play Camera (only does anything once the camera is installed)

        public string ScaleHintForSDL => RendererScale switch
        {
            SDLScaleMode.Linear => "linear",
            SDLScaleMode.Nearest => "nearest",
            SDLScaleMode.Best => "best",
            _ => throw new NotImplementedException(),
        };

        public string GetFilePathForVideo()
        {
            var format = $"NxRemotePlay_{DateTime.Now:yyyy_MM_dd_HH_mm_ss}.mp4";
            return Path.Combine(RecordingsPath, format);
        }

        public string GetFilePathForScreenshot()
        {
            var format = $"NxRemotePlay_{DateTime.Now:yyyy_MM_dd_HH_mm_ss}.png";
            return Path.Combine(ScreenshotsPath, format);
        }



        static string DefaultPlatformVideoPath()
        {
            return Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        }

        static string DefaultPlatformPicturePath()
        {
            return Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
        }

        public string SerializeToJson()
        {
            return JsonSerializer.Serialize(this, OptionsJsonSerializer.Default.NxRemotePlayOptions);
        }

        public static Options FromJson(string json)
        {
            try
            {
                return JsonSerializer.Deserialize<Options>(json, OptionsJsonSerializer.Default.NxRemotePlayOptions)!;
            }
            catch (Exception ex)
            {
                Console.WriteLine("Failed to deserialize options from json: " + ex);
                return new Options();
            }
        }
    }

    // When using AOT we must use source generation for json serialization since reflection is not available
    [JsonSourceGenerationOptions(
        WriteIndented = true,
        PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        IncludeFields = true,
        IgnoreReadOnlyProperties = true)]
    [JsonSerializable(typeof(Options), TypeInfoPropertyName = "NxRemotePlayOptions")]
    internal partial class OptionsJsonSerializer : JsonSerializerContext
    {
    }
}
