using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace NxRemotePlay.Client.Platform
{
    internal static class SystemUtil
    {
        public static bool OpenURL(string url)
        {
            try
            {
                Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            }
            catch
            {
                try
                {
                    // hack because of this: https://github.com/dotnet/corefx/issues/10361
                    {
                        url = url.Replace("&", "^&");
                        Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
                    }
                }
                catch { }
                return false;
            }
            return true;
        }

        const string SettingsFileName = "options.json";

        public static string? LoadSettingsString()
        {
            var path = Resources.SettingsStorePath();

            if (string.IsNullOrWhiteSpace(path))
                throw new Exception(Program.Strings.Errors.SettingsPathMissing);

            var file = Path.Combine(path, SettingsFileName);
            if (File.Exists(file))
                return File.ReadAllText(file);

            return null;
        }

        public static void StoreSettingsString(string json)
        {
            var path = Resources.SettingsStorePath();
            if (string.IsNullOrWhiteSpace(path))
                throw new Exception(Program.Strings.Errors.SettingsPathMissing);

            if (!Directory.Exists(path))
                Directory.CreateDirectory(path);

            var file = Path.Combine(path, SettingsFileName);
            File.WriteAllText(file, json);
        }

    }
}
