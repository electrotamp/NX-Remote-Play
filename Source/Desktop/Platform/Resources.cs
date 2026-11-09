using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.GUI.Components;
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;

namespace NxRemotePlay.Client.Platform
{
    internal static class Resources
    {
        static string BasePath = Path.Combine(AppContext.BaseDirectory, "runtimes");
        static string ResourcePath(string x) => Path.Combine(BasePath, "resources", x);

        public static string? SettingsStorePath()
        {
            return AppContext.BaseDirectory;
        }

        public static byte[] ReadResource(string path) => File.ReadAllBytes(path);

        public static string RuntimesFolder => BasePath;
        public static string MainFont { get; private set; } = ResourcePath("fonts/OpenSans.ttf");
        public static string LoadingImage => ResourcePath("loading.yuv");




        public static StringTable LoadEnglishStrings() =>
            JsonSerializer.Deserialize<StringTable>(ReadResource(ResourcePath("strings/english.json")), StringTableSerializer.Default.NxRemotePlayStringTable)
            ?? throw new Exception("Failed to deserialize English text.");
    }
}
