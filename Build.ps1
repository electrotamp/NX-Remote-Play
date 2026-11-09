<#
Installs what is needed and builds the NX Remote Play clients and Switch host from this source tree on Windows.

Double-click Build.cmd, which starts this script. Windows blocks starting a .ps1 directly by default. From a Command Prompt:

    Build.cmd -CheckOnly
    Build.cmd -Only Desktop,Android

Result: Build\package, laid out like a release. Logs: Build\logs. The versions used: Build\logs\versions.txt.
Missing tools are installed after one confirmation (-Yes skips it). Run the same command again after a Windows restart if it asks for one.

The Switch parts build in Docker against the newest Atmosphere, libnx and devkitPro packages. If they do not apply or build,
it falls back to the versions the release was tested with. -Pinned uses those versions from the start. -Refresh rebuilds the image.

The APK is unsigned unless -Keystore and -KeyAlias are given, with the passwords in the environment
variables NXRP_ANDROID_STORE_PASSWORD and NXRP_ANDROID_KEY_PASSWORD.
#>
[CmdletBinding()]
param(
    [string]$OutputPath = '',
    [string[]]$Only = @('Host', 'Nro', 'Donor', 'Android', 'Desktop'),
    [string]$AtmospherePath = '',   # an already built, patched checkout; default: Build\deps\Atmosphere, created on first use
    [string]$Keystore = '',
    [string]$KeyAlias = '',
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Pinned,
    [switch]$Refresh,
    [switch]$Yes,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$PinnedAtmosphere = '6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8'   # the Atmosphere release the host was tested with
$PinnedLibnx = '20cbcfd90567f7bf84df7725f27e345e2164fa6b'        # first libnx with the 23.0.0 calls Atmosphere needs at that commit
$SwitchPackages = 'switch-dev switch-ffmpeg switch-glfw switch-mesa switch-curl switch-zlib switch-libdrm_nouveau switch-pkg-config'

$root = $PSScriptRoot
$work = Join-Path $root 'Build'
$logs = Join-Path $work 'logs'
$package = if ($OutputPath) { [IO.Path]::GetFullPath($OutputPath) } else { Join-Path $work 'package' }
$version = (Get-Content -LiteralPath (Join-Path $root 'Source\Desktop\release_version.txt') -Raw).Trim()
$wants = @{}
# powershell -File passes a comma-separated list as one string.
$Only = @($Only -split ',' | ForEach-Object { $_.Trim() })
foreach ($component in $Only) {
    if ($component -notin 'Host', 'Nro', 'Donor', 'Android', 'Desktop') { throw "Unknown component '$component'. Use Host, Nro, Donor, Android or Desktop." }
    $wants[$component] = $true
}
$needsDocker = $wants.Host -or $wants.Nro -or $wants.Donor
$androidPlatform = 'android-' + [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'Source\Android\app\build.gradle') -Raw), 'compileSdk\s+(\d+)').Groups[1].Value

function Test-Succeeds([string]$Program, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    & $Program @Arguments *> $null
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    return $code -eq 0
}

function Update-Environment {
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH', 'User') + ';' + $env:PATH
    $javaHome = [Environment]::GetEnvironmentVariable('JAVA_HOME', 'Machine')
    if ($javaHome) { $env:JAVA_HOME = $javaHome }
}

function Find-Docker {
    $command = Get-Command docker.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($base in $env:ProgramFiles, $env:LOCALAPPDATA) {
        foreach ($relative in 'Docker\Docker\resources\bin\docker.exe', 'Programs\DockerDesktop\resources\bin\docker.exe') {
            if ($base -and (Test-Path -LiteralPath (Join-Path $base $relative))) { return Join-Path $base $relative }
        }
    }
    return $null
}

function Find-Clang {
    $command = Get-Command 'x86_64-w64-mingw32-clang++' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $found = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter 'x86_64-w64-mingw32-clang++.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    return $null
}

function Get-AndroidSdkFolders { @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA 'Android\Sdk')) | Where-Object { $_ } }

function Find-AndroidSdk {
    foreach ($candidate in Get-AndroidSdkFolders) {
        if (Test-Path -LiteralPath (Join-Path $candidate "platforms\$androidPlatform")) { return $candidate }
    }
    return $null
}

