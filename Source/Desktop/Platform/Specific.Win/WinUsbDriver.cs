using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using static NxRemotePlay.Client.Platform.Specific.Win.SetupApi;

namespace NxRemotePlay.Client.Platform.Specific.Win
{
    internal enum UsbDriverState
    {
        Unknown,       // not Windows, or Windows would not answer
        DeviceAbsent,  // no Switch plugged in
        Bound,         // a driver (WinUSB) is serving the Switch
        Missing,       // a Switch is plugged in and Windows has no driver for it
    }

    // The Switch presents itself as VID 18D1 / PID 4EE0 and needs the WinUSB driver. Google's signed Android USB driver
    // contains an INF that binds WinUSB to it; it is downloaded, hash-checked and installed with one permission prompt.
    internal static class WinUsbDriver
    {
        const string HardwareId = "USB\\VID_18D1&PID_4EE0";
        const string DriverUrl = "https://dl.google.com/android/repository/usb_driver_r13-windows.zip";
        const string DriverSha256 = "360b01d3dfb6c41621a3a64ae570dfac2c9a40cca1b5a1f136ae90d02f5e9e0b";
        const int ErrorCancelled = 1223; // the user declined the permission prompt
        const int ExitRebootRequired = 3010;

        public static UsbDriverState GetState()
        {
            {}
            try
            {
                using var set = SetupDiGetClassDevs(IntPtr.Zero, HardwareId, IntPtr.Zero, GetClassDevsFlags.DIGCF_ALLCLASSES | GetClassDevsFlags.DIGCF_PRESENT);
                if (set.IsInvalid) return UsbDriverState.Unknown;
                bool any = false;
                var info = SP_DEVINFO_DATA.Create();
                for (int i = 0; SetupDiEnumDeviceInfo(set, i, ref info); i++)
                {
                    any = true;
                    var buffer = new byte[512];
                    // The service name is empty until a driver has been installed for the device.
                    if (SetupDiGetDeviceRegistryProperty(set, info, SPDRP_SERVICE, out _, buffer, (uint)buffer.Length, out uint size) &&
                        size > 2 && Encoding.Unicode.GetString(buffer, 0, (int)size).TrimEnd('\0').Length > 0)
                        return UsbDriverState.Bound;
                }
                return any ? UsbDriverState.Missing : UsbDriverState.DeviceAbsent;
            }
            catch (Exception ex)
            {
                Trace.WriteLine("USB driver check failed: " + ex);
                return UsbDriverState.Unknown;
            }
        }

        // Returns a short result for the user. Throws only for cancellation.
        public static async Task<string> InstallAsync(CancellationToken token)
        {
            string folder = Path.Combine(Path.GetTempPath(), "NxRemotePlay-usb-driver-" + Guid.NewGuid().ToString("N"));
            try
            {
                Directory.CreateDirectory(folder);
                byte[] archive;
                using (var http = new HttpClient { Timeout = TimeSpan.FromSeconds(60) })
                    archive = await http.GetByteArrayAsync(DriverUrl, token).ConfigureAwait(false);
                if (!Convert.ToHexString(SHA256.HashData(archive)).Equals(DriverSha256, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("The downloaded USB driver did not match its expected checksum, so it was not installed.");
                using (var zip = new ZipArchive(new MemoryStream(archive)))
                    zip.ExtractToDirectory(folder);
                string inf = Path.Combine(folder, "usb_driver", "android_winusb.inf");
                if (!File.Exists(inf)) throw new IOException("The downloaded USB driver is missing its installer file.");

                // pnputil needs administrator rights: Windows shows its own permission prompt once.
                var start = new ProcessStartInfo("pnputil.exe", $"/add-driver \"{inf}\" /install")
                {
                    Verb = "runas",
                    UseShellExecute = true,
                    WindowStyle = ProcessWindowStyle.Hidden,
                };
                int exit;
                try
                {
                    using var process = Process.Start(start)!;
                    await process.WaitForExitAsync(token).ConfigureAwait(false);
                    exit = process.ExitCode;
                }
                catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == ErrorCancelled)
                {
                    return "The USB driver install was cancelled. Press USB Switch to try again.";
                }
                if (exit != 0 && exit != ExitRebootRequired)
                    throw new IOException($"Windows could not install the USB driver (pnputil exit code {exit}).");

                for (int i = 0; i < 20 && GetState() == UsbDriverState.Missing; i++)
                    await Task.Delay(500, token).ConfigureAwait(false);
                return GetState() == UsbDriverState.Missing
                    ? "USB driver installed. Unplug the USB cable and plug it back in."
                    : exit == ExitRebootRequired ? "USB driver installed. Restart Windows if the Switch is not found."
                    : "USB driver installed.";
            }
            finally
            {
                try { Directory.Delete(folder, true); } catch { }
            }
        }
    }
}
