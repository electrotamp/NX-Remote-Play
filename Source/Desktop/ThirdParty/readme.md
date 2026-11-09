# ThirdParty folder

This folder contains third party dotnet libraries that are needed for NX-Remote-Play.

Their sources are kept here, updated for the .NET native library loading API, so the app loads the native libraries it bundles (Platform/Resources) instead of the ones NuGet packages would bring.

Changes from upstream include P/Invoke wrappers for some SDL backend and internal ImGui functions in ImGui.NET. ThirdPartyFixes holds a local Windows fix for LibUsbDotNet's UsbContext.cs; the original file is kept and excluded from the build.