function Find-Apksigner([string]$Sdk) {
    $folders = Get-ChildItem -LiteralPath (Join-Path $Sdk 'build-tools') -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'apksigner.bat') }
    $newest = $folders | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($newest) { return Join-Path $newest.FullName 'apksigner.bat' }
    return $null
}

function Find-Jdk {
    $candidates = @()
    if ($env:JAVA_HOME) { $candidates += Join-Path $env:JAVA_HOME 'bin\javac.exe' }
    $command = Get-Command javac.exe -ErrorAction SilentlyContinue
    if ($command) { $candidates += $command.Source }
    $candidates += @(Get-ChildItem -Path "$env:ProgramFiles\Microsoft\jdk-*\bin\javac.exe" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    foreach ($javac in $candidates) {
        if (-not (Test-Path -LiteralPath $javac)) { continue }
        $ErrorActionPreference = 'Continue'
        $text = (& $javac -version 2>&1 | Out-String)
        $ErrorActionPreference = 'Stop'
        if ($text -match 'javac (\d+)' -and [int]$Matches[1] -ge 17) { return Split-Path -Parent (Split-Path -Parent $javac) }
    }
    return $null
}

function Test-DotnetSdk {
    if (-not (Get-Command dotnet.exe -ErrorAction SilentlyContinue)) { return $false }
    return @(@(& dotnet --list-sdks) -match '^9\.').Count -gt 0
}

function Get-MissingTools {
    $missing = New-Object Collections.ArrayList
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { [void]$missing.Add('Git') }
    if ($needsDocker -and -not (Find-Docker)) {
        if (-not (Test-Succeeds 'wsl.exe' @('--status'))) { [void]$missing.Add('WSL') }
        [void]$missing.Add('Docker Desktop')
    }
    if ($wants.Android) {
        if (-not (Find-Jdk)) { [void]$missing.Add('JDK 17') }
        $sdk = Find-AndroidSdk
        if (-not $sdk -or ($Keystore -and -not (Find-Apksigner $sdk))) { [void]$missing.Add('Android SDK') }
    }
    if ($wants.Desktop) {
        if (-not (Test-DotnetSdk)) { [void]$missing.Add('.NET 9 SDK') }
        if (-not (Find-Clang)) { [void]$missing.Add('llvm-mingw') }
    }
    return , $missing
}

function Test-SigningInput {
    if (-not $Keystore) { return }
    if (-not (Test-Path -LiteralPath $Keystore)) { throw "Keystore not found: $Keystore" }
    if (-not $KeyAlias) { throw '-KeyAlias is required with -Keystore' }
    if (-not $env:NXRP_ANDROID_STORE_PASSWORD -or -not $env:NXRP_ANDROID_KEY_PASSWORD) { throw 'Set NXRP_ANDROID_STORE_PASSWORD and NXRP_ANDROID_KEY_PASSWORD to sign the APK' }
}

# Runs a program with its output in Build\logs\<label>.log and judges it by the exit code only:
# Windows PowerShell turns any stderr line (compiler warnings) into an error record.
function Invoke-Logged([string]$Label, [string]$Program, [string[]]$Arguments, [string]$Directory = $root) {
    $log = Join-Path $logs "$Label.log"
    Push-Location -LiteralPath $Directory
    try {
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments *>> $log
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = 'Stop'
        Pop-Location
    }
    if ($code -ne 0) {
        Get-Content -LiteralPath $log -Tail 25 | Write-Host
        throw "$Label failed (exit $code). Full log: $log"
    }
}

function Install-WithWinget([string]$Id, [string[]]$Extra = @()) {
    # Not judged by winget's exit code: the caller checks afterwards that the tool can be found.
    $ErrorActionPreference = 'Continue'
    & winget install --id $Id --exact --silent --disable-interactivity --accept-package-agreements --accept-source-agreements @Extra *>> (Join-Path $logs 'install.log')
    $ErrorActionPreference = 'Stop'
}

function Install-AndroidSdk {
    Update-Environment
    $env:JAVA_HOME = Find-Jdk
    $sdk = @(Get-AndroidSdkFolders)[0]
    $env:ANDROID_HOME = $sdk
    $repository = 'https://dl.google.com/android/repository/'
    $catalog = [xml](Invoke-WebRequest -UseBasicParsing ($repository + 'repository2-3.xml')).Content
    $sdkmanager = Join-Path $sdk 'cmdline-tools\latest\bin\sdkmanager.bat'
    if (-not (Test-Path -LiteralPath $sdkmanager)) {
        $tools = $catalog.DocumentElement.remotePackage | Where-Object { $_.path -eq 'cmdline-tools;latest' }
        $archive = ($tools.archives.archive | Where-Object { $_.'host-os' -eq 'windows' }).complete.url
        $download = Join-Path $env:TEMP $archive
        Invoke-WebRequest -UseBasicParsing ($repository + $archive) -OutFile $download
        $unpacked = Join-Path $env:TEMP 'nxrp-android-cmdline-tools'
        Remove-Item -LiteralPath $unpacked -Recurse -Force -ErrorAction SilentlyContinue
        Expand-Archive -LiteralPath $download -DestinationPath $unpacked
        New-Item -ItemType Directory -Force -Path (Join-Path $sdk 'cmdline-tools') | Out-Null
        Move-Item -LiteralPath (Join-Path $unpacked 'cmdline-tools') -Destination (Join-Path $sdk 'cmdline-tools\latest')
        Remove-Item -LiteralPath $download -Force
    }
    $buildTools = $catalog.DocumentElement.remotePackage | Where-Object { $_.path -match '^build-tools;\d+\.\d+\.\d+$' } |
        Sort-Object { [version]($_.path -replace '^build-tools;', '') } -Descending | Select-Object -First 1
    $log = Join-Path $logs 'install.log'
    $ErrorActionPreference = 'Continue'
    ("y`n" * 40) | & $sdkmanager "--sdk_root=$sdk" --licenses *>> $log
    $ErrorActionPreference = 'Stop'
    # A list file because cmd would split the package names at their semicolons.
    $list = Join-Path $logs 'android-packages.txt'
    [IO.File]::WriteAllLines($list, [string[]]@("platforms;$androidPlatform", $buildTools.path))
    Invoke-Logged 'install' $sdkmanager @("--sdk_root=$sdk", "--package_file=$list")
}

function Install-Tools([Collections.ArrayList]$Missing) {
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue) -and ($Missing | Where-Object { $_ -ne 'Android SDK' })) {
        # On a new Windows install winget exists but is not registered yet.
        Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe
        if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'winget is not available: install App Installer from the Microsoft Store, then run this again.' }
    }
    foreach ($tool in $Missing) {
        Write-Host "Installing $tool..."
        switch ($tool) {
            'Git' { Install-WithWinget 'Git.Git' }
            'WSL' { Start-Process wsl.exe -Verb RunAs -Wait -ArgumentList '--install', '--no-distribution' }
            'Docker Desktop' { Install-WithWinget 'Docker.DockerDesktop' @('--override', 'install --quiet --accept-license --backend=wsl-2') }
            'JDK 17' { Install-WithWinget 'Microsoft.OpenJDK.17' }
            'Android SDK' { Install-AndroidSdk }
            '.NET 9 SDK' { Install-WithWinget 'Microsoft.DotNet.SDK.9' }
            'llvm-mingw' { Install-WithWinget 'MartinStorsjo.LLVM-MinGW.UCRT' }
        }
        Update-Environment
    }
}

