using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace NxRemotePlay.Client.Platform
{
    internal static class DynamicLibraryLoader
    {
        static string ArchName => RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.X64 => "-x64",
            Architecture.X86 => "-x86",
            Architecture.Arm => "-arm",
            Architecture.Arm64 => "-arm64",
            _ => ""
        };

        static string BundledOsNativeFolder => Path.Combine(Resources.RuntimesFolder, $"win{ArchName}", "native");

        public static string? LibLoaderOverride = null;


        static readonly Regex versionNumberRegex = new Regex(@"^(.*?)[\.\-][\d\.]+\.(dylib|dll)$", RegexOptions.IgnoreCase);
        static string? StripVersionNumber(string libraryName)
        {
            var match = versionNumberRegex.Match(libraryName);
            if (match.Success)
                return match.Groups[1].Value + Path.GetExtension(libraryName);
            else
                return null;
        }

        static IEnumerable<string> FindNativeLibrary(string libraryName)
        {
            const string libext = ".dll";

            if (!libraryName.EndsWith(libext))
                libraryName += libext;

            // Is an override set ?
            if (!string.IsNullOrWhiteSpace(LibLoaderOverride))
                yield return Path.Combine(LibLoaderOverride, libraryName);

            // Is this a library we provide ?
            yield return Path.Combine(BundledOsNativeFolder, libraryName);

            // Maybe it's in the working directory
            yield return libraryName;
        }

        readonly static ConcurrentDictionary<string, IntPtr> LibraryCache = new();
        static IntPtr CachedLibraryloader(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
        {
            if (LibraryCache.TryGetValue(libraryName, out var result))
                return result;

            var loaded = TryLoadLibrary(libraryName);


            // Cache the failure so we don't try again
            // Dotnet will try to load the library again using its own resolver if we return IntPtr.Zero
            LibraryCache.TryAdd(libraryName, loaded);

            return loaded;
        }

        public static IntPtr TryLoadLibrary(string libraryName)
        {

            var paths = FindNativeLibrary(libraryName)
                .Concat(FindNativeLibrary("lib" + libraryName));

            foreach (var candidate in paths)
            {
                if (!File.Exists(candidate))
                {
                    continue;
                }


                if (NativeLibrary.TryLoad(candidate, out var result))
                    return result;

            }

            // Try to load the library from the OS.
            // This is not included in the above search because it's filtered by the File.Exists
            {

                var result = IntPtr.Zero;

                if (NativeLibrary.TryLoad(libraryName, out result))
                    return result;

                if (NativeLibrary.TryLoad($"lib{libraryName}", out result))
                    return result;
            }

            // Try again, but without the version number
            if (StripVersionNumber(libraryName) is string withoutNumber)
            {
                return TryLoadLibrary(withoutNumber);
            }

            return IntPtr.Zero;
        }

        public static void Initialize()
        {
            NativeLibrary.SetDllImportResolver(Assembly.GetExecutingAssembly(), CachedLibraryloader);
        }
    }
}
