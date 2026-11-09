# Windows LibUsbDotNet finalizer fix

`UsbContext.cs` is a local copy of the vendored LibUsbDotNet source with its existing copyright/license preserved. `Client.csproj` uses this copy only for `NxRemotePlayTarget=windows`; other targets keep the original. Keep the original dependency source unchanged when maintaining this override.

An exception from `libusb_init` (notably a missing native DLL) leaves `OpenDevices` null. .NET still finalizes the failed object; upstream's Dispose then dereferences the uninitialized list and ends the process. The local Dispose guard releases any allocated context and returns when initialization did not finish.

`Test-Desktop.ps1` exercises both bundled-library enumeration and a missing-library fixture with forced GC/finalization. When updating the shared dependency, compare/reapply this fix or remove the override only when the upstream implementation and regression test demonstrate safe failed construction.