function Start-DockerEngine {
    $script:docker = Find-Docker
    # Docker's credential helper is next to docker.exe and has to be on the PATH.
    $env:PATH = (Split-Path -Parent $script:docker) + ';' + $env:PATH
    if (Test-Succeeds $script:docker @('info')) { return }
    Write-Host 'Starting Docker Desktop...'
    Start-Process (Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $script:docker))) 'Docker Desktop.exe')
    $deadline = (Get-Date).AddMinutes(5)
    while ((Get-Date) -lt $deadline) {
        if (Test-Succeeds $script:docker @('info')) { return }
        Start-Sleep -Seconds 5
    }
    throw 'Docker did not start. Restart Windows and run this again: a new WSL install needs a restart. If it still fails, open Docker Desktop to see why, and check that virtualization is enabled in the BIOS.'
}

function Resolve-SwitchVersions {
    if ($Pinned -or $script:usePinned) {
        $script:atmosphereCommit = $PinnedAtmosphere
        $script:libnxCommit = $PinnedLibnx
    } else {
        $script:atmosphereCommit = ((& git ls-remote https://github.com/Atmosphere-NX/Atmosphere.git refs/heads/master) -split '\s')[0]
        $script:libnxCommit = ((& git ls-remote https://github.com/switchbrew/libnx.git refs/heads/master) -split '\s')[0]
    }
    $script:image = "nxremoteplay-build:$($script:atmosphereCommit.Substring(0, 8))-$($script:libnxCommit.Substring(0, 8))"
}

function Get-Atmosphere {
    if ($AtmospherePath) { $script:atmosphere = (Resolve-Path -LiteralPath $AtmospherePath).Path; return }
    $script:atmosphere = Join-Path $work 'deps\Atmosphere'
    $marker = Join-Path $script:atmosphere '.nxremoteplay-ready'
    if ((-not $Refresh) -and (Test-Path -LiteralPath $marker) -and ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $script:image)) { return }
    Write-Host "Downloading Atmosphere $($script:atmosphereCommit.Substring(0, 8))..."
    if (-not (Test-Path -LiteralPath (Join-Path $script:atmosphere '.git'))) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $script:atmosphere) | Out-Null
        # Keep LF endings: this tree is built inside a Linux container.
        Invoke-Logged 'atmosphere' 'git' @('-c', 'core.autocrlf=false', 'clone', '--filter=blob:none', '--no-checkout', 'https://github.com/Atmosphere-NX/Atmosphere.git', $script:atmosphere)
        Invoke-Logged 'atmosphere' 'git' @('-C', $script:atmosphere, 'config', 'core.autocrlf', 'false')
    } else {
        Invoke-Logged 'atmosphere' 'git' @('-C', $script:atmosphere, 'fetch', 'origin')
    }
    Invoke-Logged 'atmosphere' 'git' @('-C', $script:atmosphere, 'checkout', '--force', '--detach', $script:atmosphereCommit)
    # Also removes the library built from the previous version.
    Invoke-Logged 'atmosphere' 'git' @('-C', $script:atmosphere, 'clean', '-fdxq')
    Invoke-Logged 'atmosphere' 'git' @('-C', $script:atmosphere, 'apply', '--whitespace=nowarn', (Join-Path $root 'Source\Host\atmosphere-patch\nxrp-libstratosphere.patch'))
    Set-Content -LiteralPath $marker -Value $script:image
}

function Build-Image {
    if ((-not $Refresh) -and (Test-Succeeds $script:docker @('image', 'inspect', $script:image))) { return }
    Write-Host 'Building the Docker image...'
    $context = Join-Path $work 'deps\image'
    New-Item -ItemType Directory -Force -Path $context | Out-Null
    $base = @('build', '-t', 'nxremoteplay-build:base', '-f', (Join-Path $script:atmosphere 'Dockerfile'), $context)
    if ($Refresh) { $base = @('build', '--no-cache') + $base[1..($base.Count - 1)] }
    Invoke-Logged 'image' $script:docker $base
    $dockerfile = @(
        'FROM nxremoteplay-build:base'
        'USER root'
        "RUN dkp-pacman -Syu --noconfirm --needed $SwitchPackages"
        "RUN cd /tmp && git clone https://github.com/switchbrew/libnx.git && cd libnx && git checkout $($script:libnxCommit) && make -j`$(nproc) && cd nx && make install && cd /tmp && rm -rf libnx"
    ) -join "`n"
    $path = Join-Path $context 'Dockerfile'
    [IO.File]::WriteAllText($path, $dockerfile + "`n", (New-Object Text.ASCIIEncoding))
    # No cache: the devkitPro packages have to be the current ones when an image is made.
    Invoke-Logged 'image' $script:docker @('build', '--no-cache', '-t', $script:image, '-f', $path, $context)
}

function Invoke-InContainer([string]$Label, [string]$Directory, [string[]]$Mounts, [string]$Command) {
    $arguments = @('run', '--rm', '-u', 'root', '-e', 'HOME=/root', '-e', "MAKEFLAGS=-j$Jobs", '-e', 'GIT_OPTIONAL_LOCKS=0',
        '-e', 'GIT_CONFIG_COUNT=1', '-e', 'GIT_CONFIG_KEY_0=status.showUntrackedFiles', '-e', 'GIT_CONFIG_VALUE_0=no') + $Mounts + @('-w', $Directory, $script:image, 'bash', '-lc', $Command)
    Invoke-Logged $Label $script:docker $arguments
}

function Build-Host {
    Write-Host 'Building the Switch host...'
    $ams = $script:atmosphere.Replace('\', '/')
    # The source is mounted inside the read-only Atmosphere tree, so its mount point must already exist.
    New-Item -ItemType Directory -Force -Path (Join-Path $script:atmosphere 'stratosphere\nxremoteplay') | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $script:atmosphere 'libraries\libstratosphere\lib\nintendo_nx_arm64_armv8a\release\libstratosphere.a'))) {
        Write-Host 'Building the Atmosphere library...'
        Invoke-InContainer 'atmosphere-library' '/ams/libraries/libstratosphere' @('-v', "${ams}:/ams") 'git config --global --add safe.directory /ams && make nx_release'
    }
    $hostDir = Join-Path $root 'Source\Host'
    foreach ($folder in 'build', 'out') { Remove-Item -LiteralPath (Join-Path $hostDir $folder) -Recurse -Force -ErrorAction SilentlyContinue }
    # Always build clean: make does not track DEFINES, so a stale object could keep excluded code.
    Invoke-InContainer 'host' '/ams/stratosphere/nxremoteplay' @('-v', "${ams}:/ams:ro", '-v', "$($hostDir.Replace('\', '/')):/ams/stratosphere/nxremoteplay") 'git config --global --add safe.directory /ams && make ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1 CONFIG_JSON=nxremoteplay.json nx_release'
    $out = Join-Path $hostDir 'out\nintendo_nx_arm64_armv8a\release'
    if ((Get-Item -LiteralPath (Join-Path $out 'nxremoteplay.npdm')).Length -ne 968) { throw 'Host NPDM must be 968 bytes: the module would not launch.' }
    Copy-ToSdCard (Join-Path $out 'nxremoteplay.nsp') "atmosphere\contents\$(Get-TitleId 'Source\Host\nxremoteplay.json')\exefs.nsp"
}

