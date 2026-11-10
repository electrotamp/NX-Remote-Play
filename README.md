<p align="center">
  <a href="https://nxremoteplay.com/demo/guide.html"><img src="https://nxremoteplay.com/demo/assets/logo.png" alt="NX Remote Play logo" width="96"></a>
</p>

<h1 align="center">NX Remote Play</h1>

<p align="center"><strong>Remote play for Nintendo Switch and Switch Lite.</strong><br>Your game runs on your Switch. You play it on another screen.</p>

<p align="center">
  <a href="#-beta-limitations-and-privacy"><img alt="Status: beta" src="https://img.shields.io/badge/status-beta-b655a4?style=for-the-badge"></a>
  <a href="#connect-windows"><img alt="Windows" src="https://img.shields.io/badge/Windows-0078D4?style=for-the-badge"></a>
  <a href="#connect-android"><img alt="Android" src="https://img.shields.io/badge/Android-14674F?style=for-the-badge"></a>
  <a href="#connect-browser"><img alt="Browser" src="https://img.shields.io/badge/Browser-805710?style=for-the-badge"></a>
  <a href="#connect-switch"><img alt="Nintendo Switch" src="https://img.shields.io/badge/Switch-C2402A?style=for-the-badge"></a>
</p>

<p align="center">
  <a href="https://github.com/ElectroTamp/NX-Remote-Play/releases"><strong>Downloads</strong></a> ·
  <a href="https://nxremoteplay.com/demo/guide.html"><strong>Setup guide</strong></a> ·
  <a href="https://nxremoteplay.com/wiki/"><strong>Wiki</strong></a> ·
  <a href="https://discord.gg/cMzfzqqFQj"><strong>Discord</strong></a> ·
  <a href="https://github.com/ElectroTamp/NX-Remote-Play/issues"><strong>Report an issue</strong></a>
</p>

---

NX Remote Play lets you play your Nintendo Switch on another screen, with sound and controller support. The game runs on your Switch; the other device shows its screen and sends your button presses back. You can use the HOME menu too.

Play on Windows, Android, Android TV, Google TV or another Switch. Browser play, including Safari on iPhone and iPad, uses a Windows PC connected to the Switch by USB.

> [!IMPORTANT]
> **The Switch running the game must have custom firmware and homebrew support.** The project is in beta, and compatibility is still being tested.

<p align="center">
  <strong>On this page</strong><br>
  <a href="#discord-and-personal-demos">Discord and demos</a> ·
  <a href="#-what-you-need">Requirements</a> ·
  <a href="#-getting-started">Setup</a> ·
  <a href="#-sharing-through-a-windows-pc">Sharing</a> ·
  <a href="#-other-features">Features</a> ·
  <a href="#-troubleshooting">Help</a> ·
  <a href="#-known-bugs">Known bugs</a> ·
  <a href="#-for-developers">Building</a>
</p>

---

## Discord and personal demos

