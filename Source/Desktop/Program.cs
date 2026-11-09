using SDL2;
using NxRemotePlay.Client.App;
using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.GUI.Components;
using NxRemotePlay.Client.Platform;
using System;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

[assembly: SuppressMessage("Usage", "CA2265:Comparing a span to 'null' might be redundant, the 'null' literal will be implicitly converted to a 'Span<T>.Empty'", Justification = "Auto generated code.", Target = "ImGuiNET", Scope = "namespaceanddescendants")]

namespace NxRemotePlay.Client
{
    public static class Program
    {
        public static SDLContext SdlCtx;

        // This is the static instance of the client app. This is initialized once at the start of the program.
        internal static IApplicationInstance Instance = null!;
        internal static StringTable Strings = new();

        static readonly StringBuilder InitializationError = new();

        public static readonly string DisplayVersion = typeof(Program).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "unknown";

        public static readonly string BuildID = ThisAssembly.Git.Commit.ToString();

        public static Options Options = new();

        // True while a controller is forwarded to the active stream. While this is set,
        // the same physical controller should drive the Switch exclusively - the client's
        // own gamepad-based UI navigation (B = Back, ImGui gamepad nav) is suppressed so
        // pressing B in-game doesn't also back out of the PC client.
        public static bool ControllerInputForwardingActive = false;
        // The Guide button handed the controller to this app (it is then not sent to the Switch).
        public static bool AppControlActive = false;
        // PadNavigator handles the controller's Back button; the raw joystick event mapping in SDLContext stands down.
        public static bool PadNavigatorOwnsBack = false;

        // Errors that would otherwise vanish with the window: a plain log next to the other diagnostics.
        internal static void LogError(string where, object error)
        {
            try
            {
                var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
                Directory.CreateDirectory(dir);
                File.AppendAllText(Path.Combine(dir, "desktop-errors.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} {where}: {error}{Environment.NewLine}");
            }
            catch (Exception) { }
        }
        public static void Main(string[] args)
        {
            RunApp();
        }

        public static void DebugLog(string message)
        {
            if (Options.Debug.Log)
                Console.WriteLine(message);
        }

        private static void RunApp()
        {
            try
            {
                AppDomain.CurrentDomain.UnhandledException += CurrentDomain_UnhandledException;
            }
            catch (Exception ex)
            {
                Console.WriteLine("Failed to set global exception handler: " + ex.ToString());
            }

            try
            {
                TaskScheduler.UnobservedTaskException += TaskScheduler_UnobservedTaskException;
            }
            catch (Exception ex)
            {
                Console.WriteLine("Failed to set tasl exception handler: " + ex.ToString());
            }

            try
            {
                DynamicLibraryLoader.Initialize();

                Console.WriteLine($"NX-Remote-Play {DisplayVersion}");
                Console.WriteLine("https://nxremoteplay.com");
                Console.WriteLine($"Build ID: {BuildID}\n");

                LoadSettings();
                LoadEnglishStrings();

                SdlCtx = new();
                Platform.Specific.Win.AntiInject.Initialize();
                Instance = new ClientApp();

                Instance.Initialize();

                Instance.Entrypoint();
            }
            catch (Exception ex)
            {
                LogError("main loop", ex);
                Console.WriteLine(ex.ToString());
                SDL.SDL_Quit();
            }
        }

        private static void LoadEnglishStrings()
        {
            try
            {
                Strings = Resources.LoadEnglishStrings();
            }
            catch (Exception ex)
            {
                AddInitializationError($"Failed to load the English text resource; using built-in English.\n{ex}");
            }
        }

        internal static void AddInitializationError(string error)
        {
            InitializationError.AppendLine(error);

            Console.ForegroundColor = ConsoleColor.Red;
            Console.WriteLine(error);
            Console.ResetColor();
        }

        private static void LoadSettings()
        {
            try
            {
                var set = SystemUtil.LoadSettingsString();
                if (set is not null)
                {
                    Program.Options = Options.FromJson(set);
                    Console.WriteLine("Settings loaded");
                }
                {
                    NxRemotePlay.Client.Targets.VirtualCameraFeed.Apply(Program.Options.VirtualCamera && NxRemotePlay.Client.Targets.VirtualCameraFeed.IsInstalled);
                    NxRemotePlay.Client.Targets.VirtualCameraFeed.CleanOldCopiesQuietly();
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Failed to load settings: {ex}");
            }

            if (Debugger.IsAttached)
                Options.Debug.Log = true;
        }

        private static void TaskScheduler_UnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
        {
            // A background task that failed and nobody awaited (for example while a connection was being torn down) must not take
            // the whole window with it: log it and carry on.
            LogError("unobserved task", e.Exception);
            Console.WriteLine("TaskScheduler_UnobservedTaskException: " + e.Exception.ToString());
            e.SetObserved();
        }

        private static void CurrentDomain_UnhandledException(object sender, UnhandledExceptionEventArgs e)
        {
            LogError("unhandled", e.ExceptionObject);
            Console.WriteLine("CurrentDomain_UnhandledException: " + e.ExceptionObject.ToString());
            SDL.SDL_Quit();
        }
    }
}