function Build-Nro {
    Write-Host 'Building the Switch receiver...'
    $nro = Join-Path $root 'Source\Nro'
    Invoke-InContainer 'nro' '/src' @('-v', "$($nro.Replace('\', '/')):/src") 'make clean && make'
    Copy-ToSdCard (Join-Path $nro 'NX-Remote-Play.nro') 'switch\NX-Remote-Play.nro'
}

function Build-Donor {
    Write-Host 'Building the memory companion...'
    # Built in a scratch copy so the source tree stays free of build files.
    $scratch = Join-Path $work 'donor'
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    Copy-Item (Join-Path $root 'Source\Donor\Makefile'), (Join-Path $root 'Source\Donor\donor.json') $scratch
    Copy-Item (Join-Path $root 'Source\Donor\source') (Join-Path $scratch 'source') -Recurse
    Invoke-InContainer 'donor' '/src' @('-v', "$($scratch.Replace('\', '/')):/src") 'make'
    Copy-ToSdCard (Join-Path $scratch 'nxrp-memory-donor.nsp') "atmosphere\contents\$(Get-TitleId 'Source\Donor\donor.json')\exefs.nsp"
}

function Build-SwitchParts {
    Resolve-SwitchVersions
    Get-Atmosphere
    Build-Image
    if ($wants.Host) { Build-Host }
    if ($wants.Nro) { Build-Nro }
    if ($wants.Donor) { Build-Donor }
}

