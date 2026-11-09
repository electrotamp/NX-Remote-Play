using NxRemotePlay.Client.Sources;
using System;
using System.Text;

namespace NxRemotePlay.Client.Core
{
    public static class StreamInfo
    {
        public static readonly byte[] SPS = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
        public static readonly byte[] PPS = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };

        // This is VideoPayloadSize, which is also the max payload possible over the NxRemotePlay protocol
        public const int MaxPayloadSize = 0x54000;

        public const int VideoWidth = 1280;
        public const int VideoHeight = 720;

        public const int AudioPayloadSize = 0x1000;
        public const int MaxAudioBatching = 5;

        public const int AudioChannels = 2;
        public const int AudioSampleRate = 48000;
        public const int AudioSampleSize = sizeof(short);

        // Doesn't accoutn for batching, there may be more samples than this
        public const int MinAudioSamplesPerPayload = AudioPayloadSize / (AudioChannels * AudioSampleSize);
    }

    public enum ConnectionType
    {
        Net,
        Usb
    }

    public class DeviceInfo : IDisposable
    {

        public readonly ConnectionType Source;
        public readonly string ConnectionString;
        public readonly string Version;
        public readonly string Serial;

        public readonly string TextRepresentation;

        // Sources that need to carry a context for connection can store it here
        public object? ConnectionHandle;

        public DeviceInfo(ConnectionType source, string advertisementString, string connectionString)
        {
            this.Source = source;
            this.ConnectionString = connectionString;

            // Example beacon format:
            //      NxRemotePlay|6.0|00|NX00000000

            var parts = advertisementString.Split('|');

            if (parts[0] != "NxRemotePlay")
                throw new Exception("Invalid device beacon format");

            Version = parts[1];

            var protover = parts[2];
            if (protover.Length != "00".Length)
                throw new Exception("Invalid protocol version format");

            Serial = parts[3];

            var printSerial = Program.Options.HideSerials ? Program.Strings.General.PlaceholderSerial : Serial;

            TextRepresentation = Source == ConnectionType.Net ?
                $"NxRemotePlay {Version} - {printSerial} @ {ConnectionString}" :
                $"NxRemotePlay {Version} - {printSerial} @ USB";
        }

        public override string ToString() =>
            TextRepresentation;

        public static DeviceInfo? TryParse(ConnectionType source, string advertisementString, string connectionString)
        {
            try
            {
                return new DeviceInfo(source, advertisementString, connectionString);
            }
            catch
            {
                return null;
            }
        }

        public void Dispose()
        {
            if (ConnectionHandle is IDisposable disposable)
                disposable.Dispose();
        }
    }
}
