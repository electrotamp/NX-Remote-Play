using System;
using System.Runtime.InteropServices;

namespace NxRemotePlay.Client.Platform.Specific.Win
{
    internal static class SetupApi
    {
        public static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

        [Flags]
        public enum GetClassDevsFlags
        {
            DIGCF_ALLCLASSES = 0x00000004,
            DIGCF_DEVICEINTERFACE = 0x00000010,
            DIGCF_DEFAULT = 0x00000001,
            DIGCF_PRESENT = 0x00000002,
            DIGCF_PROFILE = 0x00000008,
        }

        public class SafeDeviceInfoSetHandle : SafeHandle
        {
            public SafeDeviceInfoSetHandle()
                : base(INVALID_HANDLE_VALUE, true) { }

            public override bool IsInvalid => this.handle == INVALID_HANDLE_VALUE;
            protected override bool ReleaseHandle() => SetupDiDestroyDeviceInfoList(this.handle);
        }

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct SP_DEVINFO_DATA
        {
            public int Size;
            public Guid ClassGuid;
            public uint DevInst;
            public IntPtr Reserved;
            public static unsafe SP_DEVINFO_DATA Create() => new SP_DEVINFO_DATA { Size = sizeof(SP_DEVINFO_DATA) };
        }

        [DllImport(nameof(SetupApi), SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

        [DllImport(nameof(SetupApi), SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern unsafe SafeDeviceInfoSetHandle SetupDiGetClassDevs(
            IntPtr classGuid,
            string enumerator,
            IntPtr hwndParent,
            GetClassDevsFlags flags);

        [DllImport(nameof(SetupApi), SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern unsafe bool SetupDiEnumDeviceInfo(
            SafeDeviceInfoSetHandle deviceInfoSet,
            int memberIndex,
            ref SP_DEVINFO_DATA deviceInfoData);

        public const uint SPDRP_SERVICE = 0x00000004;

        [DllImport(nameof(SetupApi), SetLastError = true, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetupDiGetDeviceRegistryProperty(
            SafeDeviceInfoSetHandle deviceInfoSet,
            in SP_DEVINFO_DATA deviceInfoData,
            uint property,
            out uint propertyRegDataType,
            byte[] propertyBuffer,
            uint propertyBufferSize,
            out uint requiredSize);
    }
}