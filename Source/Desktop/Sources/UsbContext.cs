using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;
using NxRemotePlay.Client.Core;
using System;
using System.Collections.Generic;
using System.Linq;

namespace NxRemotePlay.Client.Sources
{
    // Making UsbContext non static will remove requirement on libusb, it will be loaded only if actually using USB, this is useful on architectures where it's not supported
    // This class should be used as a singleton
    abstract class DvrUsbDevice : IDisposable
    {
        protected readonly DvrUsbContext Context;
        public readonly DeviceInfo Info;

        public IUsbDevice DeviceHandle { get; protected set; }

        protected DvrUsbDevice(DvrUsbContext context, DeviceInfo deviceInfo)
        {
            Context = context;
            Info = deviceInfo;

            // Bind this device to the connection info object
            Info.ConnectionHandle = this;
        }

        public virtual (UsbEndpointReader, UsbEndpointWriter) Open()
        {
            var (epIn, epOut) = (ReadEndpointID.Ep01, WriteEndpointID.Ep01);

            var reader = DeviceHandle.OpenEndpointReader(epIn, PacketHeader.MaxTransferSize, EndpointType.Bulk);
            var writer = DeviceHandle.OpenEndpointWriter(epOut, EndpointType.Bulk);

            return (reader, writer);
        }

        // Should call close and then initialize a new device handle
        public abstract bool TryReconnect();

        public virtual void Close()
        {
            DeviceHandle?.Dispose();
            DeviceHandle = null;
        }

        public virtual void Dispose()
        {
            Close();
        }
    }

    class DvrUsbContext
    {
        const ushort NxRemotePlayVid = 0x18D1;
        const ushort NxRemotePlayPid = 0x4EE0;

        static UsbContext LibUsbCtx = null;

        private UsbLogLevel _debugLevel;
        public UsbLogLevel DebugLevel
        {
            set
            {
                _debugLevel = value;
                LibUsbCtx.SetDebugLevel(value switch
                {
                    UsbLogLevel.Error => LibUsbDotNet.LogLevel.Error,
                    UsbLogLevel.Warning => LibUsbDotNet.LogLevel.Info,
                    UsbLogLevel.Debug => LibUsbDotNet.LogLevel.Debug,
                    _ => LibUsbDotNet.LogLevel.None,
                });
            }
            get => _debugLevel;
        }

        public DvrUsbContext(UsbLogLevel logLevel = UsbLogLevel.Error)
        {
            if (LibUsbCtx == null)
            {
                LibUsbCtx = new LibUsbDotNet.LibUsb.UsbContext();
            }

            DebugLevel = logLevel;
        }

        static bool MatchNxRemotePlayDevice(IUsbDevice device)
        {
            try
            {
                return device.VendorId == NxRemotePlayVid && device.ProductId == NxRemotePlayPid;
            }
            catch (Exception ex)
            {
                Program.DebugLog("Warning: failed to query device ID " + ex);
                return false;
            }
        }

        public DisposableCollection<DvrUsbDevice> FindNxRemotePlayDevices()
        {
            // This is hacky but libusb can't seem to get the device serial without opening it first
            // If the device is already opened by another instance of nxremoteplay it will print an error, suppress it by temporarily changing the log level 
            var old = DebugLevel;
            DebugLevel = UsbLogLevel.None;

            var res = LibUsbCtx.FindAll(MatchNxRemotePlayDevice).Select(x =>
            {
                try
                {
                    if (!x.TryOpen())
                        return null;

                    var serial = x.Info.SerialNumber.Trim();
                    x.Close();

                    var dev = DeviceInfo.TryParse(ConnectionType.Usb, serial, "usb device");
                    if (dev == null)
                        return null;

                    return new DesktopUsbDevice(this, dev, x);
                }
                catch (Exception ex)
                {
                    Program.DebugLog("Warning: failed to query device serial " + ex);
                    return null;
                }
            }).Where(x => x != null).Cast<DesktopUsbDevice>();

            DebugLevel = old;
            return new(res);
        }
    }

    class DesktopUsbDevice : DvrUsbDevice
    {
        public DesktopUsbDevice(DvrUsbContext ctx, DeviceInfo info, IUsbDevice dev) : base(ctx, info)
        {
            DeviceHandle = dev;
        }

        public override (UsbEndpointReader, UsbEndpointWriter) Open()
        {
            DeviceHandle.Open();

            if (!DeviceHandle.ClaimInterface(0))
                throw new Exception($"Couldn't claim device interface");

            return base.Open();
        }

        public override bool TryReconnect()
        {
            Close();
            using var devs = Context.FindNxRemotePlayDevices();

            var dev = devs.Where(X => X.Info.Serial == Info.Serial).FirstOrDefault();
            if (dev == null)
                return false;

            devs.ExcludeFromDispose(dev);

            DeviceHandle = dev.DeviceHandle;
            return true;
        }
    }
}
