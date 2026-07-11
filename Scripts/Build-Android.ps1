[CmdletBinding()]
param(
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Shipping',

    [ValidateSet('APK', 'AAB')]
    [string]$PackageType = 'APK',

    [string]$EngineDir,
    [string]$OutputDir,
    [string]$Device,

    [switch]$Install,
    [switch]$Launch,
    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$SkipCook,
    [switch]$NoCompileEditor,
    [switch]$Distribution
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectDir = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $ProjectDir 'UEGaussianSplatting.uproject'
$ProjectName = [IO.Path]::GetFileNameWithoutExtension($ProjectFile)

function Resolve-EngineDir {
    param([string]$RequestedPath)

    $candidates = @(
        $RequestedPath,
        $env:UE_ENGINE_DIR,
        'D:\Software\Epic Games\UE_5.5',
        'C:\Program Files\Epic Games\UE_5.5'
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        $root = [IO.Path]::GetFullPath($candidate)
        if (Test-Path (Join-Path $root 'Engine\Build\BatchFiles\RunUAT.bat')) {
            return $root
        }
    }

    throw 'UE 5.5 was not found. Pass -EngineDir or set UE_ENGINE_DIR.'
}

function Resolve-Adb {
    $commands = @('adb.exe', 'adb')
    foreach ($command in $commands) {
        $found = Get-Command $command -ErrorAction SilentlyContinue
        if ($found) { return $found.Source }
    }

    $sdkRoots = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT) | Where-Object { $_ }
    foreach ($sdkRoot in $sdkRoots) {
        $candidate = Join-Path $sdkRoot 'platform-tools\adb.exe'
        if (Test-Path $candidate) { return $candidate }
    }

    throw 'adb was not found. Configure the Android SDK and ANDROID_HOME or ANDROID_SDK_ROOT.'
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$ArgumentList)

    Write-Host "`n> $FilePath $($ArgumentList -join ' ')" -ForegroundColor Cyan
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path $ProjectFile)) {
    throw "Project file does not exist: $ProjectFile"
}

$EngineDir = Resolve-EngineDir $EngineDir
$AndroidTarget = Join-Path $EngineDir 'Engine\Binaries\Android\UnrealGame.target'
$AndroidTargetPlatform = Join-Path $EngineDir 'Engine\Binaries\Win64\Android\UnrealEditor-AndroidTargetPlatform.dll'
if (-not (Test-Path $AndroidTarget) -or -not (Test-Path $AndroidTargetPlatform)) {
    throw 'The UE Android Target Platform component is missing. In Epic Games Launcher open UE 5.5 Options and install Android.'
}

