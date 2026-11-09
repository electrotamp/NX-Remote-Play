using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NxRemotePlay.Client.Core
{
    // English text defaults; the fixed English resource can override these values.
    internal class StringTable
    {
        // The following are general strings used in multiple places
        internal class GeneralTable
        {

            // Shown when the user requested to not show serial numbers in the UI
            public string PlaceholderSerial = "(serial hidden)";
        }

        internal class PlayerTable
        {

            // {0} is replaced with the name of the codec
            public string PlayerInitializationMessage = "Initializing video player with {0} codec.";
        }

        internal class ErrorTable
        {
            public string SettingsPathMissing = "Couldn't find a valid path to store settings";

            // This message does not appear as is but is appended to the following messages 
            public string VersionTroubleshooting = "Make sure to use the same version on both your PC and console, you may need to reboot your console.";
            public string ConsoleRejectWrongVersion = "Your console rejected the connection because it is running a different NxRemotePlay version.";
            public string InitialPacketError = "Error initiating communication, your console may be on an incompatible NxRemotePlay version.";
            public string InitialPacketWrongVersion = "Your console reports to be using an incompatible NxRemotePlay version.";

            // {0} is replaced with the internal error code
            public string UsbResetMessage = "USB warning: Couldn't communicate with the console ({0}). Resetting the connection...";
            public string UsbTimeoutError = "USB warning: Couldn't communicate with the console. Try entering a compatible game, unplugging your console or restarting it.";
        }

        public GeneralTable General = new();

        public PlayerTable Player = new();

        public ErrorTable Errors = new();

        // Enumerate text through generated JSON metadata so font building works under AOT.
        public IEnumerable<string> GetAllStringForFontBuilding()
        {
            var objects = StringTableSerializer.Default.NxRemotePlayStringTable.Properties
                .Where(prop => prop.Get != null)
                .Select(prop => (prop.PropertyType, prop.Get(this)))
                .ToArray();

            return objects.SelectMany(x =>
                StringTableSerializer.Default.GetTypeInfo(x.Item1)
                    .Properties
                    .Where(prop => prop.Get != null)
                    .Where(prop => prop.PropertyType == typeof(string))
                    .Select(prop => (string?)prop.Get(x.Item2))
                    .Where(str => str != null)
            );
        }
    }

    // When using AOT we must use source generation for json serialization since reflection is not available
    [JsonSourceGenerationOptions(
        WriteIndented = true,
        PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        IncludeFields = true,
        UseStringEnumConverter = true,
        IgnoreReadOnlyProperties = true)]

    [JsonSerializable(typeof(StringTable), TypeInfoPropertyName = "NxRemotePlayStringTable")]
    internal partial class StringTableSerializer : JsonSerializerContext
    {
    }
}
