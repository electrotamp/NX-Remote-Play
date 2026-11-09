NX-Remote-Play 1.0.0-beta.1 - first beta

This is the first beta of NX-Remote-Play. It has been tried on a Switch Lite and a regular Switch, an Android phone, Windows and web browsers, but not on every device, firmware version or network. Expect rough edges. Every app has an Export logs button to help with a problem report (see PRIVACY-AND-CONNECTIONS.txt).

What this release is: the first beta of the clients and the Switch host only. That means the Switch host (sysmodule), the Switch receiver (NRO), the Android app and the Windows desktop app, which also does browser play and the USB bridge. The rendezvous server is not part of this release and no server programs are included: the apps use the default service play.nxremoteplay.com:7921, and Settings lets you pick another server. The server source is in the source repository.

Package contents:
- Switch SD Card Files: one combined SD-card root containing the host, memory companion, Switch receiver (switch/NX-Remote-Play.nro) and system patches. Copy its contents to the SD card root. The files belong together; use the receiver with the host and companion from this package. The apps create their own settings folder (config/nxremoteplay) on the console the first time they run. Keep each console's own credentials and settings when updating; do not copy identities between consoles.
- Android: Android receiver. It is signed with the same key as earlier builds of this app, so it installs over them as an update.
- Desktop-Windows: Windows receiver and USB/browser bridge. If a Switch is plugged in by USB and Windows has no driver for it, the app offers to install Google's signed Android USB driver (one Windows permission prompt). Video and audio travel over UDP when the console and network allow it, and over the TCP connection otherwise; UDP must be reachable for the best result.

Updates: each app (Switch, Android, Windows and the web page) checks GitHub at start and says when a newer version is out. Settings shows the result and the releases page, https://github.com/ElectroTamp/NX-Remote-Play/releases, where every version can be downloaded. Nothing is downloaded or installed automatically.

Known bug: opening hbmenu through Album while a homebrew app is minimized locks up the console. Close the homebrew app first. Holding R to open the regular Album still works. If the console locks up, hold the power button for about 12 seconds to restart it.

The manifest lists every file in this package with its hash. LICENSE.txt contains the general license and added third-party notices; stock dependency files retain their existing notices. Read PRIVACY-AND-CONNECTIONS.txt for credential and diagnostic-log behavior.