Join the [Discord server](https://discord.gg/cMzfzqqFQj) to chat about NX Remote Play, find people to play with, or request a personal demo with me. We can arrange a time there.

---

## 📸 Screenshots

<p align="center">
  <a href="#connect-windows"><img src="https://nxremoteplay.com/demo/images/guide/windows-interface-v2.webp" alt="Windows app showing gameplay, playback tools and connection status" width="960"></a>
  <br><sub>Windows app</sub>
</p>

<table>
  <tr>
    <th align="center">Android</th>
    <th align="center">iPhone Safari</th>
  </tr>
  <tr>
    <td align="center" valign="middle"><a href="#connect-android"><img src="https://nxremoteplay.com/demo/images/guide/android/home-controls-v1.webp" alt="Switch HOME menu on Android with on-screen controls" width="520"></a></td>
    <td align="center" valign="middle"><a href="#connect-browser"><img src="https://nxremoteplay.com/demo/images/guide/iphone-safari-portrait-v3.webp" alt="iPhone Safari showing gameplay and touch controls; the server address is hidden" width="220"></a></td>
  </tr>
</table>

---

## ✅ What you need

- A Switch or Switch Lite running custom firmware with homebrew support.
- A device to play on, from the table below.
- A controller for Windows or TV, or your device’s built-in controls. Android and browser players also offer touch buttons.
- A USB cable that carries data if you want to connect the Switch to a Windows PC.

| Where you want to play | How it connects |
|---|---|
| **Windows PC** | USB cable, or the Switch’s Session ID and Password over the network. |
| **Android phone or tablet** | Android app, using the Switch’s Session ID and Password. |
| **Android TV or Google TV** | Android app installed from an APK file, plus a controller paired with the TV. |
| **iPhone, iPad or another browser device** | A shared link from a Windows PC that has the Switch connected by USB. |
| **Another Switch with homebrew support** | NX Remote Play installed on both consoles, using the first Switch’s Session ID and Password. |

The apps use the project’s connection service to find one another. You do not need to run your own server.

---

## 🚀 Getting started

### Step 1: Download the apps

App downloads are listed under **[GitHub Releases](https://github.com/ElectroTamp/NX-Remote-Play/releases)**. GitHub’s **Code → Download ZIP** gives you the source code, not an app installer.

The first beta includes the Switch, Android and Windows apps, plus the background host for the Switch. The Windows app provides browser play. Server programs are not included.

The release package has three app folders:

| Folder | What to do with it |
|---|---|
| `Switch SD Card Files` | Copy its contents to the Switch’s SD card root. Keep the supplied folders and files together. |
| `Android` | Install the Android app file (`.apk`) on your device. |
| `Desktop-Windows` | Extract the whole folder on your PC and run `NX-Remote-Play.exe`. Keep the other files beside it. |

Use matching versions on all devices. Read the package’s installation and privacy notes before installing.

### Step 2: Set up the Switch

1. Back up the SD card and check the release’s firmware requirements.
2. Copy everything **inside** `Switch SD Card Files` to the SD card root.
3. Restart into custom firmware so the background host and system patches can load.

For USB playback, continue with the Windows instructions below. To connect using codes:

1. Open **NX Remote Play** from the Switch’s homebrew launcher.
2. Choose **Host This Switch**, then **Turn On Hosting** if shown.
3. Note the **Session ID** and **Password**. Enter them on the device you want to play on.

The Switch running the game is the **host**. Sharing continues when you leave the app and start a game. **Turn Off Hosting** stops new connections; **Refresh ID and Password** changes the codes.

> [!TIP]
> When updating, keep the Switch’s own settings and codes. Do not copy one console’s settings onto another.

### Step 3: Connect your device

<a id="connect-windows"></a>

#### 🖥️ Windows

Run **`NX-Remote-Play.exe`** from the complete `Desktop-Windows` folder.

- **With a cable:** plug the Switch into the PC. The app can open it automatically, or choose **Connect By USB**. If it offers to install the USB driver on first use, approve the Windows prompt.
- **Over the network:** choose **Connect To A Switch**, enter the Switch’s **Session ID** and **Password**, then select **Join**.

Connect a controller and press a button. **Menu** has fullscreen, volume, HOME and controller options.

[Windows instructions and settings →](https://nxremoteplay.com/wiki/windows.html)

<a id="connect-android"></a>

#### 📱 Android phones and tablets

Install the APK from the `Android` folder, then open NX Remote Play. Choose **Connect To A Switch**, enter the Switch’s **Session ID** and **Password**, and tap **Join**.

Use the touch buttons or a paired controller. The **☰** menu has **Home**, **Controller** options and **Exit**.

[Android instructions and settings →](https://nxremoteplay.com/wiki/android.html)

<a id="connect-tv"></a>

#### 📺 Android TV and Google TV

Install the Android APK file on the TV or streaming device. This is called **sideloading**; the app is not installed through the TV’s Play Store.

1. Transfer the APK from your phone or computer, or use a USB drive if supported.
2. Open it with a file manager. If prompted, allow **Install unknown apps** for that file manager. Menu names vary by TV.
3. Install and open NX Remote Play. If it has no home-screen tile, find it under **Settings → Apps → See all apps**.
4. Pair a Bluetooth controller with the TV. In NX Remote Play, choose **Connect To A Switch**, enter the Switch’s codes and select **Join**.

You can turn that installation permission off afterward. This works with a Switch Lite and needs no Windows PC.

[TV installation details in the Android guide →](https://nxremoteplay.com/demo/guide.html)

<a id="connect-browser"></a>

#### 🌐 iPhone, iPad and other browsers

The Switch must be **connected by USB to a Windows PC running NX Remote Play**. Keep the PC and app running while you play.

1. Start the USB connection in the Windows app. If browser sharing is off, enable it in **Settings → Web Sharing**, then reconnect the Switch.
2. On the same network, open the **Local** link or scan its QR code. On another network, use the **Public** link after setting up outside access.
3. Open the link in Safari or another supported browser. Open **Menu → Sound** to enable audio; it starts off until you turn it on.

Play with the touch buttons or a connected controller. On iPhone and iPad, **Share → Add to Home Screen** in Safari lets you open the player in its own app-style window.

[Browser instructions →](https://nxremoteplay.com/wiki/browser.html)

<a id="connect-switch"></a>

#### 🎮 Another Switch

The second console also needs custom firmware and homebrew support. Install the matching Switch files, open NX Remote Play, choose **Connect To A Switch**, enter the first Switch’s codes and select **Connect**.

- Hold **− and +** together to press HOME on the Switch you are playing.
- Hold **L + R + ZL + ZR** to switch your buttons between controlling that Switch and using this app’s menus.
- Choose **Exit** in the floating menu to end the session.

[Switch instructions →](https://nxremoteplay.com/wiki/switch.html)

---

## 🤝 Sharing through a Windows PC

The Windows app can share a USB-connected Switch with other people. This is **Bridge Mode**: the PC passes the Switch’s picture and sound on to them. You can keep playing on the PC too.

- People using the **Windows, Android or Switch app** join with the Switch’s usual **Session ID** and **Password**.
- People using a **browser** open a **Local** or **Public** link from the Windows app.
- Up to **three remote players** can join, each with their own controller on the Switch.

**Local links** work on the PC’s network. **Public links** need the router to allow connections from outside. This depends on the router and internet provider; the link can appear even when outside access is blocked.

Browser links allow joining until the sharing session ends. Only send them to people you want to let play.

#### 🌍 Public link setup

The defaults are **TCP 8443** for the web page and **UDP 8444** for picture and sound. The app can ask the router to open these ports; otherwise, manual port forwarding may be needed. Some providers block incoming connections or share a public address between customers.

The app’s own HTTPS certificate may produce a browser warning. Optional automatic certificates need extra setup. Follow the [Windows sharing instructions](https://nxremoteplay.com/wiki/windows.html#share) for details.

---

## ✨ Other features

- **Windows screenshots and recording:** save PNG screenshots and MP4 videos, or copy a screenshot to the clipboard. Choose the save folders in Settings.
- **Windows virtual camera:** select **NX Remote Play Camera** in OBS, Discord, Zoom or another compatible program. It sends the game picture; set up sound separately in that program.
- **Android touch layout:** open **☰ → Controller → Edit layout**, move the buttons and choose **Save layout**. You can also hide the buttons when using a controller.

See the [setup guide](https://nxremoteplay.com/demo/guide.html) for everyday use and the [wiki](https://nxremoteplay.com/wiki/) for detailed settings.

---

## 🧰 Troubleshooting

| Problem | What to check |
|---|---|
| Cannot join | Hosting is on, the codes match and both apps are from the same release. If you changed the connection server, use the same one on both devices. |
| USB does not connect | Use a data cable, check the USB driver and select **Connect By USB** if you previously disconnected the session yourself. |
| Buttons do nothing | On the Switch, open **Controllers → Change Grip/Order**, then press **L and R** on the device you are playing from. Check that sending controller input is enabled in the app. |
| Browser has no sound | Turn on **Menu → Sound**. |
| Public link fails outside the home | Check the PC’s firewall, router ports and whether the internet provider allows incoming connections. |
| Picture stutters or falls behind | Compare with USB playback or a stronger network connection. Change one setting at a time and collect logs from both devices. |

### Collect logs and report a problem

Try to export logs soon after the problem happens. Note the time and time zone, what you were doing, and whether you were using USB, a local network, the internet or Windows Bridge Mode. Do not clear app data or repeat lots of tests before saving the files.

Collect logs from the **Switch running the game and the device you played on**. For browser play, include logs from all three: the **Switch, the Windows PC sharing it, and the browser**. Each export only collects that device’s logs. For Switch-to-Switch play, export on both consoles and label copies of the files **host** and **receiver**.

#### Switch

1. Open NX Remote Play on the console, then **Settings → Actions → Export logs**.
2. Choose **Create log file** and wait for **Saved to the SD card root.**
3. Copy **`NX-Remote-Play-logs.txt`** from the SD card root to the device you will use to report the issue. You can use an existing file-transfer setup, or fully shut down the Switch before removing the SD card and reading it on a computer.

Each export replaces that file. Keep a separate copy before exporting another test.

#### Windows

1. Reproduce the problem, then disconnect or stop the stream, but keep NX Remote Play open. Some diagnostic files cannot be included while they are still being written.
2. Open **Settings → Diagnostics & Logging → Export logs**.
3. The app opens its Diagnostics folder. Keep the new **`NX-Remote-Play-desktop-logs-YYYYMMDD-HHMMSS.zip`**.

If you need to find it again, paste **`%LOCALAPPDATA%\NX-Remote-Play\Diagnostics`** into File Explorer’s address bar. Attach the ZIP, not just a screenshot of the folder.

#### Android, Android TV and Google TV

1. After the problem, exit the streaming session and return to the app’s start screen.
2. Open **Settings → About → Export logs** and wait for **Logs saved to Downloads/NX-Remote-Play**.
3. Use your device’s file manager to open **Downloads → NX-Remote-Play** and find **`NX-Remote-Play-logs-export-<timestamp>.zip`**. On a TV, copy the ZIP to a phone or computer using your usual file-transfer method.

Ending a session or a failed connection can also save a diagnostic ZIP automatically: **`NX-Remote-Play-logs-<session-timestamp>.zip`** for a session, or **`NX-Remote-Play-logs-join-failed-<timestamp>.zip`** for a failed join. They are saved in the same folder. Use a file from the attempt you are reporting; check its date and time.

**Log export requires Android 10 or newer.** On Android 7–9, the app keeps private logs but cannot export them through this feature. Include your Android version and attach the Switch and any other available device logs instead.

#### Browser, including Safari on iPhone and iPad

1. Export **before refreshing or closing the player**: browser logs are held in that page and are lost when it closes or reloads.
2. Open **☰ Menu → Export logs**. If joining failed, use **Export logs** on the join screen instead.
3. Save **`NX-Remote-Play-web-log-YYYYMMDD-HHMMSS.txt`** (UTC) through the browser’s download or share prompt. On iPhone or iPad, choose **Save to Files** if offered and note the folder you select.

Also export the Windows PC’s ZIP and the host Switch’s text file for that same attempt.

#### Attach the files to a GitHub issue

**Review the files before attaching them.** Open ZIPs and check the text files inside for passwords, Session IDs, private sharing links, addresses or other details you do not want public. Redact private details in a copy. Do not attach your whole SD card, settings/configuration folders or owner keys. GitHub issues and uploaded attachments can be viewed by other people; files upload as soon as you attach them.

1. Sign in to GitHub and open **[NX Remote Play Issues](https://github.com/electrotamp/NX-Remote-Play/issues)**. Check whether someone has already reported the same problem.
2. Choose **New issue**, or add a comment to the existing matching issue. Use a title that describes the problem, such as “Android picture freezes after returning to HOME”.
3. Include the app/release versions, Switch model and firmware, receiving device and operating-system version, connection type, and the time/time zone of the problem. Describe how to reproduce it, what you expected and what happened instead. Label which log came from each device.
4. Drag the reviewed **ZIP or TXT files** into the issue description/comment box, or use the attachment control below it to select them. On a phone, use the file picker to select the saved files. Wait until each upload finishes and its attachment link appears in the text.
5. Submit the issue or comment. Keep your original exports in case more details are needed. If export itself failed, include the exact error and whichever other logs you could collect.

[Troubleshooting and log-export instructions →](https://nxremoteplay.com/wiki/help.html#logs)

---

## 🐞 Known bugs

- **Opening hbmenu through Album while a homebrew app is minimized locks up the console.** Close the homebrew app first. Holding **R** to open the regular Album still works. If the console locks up, hold the power button for about 12 seconds to restart it.

Found something else? [Report an issue](https://github.com/ElectroTamp/NX-Remote-Play/issues).

---

## 🔒 Beta limitations and privacy

Testing has included a Switch Lite, a regular Switch, Android, Windows and browsers. Compatibility is not fully checked. Some games need the supplied streaming patches, which depend on firmware compatibility. Picture quality, frame rate and delay vary with the setup.

The minimum Android version is **7.0**, though some devices may not work. The Windows app is **64-bit**. On macOS or Linux, use browser play through a Windows PC; this release has no separate desktop app for those systems.

> [!WARNING]
> **This beta can send the Session ID and Password to the connection service without encryption.** Someone monitoring that traffic could obtain them. Browser connections use HTTPS, but that does not protect the messages sent separately to the connection service.

Keep codes and links private. Logs may contain addresses and session details; the apps save them locally for you to share if needed. Updates are checked through GitHub and are not installed automatically. Driver downloads, Public link setup and optional certificates also contact outside services. See **`PRIVACY-AND-CONNECTIONS.txt`** in the package.

---

## 🔧 For developers

These notes describe the source layout and build process.

#### 📁 Source layout

| Path | What it contains |
|---|---|
| `Source/Host` | The Switch background program that captures the screen, sends picture and sound, and handles controller input |
| `Source/Donor` | Memory support used by the Switch host |
| `Source/Nro` | The Switch app for hosting controls and playing another Switch |
| `Source/Desktop` | Windows playback, USB connections and sharing |
| `Source/Android` | Android app |
| `Source/Rendezvous` | Connection-service code and browser components shared with the Windows app |
| `Source/Rendezvous.Embedded` | Builds the browser components into the Windows app |
| `Source/Shared` | Connection messages shared by the apps |
| `Source/VirtualCamera` | Windows virtual camera |

The connection service introduces devices; it does not relay picture and sound. The Windows app hosts the browser player. The Switch uses hardware H.264 encoding; app connections use UDP with TCP fallback. `PROTOCOL.md` describes the UDP video format.

### Building on Windows

From the full source checkout, double-click **`Build.cmd`**. Right-clicking `Build.ps1` usually does not work, because Windows blocks `.ps1` files by default.

It installs whatever is missing after one confirmation: Git, WSL, Docker Desktop, JDK 17, the Android SDK, the .NET 9 SDK and llvm-mingw. If Windows needs a restart for WSL, restart and run it again. From a Command Prompt, `Build.cmd -CheckOnly` shows what it would install without changing anything.

The Switch parts build in Docker against the newest Atmosphère, libnx and devkitPro packages. If those fail to apply or build, it falls back to the versions this release was tested with. A build against newer versions has not been tested on a console. Add `-Pinned` to use the tested Atmosphère and libnx from the start, and `-Refresh` to rebuild the Docker image. The versions used are recorded in `Build\logs\versions.txt`.

Output goes to **`Build\package`** and logs to **`Build\logs`**. To select components from a Command Prompt:

```bat
Build.cmd -Only Desktop,Android
```

> [!NOTE]
> Android cannot install the default unsigned APK. To sign it, provide `-Keystore`, `-KeyAlias`, and the `NXRP_ANDROID_STORE_PASSWORD` and `NXRP_ANDROID_KEY_PASSWORD` environment variables.

A full build assembles a release-style folder layout. Partial builds may leave out required files. Keep the Switch host, companion, app and firmware-specific patches together.

The connection server is built separately and is not included by `Build.ps1`:

```powershell
dotnet publish Source\Rendezvous\Rendezvous.csproj -c Release
```

---

## 📜 License and acknowledgements

The project license is [GNU GPL version 2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html). Third-party components keep their own licenses and notices; see the release’s **`LICENSE.txt`** and dependency notices for their terms.

NX Remote Play includes modified work derived from:

- [SysDVR](https://github.com/exelix11/SysDVR) — exelix and contributors.
- [switch-frame-tap](https://github.com/TomasUribe/switch-frame-tap) — Tomas Uribe and contributors.
- [Atmosphère / libstratosphere](https://github.com/Atmosphere-NX/Atmosphere) — Atmosphère contributors.

<p align="center"><sub>NX Remote Play is an independent homebrew project and is not affiliated with Nintendo.</sub></p>