# UE 5.5 requires the Android 34 / Build Tools 34 / NDK r25b toolchain.
# Android Studio often installs only its newest SDK, which UBT rejects.
$AndroidSdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$RequiredSdkPaths = @(
    (Join-Path $AndroidSdkRoot 'platforms\android-34'),
    (Join-Path $AndroidSdkRoot 'build-tools\34.0.0'),
    (Join-Path $AndroidSdkRoot 'cmake\3.22.1'),
    (Join-Path $AndroidSdkRoot 'ndk\25.1.8937393')
)
$MissingSdkPaths = @($RequiredSdkPaths | Where-Object { -not (Test-Path $_) })
if ($MissingSdkPaths.Count -gt 0) {
    $SetupAndroid = Join-Path $EngineDir 'Engine\Extras\Android\SetupAndroid.bat'
    throw "The UE 5.5 Android SDK toolchain is incomplete. Missing: $($MissingSdkPaths -join ', '). Install Android SDK Command-line Tools (latest), then run: `"$SetupAndroid`" android-34 34.0.0 3.22.1 25.1.8937393"
}
$env:ANDROID_HOME = $AndroidSdkRoot
$env:ANDROID_SDK_ROOT = $AndroidSdkRoot
$env:NDKROOT = Join-Path $AndroidSdkRoot 'ndk\25.1.8937393'
$env:NDK_ROOT = $env:NDKROOT

if (-not $OutputDir) {
    $OutputDir = Join-Path $ProjectDir "Packaged\Android-$Configuration"
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$RunUAT = Join-Path $EngineDir 'Engine\Build\BatchFiles\RunUAT.bat'

$uatArgs = @(
    'BuildCookRun',
    "-project=$ProjectFile",
    '-nop4',
    '-utf8output',
    '-platform=Android',
    "-clientconfig=$Configuration",
    '-target=UEGaussianSplatting',
    '-cook',
    '-build',
    '-stage',
    '-pak',
    '-iostore',
    '-package',
    '-prereqs',
    '-archive',
    "-archivedirectory=$OutputDir"
)

if ($Distribution) { $uatArgs += '-distribution' }
if ($PackageType -eq 'AAB') { $uatArgs += '-buildappbundle' }
if ($Clean) { $uatArgs += '-clean' }
if ($SkipBuild) { $uatArgs = $uatArgs | Where-Object { $_ -ne '-build' } }
if ($SkipCook) { $uatArgs = $uatArgs | Where-Object { $_ -ne '-cook' } }
if ($NoCompileEditor) { $uatArgs += '-nocompileeditor' }

Write-Host "UE:     $EngineDir"
Write-Host "Project: $ProjectFile"
Write-Host "Output:  $OutputDir"
Invoke-Checked $RunUAT $uatArgs

$extension = if ($PackageType -eq 'AAB') { '*.aab' } else { '*.apk' }
$packages = @(Get-ChildItem -Path $OutputDir -Filter $extension -File -Recurse |
    Sort-Object LastWriteTime -Descending)
if ($packages.Count -eq 0) {
    throw "Packaging succeeded but no $extension was found under $OutputDir."
}

$package = $packages[0]
Write-Host "`nPackage created: $($package.FullName)" -ForegroundColor Green

if ($Install -or $Launch) {
    if ($PackageType -ne 'APK') {
        throw 'An AAB cannot be installed directly with adb. Use -PackageType APK or a store test track.'
    }

    $adb = Resolve-Adb
    $adbPrefix = @()
    if ($Device) { $adbPrefix = @('-s', $Device) }

    & $adb start-server | Out-Null
    $deviceOutput = @(& $adb devices -l)
    $devices = @($deviceOutput | Select-String '^\S+\s+device(?:\s|$)')
    if (-not $Device -and $devices.Count -ne 1) {
        throw "Exactly one authorized Android device is required; found $($devices.Count). ADB output: $($deviceOutput -join ' | '). Enable USB debugging, accept the authorization prompt, or pass -Device <serial>."
    }

    if ($Install) {
        # UE packages may keep cooked data in an OBB next to the APK. Its
        # generated installer handles APK replacement, OBB upload and grants.
        $ueInstaller = Get-ChildItem -Path $OutputDir -Filter 'Install_*-arm64.bat' -File -Recurse |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($ueInstaller) {
            Push-Location $ueInstaller.DirectoryName
            try {
                $installerArgs = @('/c', $ueInstaller.Name)
                if ($Device) { $installerArgs += $Device }
                Invoke-Checked 'cmd.exe' $installerArgs
            }
            finally {
                Pop-Location
            }
        }
        else {
            Invoke-Checked $adb ($adbPrefix + @('install', '-r', '-d', $package.FullName))
        }
    }

    if ($Launch) {
        $packageName = (& $adb @adbPrefix shell pm list packages) |
            ForEach-Object { $_ -replace '^package:', '' } |
            Where-Object { $_ -match 'UEGaussianSplatting|GaussianSplatting' } |
            Select-Object -First 1
        if (-not $packageName) {
            throw 'Could not detect the application package name. Use -Install first or verify that the app is installed.'
        }
        Invoke-Checked $adb ($adbPrefix + @('shell', 'monkey', '-p', $packageName, '-c', 'android.intent.category.LAUNCHER', '1'))
    }
}