function Build-Android {
    Write-Host 'Building the Android app...'
    $sdk = Find-AndroidSdk
    $env:JAVA_HOME = Find-Jdk
    $env:ANDROID_HOME = $sdk
    $android = Join-Path $root 'Source\Android'
    Invoke-Logged 'android' $env:COMSPEC @('/d', '/c', "`"$android\gradlew.bat`" --no-daemon :app:assembleRelease") $android
    $unsigned = Join-Path $android 'app\build\outputs\apk\release\app-release-unsigned.apk'
    $folder = Join-Path $package 'Android'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    Get-ChildItem -LiteralPath $folder -File | Remove-Item -Force
    $name = 'NX-Remote-Play-' + $version.Replace('1.0.0-', '')
    if ($Keystore) {
        $signer = Find-Apksigner $sdk
        Invoke-Logged 'android-sign' $signer @('sign', '--ks', $Keystore, '--ks-key-alias', $KeyAlias, '--ks-pass', 'env:NXRP_ANDROID_STORE_PASSWORD', '--key-pass', 'env:NXRP_ANDROID_KEY_PASSWORD', '--out', (Join-Path $folder "$name.apk"), $unsigned)
        Invoke-Logged 'android-sign' $signer @('verify', (Join-Path $folder "$name.apk"))
        # Only used for incremental installs from a development PC.
        Remove-Item -LiteralPath (Join-Path $folder "$name.apk.idsig") -Force -ErrorAction SilentlyContinue
    } else {
        Copy-Item -LiteralPath $unsigned -Destination (Join-Path $folder "$name-unsigned.apk") -Force
    }
}

function Build-Desktop {
    Write-Host 'Building the Windows app...'
    $destination = Join-Path $package 'Desktop-Windows'
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    Invoke-Logged 'desktop' 'dotnet' @('publish', (Join-Path $root 'Source\Desktop\Client.csproj'), '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true',
        '-p:NxRemotePlayTarget=windows', '-p:PublishTrimmed=false', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=false',
        '-p:IncludeAllContentForSelfExtract=false', '-p:EnableCompressionInSingleFile=false', '-p:AssemblyName=NX-Remote-Play', "-p:GitRoot=$root", '-o', $destination)
    # Standalone desktop hosting does not use the IIS worker-process module.
    Remove-Item -LiteralPath (Join-Path $destination 'aspnetcorev2_inprocess.dll') -Force -ErrorAction SilentlyContinue
    $native = Join-Path $destination 'runtimes\win-x64\native'
    if (-not (Test-Path -LiteralPath (Join-Path $native 'libusb-1.0.dll'))) { throw 'The published desktop app is missing its USB library.' }
    # The virtual camera ships with the other native files; the app copies and registers it when the user installs the camera.
    $camera = Join-Path $root 'Source\VirtualCamera'
    Invoke-Logged 'desktop-camera' (Find-Clang) @('-O2', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-cast-function-type', '-static', '-shared',
        '-o', (Join-Path $native 'NXRemotePlayCam.dll'), (Join-Path $camera 'nxcam.cpp'), (Join-Path $camera 'nxcam.def'), '-lstrmiids', '-luuid', '-lole32', '-loleaut32', '-ladvapi32', '-lgdi32', '-lwinmm', '-Wl,--kill-at')
}

function Get-TitleId([string]$Config) {
    $json = Get-Content -LiteralPath (Join-Path $root $Config) -Raw | ConvertFrom-Json
    return $json.title_id.Substring(2).ToUpperInvariant()
}

function Copy-ToSdCard([string]$Source, [string]$Relative) {
    $destination = Join-Path $package "Switch SD Card Files\$Relative"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination -Force
}

function Write-PackageFiles {
    $support = Join-Path $root 'Source\ReleaseSupport'
    foreach ($name in 'README.txt', 'LICENSE.txt', 'PRIVACY-AND-CONNECTIONS.txt') { Copy-Item -LiteralPath (Join-Path $support $name) -Destination (Join-Path $package $name) -Force }
    if ($needsDocker) {
        # The boot flag and system patches the host needs.
        $tree = Join-Path $support 'Switch SD Card Files'
        foreach ($file in Get-ChildItem -LiteralPath $tree -Recurse -File | Where-Object { $_.Extension -notin '.nsp', '.nro' }) {
            Copy-ToSdCard $file.FullName $file.FullName.Substring($tree.Length + 1)
        }
    }
    $base = (Resolve-Path -LiteralPath $package).Path
    $manifest = Join-Path $base 'manifest.json'
    Remove-Item -LiteralPath $manifest -Force -ErrorAction SilentlyContinue
    $files = @(Get-ChildItem -LiteralPath $base -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ Path = $_.FullName.Substring($base.Length + 1).Replace('\', '/'); Bytes = $_.Length; SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    [ordered]@{ Release = $version; Files = $files } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
}

function Write-Versions {
    $lines = New-Object Collections.ArrayList
    if ($needsDocker) {
        $atmosphereUsed = (& git -C $script:atmosphere rev-parse HEAD)
        [void]$lines.Add("Atmosphere: $atmosphereUsed")
        [void]$lines.Add("libnx: $($script:libnxCommit)")
        if ($script:usePinned) { [void]$lines.Add('The newest Atmosphere or libnx failed, so this used the tested versions.') }
        $ErrorActionPreference = 'Continue'
        $packages = & $script:docker run --rm $script:image bash -lc 'dkp-pacman -Q' 2>$null
        $ErrorActionPreference = 'Stop'
        [void]$lines.Add('devkitPro packages:')
        foreach ($line in $packages) { [void]$lines.Add("  $line") }
    }
    if ($wants.Desktop) { [void]$lines.Add(".NET SDK: $(& dotnet --version)") }
    if ($wants.Android) {
        $ErrorActionPreference = 'Continue'
        $javac = (& (Join-Path (Find-Jdk) 'bin\javac.exe') -version 2>&1 | Out-String).Trim()
        $ErrorActionPreference = 'Stop'
        [void]$lines.Add("JDK: $javac")
        [void]$lines.Add("Android platform: $androidPlatform")
    }
    Set-Content -LiteralPath (Join-Path $logs 'versions.txt') -Value $lines
}

# Started by right-click: the window closes when the script ends, so it has to wait for the user to read the result.
$parent = (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId
$startedFromExplorer = (Get-Process -Id $parent -ErrorAction SilentlyContinue).ProcessName -eq 'explorer'
$exitCode = 0

$saved = @{}
foreach ($name in 'NoDefaultCurrentDirectoryInExePath', 'PATH', 'JAVA_HOME', 'ANDROID_HOME') {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    # With this set, cmd refuses to run gradlew.bat from the current folder.
    $env:NoDefaultCurrentDirectoryInExePath = $null
    Test-SigningInput
    $missing = Get-MissingTools
    if ($CheckOnly) {
        if ($missing.Count) {
            Write-Host 'Would install:' -ForegroundColor Yellow
            foreach ($tool in $missing) { Write-Host "  - $tool" }
        } else {
            Write-Host "All tools found for: $($Only -join ', ')" -ForegroundColor Green
        }
        exit 0
    }

    New-Item -ItemType Directory -Force -Path $logs, $package | Out-Null
    Get-ChildItem -LiteralPath $logs -File | Remove-Item -Force
    if ($missing.Count) {
        Write-Host 'These tools are missing and will be installed at their newest versions:' -ForegroundColor Yellow
        foreach ($tool in $missing) { Write-Host "  - $tool" }
        Write-Host 'Continuing accepts their licences, including the Docker Subscription Service Agreement and the Android SDK licence. Windows may ask for permission several times.'
        if ($missing -contains 'Android SDK') { Write-Host "Google's Android command-line tools say they collect usage data about their commands." }
        if (-not $Yes -and (Read-Host 'Continue? (y/n)') -notmatch '^[yY]') { exit 1 }
        Install-Tools $missing
        $missing = Get-MissingTools
        if ($missing.Count) { throw "Could not install: $($missing -join ', '). Details: $(Join-Path $logs 'install.log'). If Windows was asked to enable something, restart and run this again." }
    }

    if ($needsDocker) {
        Start-DockerEngine
        try {
            Build-SwitchParts
        } catch {
            if ($Pinned) { throw }
            Write-Warning "The newest Switch dependencies failed, so this is falling back to the tested versions. Reason: $_"
            $script:usePinned = $true
            Build-SwitchParts
        }
    }
    if ($wants.Android) { Build-Android }
    if ($wants.Desktop) { Build-Desktop }
    Write-PackageFiles
    Write-Versions
    Write-Host "Done: $package" -ForegroundColor Green
    if (-not $Keystore -and $wants.Android) { Write-Host 'The APK is unsigned: Android will not install it until it is signed.' }
} catch {
    Write-Host "Build failed: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    if ($startedFromExplorer) { [void](Read-Host 'Press Enter to close this window') }
}
exit $exitCode
