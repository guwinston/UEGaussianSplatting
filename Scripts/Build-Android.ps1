[CmdletBinding()]
param(
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Shipping',

    [ValidateSet('APK', 'AAB')]
    [string]$PackageType = 'APK',

    [string]$EngineDir,
    [string]$OutputDir,
    [string]$Device,

    # Additional maps to cook into the package (e.g. -Map LargeScene or
    # -Map LargeScene,MobileDemo). Without -Map, only the GameDefaultMap is
    # cooked. Use -AllMaps to cook every map in the project.
    [string]$Map,
    [switch]$AllMaps,

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

# ---- Auto-detect UE version from the project file ----
$ProjectJson = Get-Content -Raw $ProjectFile | ConvertFrom-Json
$EngineAssociation = $ProjectJson.EngineAssociation
if (-not $EngineAssociation) {
    throw "Could not read EngineAssociation from $ProjectFile."
}

# Extract a short version number (e.g. "UE_5.6.1_TSplat" -> "5.6", "5.5" -> "5.5")
$UEVersion = $null
if ($EngineAssociation -match '(\d+\.\d+)') {
    $UEVersion = $Matches[1]
}
if (-not $UEVersion) {
    throw "Could not extract UE version from EngineAssociation '$EngineAssociation'. Pass -EngineDir or set UE_ENGINE_DIR."
}
Write-Host "Detected engine: $EngineAssociation (version $UEVersion)"

function Resolve-EngineDir {
    param([string]$RequestedPath)

    $candidates = [System.Collections.ArrayList]::new()

    # Explicit paths from parameter or environment
    if ($RequestedPath) { [void]$candidates.Add($RequestedPath) }
    if ($env:UE_ENGINE_DIR) { [void]$candidates.Add($env:UE_ENGINE_DIR) }

    # Try the Windows registry (Epic Games Launcher stores installed engine paths under this key)
    $regPath = "HKCU:\Software\Epic Games\Unreal Engine\Builds"
    if (Test-Path $regPath) {
        $regData = Get-ItemProperty -Path $regPath -Name $EngineAssociation -ErrorAction SilentlyContinue
        if ($regData -and $regData.$EngineAssociation) {
            [void]$candidates.Add($regData.$EngineAssociation)
        }

        # Also try with the bare version name (e.g. "5.5" or "UE_5.5")
        $altNames = @("UE_$UEVersion", $UEVersion)
        foreach ($alt in $altNames) {
            if ($alt -ne $EngineAssociation) {
                $altData = Get-ItemProperty -Path $regPath -Name $alt -ErrorAction SilentlyContinue
                if ($altData -and $altData.$alt) {
                    [void]$candidates.Add($altData.$alt)
                }
            }
        }
    }

    # Common installation directories
    $commonPaths = @(
        "D:\Software\Epic Games\UE_$UEVersion",
        "C:\Program Files\Epic Games\UE_$UEVersion",
        "D:\Software\Epic Games\$EngineAssociation",
        "C:\Program Files\Epic Games\$EngineAssociation"
    )
    foreach ($p in $commonPaths) { [void]$candidates.Add($p) }

    foreach ($candidate in $candidates) {
        $root = [IO.Path]::GetFullPath($candidate)
        if (Test-Path (Join-Path $root 'Engine\Build\BatchFiles\RunUAT.bat')) {
            return $root
        }
    }

    throw "UE $UEVersion (EngineAssociation: $EngineAssociation) was not found. Pass -EngineDir or set UE_ENGINE_DIR."
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

function Invoke-GradleLoopbackRecovery {
    param(
        [string]$EngineRoot,
        [string]$ProjectRoot,
        [string]$ArchiveRoot,
        [string]$BuildConfiguration,
        [string]$BuildPackageType
    )

    $uatLog = Join-Path $EngineRoot 'Engine\Programs\AutomationTool\Saved\Logs\Log.txt'
    if (-not (Test-Path $uatLog)) { return $false }

    $uatLogText = Get-Content -Raw $uatLog
    $loopbackError = 'java.io.IOException: Unable to establish loopback connection'
    if ($uatLogText -notmatch [regex]::Escape($loopbackError)) { return $false }

    $gradleDir = Join-Path $ProjectRoot 'Intermediate\Android\arm64\gradle'
    $runGradle = Join-Path $gradleDir 'rungradle.bat'
    if (-not (Test-Path $runGradle)) {
        throw "UAT hit the Gradle loopback error, but the generated wrapper was not found: $runGradle"
    }

    # Reuse the exact task printed by UAT. Fall back to the expected task only
    # if an engine-version log-format change prevents extraction.
    $taskMatches = [regex]::Matches($uatLogText, 'rungradle\.bat"\s+(?<Task>:app:[A-Za-z0-9]+)')
    if ($taskMatches.Count -gt 0) {
        $gradleTask = $taskMatches[$taskMatches.Count - 1].Groups['Task'].Value
    }
    else {
        $variant = if ($BuildConfiguration -eq 'Shipping') { 'Release' } else { 'Debug' }
        $gradleTask = if ($BuildPackageType -eq 'AAB') { ":app:bundle$variant" } else { ":app:assemble$variant" }
    }

    Write-Warning @"
UAT completed native build/cook, but Gradle's single-use daemon hit the Windows/JDK
loopback PipeImpl error. Recovering without a daemon: $gradleTask
"@

    # Gradle --no-daemon still forks a disposable daemon when the wrapper JVM does
    # not exactly match org.gradle.jvmargs. That path uses the broken NIO PipeImpl.
    # Launch the wrapper directly with the requested heap/open-module arguments and
    # Gradle's own instrumentation agent so the current JVM is reusable.
    if (-not $env:JAVA_HOME) {
        throw 'JAVA_HOME is not set; cannot run daemon-free Gradle recovery.'
    }
    $javaExe = Join-Path $env:JAVA_HOME 'bin\java.exe'
    $wrapperJar = Join-Path $gradleDir 'gradle\wrapper\gradle-wrapper.jar'
    $wrapperProperties = Join-Path $gradleDir 'gradle\wrapper\gradle-wrapper.properties'
    if (-not (Test-Path $javaExe) -or -not (Test-Path $wrapperJar) -or -not (Test-Path $wrapperProperties)) {
        throw 'Daemon-free Gradle recovery prerequisites are incomplete.'
    }

    $distributionLine = Get-Content $wrapperProperties |
        Where-Object { $_ -match '^distributionUrl=' } |
        Select-Object -First 1
    if ($distributionLine -notmatch 'gradle-(?<Version>[0-9.]+)-(?:all|bin)\.zip') {
        throw "Could not determine the Gradle version from $wrapperProperties."
    }
    $gradleVersion = $Matches['Version']
    $agentPattern = Join-Path $env:USERPROFILE (
        ".gradle\wrapper\dists\gradle-$gradleVersion-*\*\gradle-$gradleVersion\lib\agents\" +
        "gradle-instrumentation-agent-$gradleVersion.jar")
    $agent = Get-Item -Path $agentPattern -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $agent) {
        throw "Gradle $gradleVersion instrumentation agent was not found under the wrapper cache."
    }

    $gradleProperties = Join-Path $gradleDir 'gradle.properties'
    $jvmArgLine = Get-Content $gradleProperties |
        Where-Object { $_ -match '^org\.gradle\.jvmargs=' } |
        Select-Object -First 1
    if (-not $jvmArgLine) {
        throw "org.gradle.jvmargs was not found in $gradleProperties."
    }
    $heapArgs = (($jvmArgLine -replace '^org\.gradle\.jvmargs=', '').Trim() -split '\s+')

    $directJvmArgs = @(
        "-javaagent:$($agent.FullName)"
    ) + $heapArgs + @(
        '--add-opens=java.base/java.util=ALL-UNNAMED',
        '--add-opens=java.base/java.lang=ALL-UNNAMED',
        '--add-opens=java.base/java.lang.invoke=ALL-UNNAMED',
        '--add-opens=java.prefs/java.util.prefs=ALL-UNNAMED',
        '--add-opens=java.base/java.nio.charset=ALL-UNNAMED',
        '--add-opens=java.base/java.net=ALL-UNNAMED',
        '--add-opens=java.base/java.util.concurrent.atomic=ALL-UNNAMED',
        '-Dfile.encoding=GBK',
        '-Duser.country=CN',
        '-Duser.language=zh',
        '-Duser.variant',
        '-classpath',
        $wrapperJar,
        'org.gradle.wrapper.GradleWrapperMain',
        $gradleTask,
        '--no-daemon',
        '--stacktrace'
    )

    Push-Location $gradleDir
    try {
        Write-Host 'Running daemon-free Gradle recovery...' -ForegroundColor Yellow
        & $javaExe @directJvmArgs | Out-Host
        $gradleExitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    if ($gradleExitCode -ne 0) {
        throw "Daemon-free Gradle recovery failed with exit code $gradleExitCode."
    }

    $extension = if ($BuildPackageType -eq 'AAB') { '*.aab' } else { '*.apk' }
    $artifactRoots = @(
        (Join-Path $ProjectRoot 'Binaries\Android'),
        (Join-Path $gradleDir 'app\build\outputs')
    )
    $artifacts = @($artifactRoots |
        Where-Object { Test-Path $_ } |
        ForEach-Object { Get-ChildItem -Path $_ -Filter $extension -File -Recurse } |
        Where-Object { $_.Name -notlike 'AFS_*' } |
        Sort-Object LastWriteTime -Descending)
    $artifact = $artifacts | Select-Object -First 1
    if (-not $artifact) {
        throw "Gradle recovery succeeded but no $extension artifact was found."
    }

    [void](New-Item -ItemType Directory -Path $ArchiveRoot -Force)
    $archivedArtifact = Join-Path $ArchiveRoot $artifact.Name
    Copy-Item -LiteralPath $artifact.FullName -Destination $archivedArtifact -Force
    Write-Host "Recovered Gradle artifact: $archivedArtifact" -ForegroundColor Green
    return $true
}

if (-not (Test-Path $ProjectFile)) {
    throw "Project file does not exist: $ProjectFile"
}

$EngineDir = Resolve-EngineDir $EngineDir
$AndroidTarget = Join-Path $EngineDir 'Engine\Binaries\Android\UnrealGame.target'
$AndroidTargetPlatform = Join-Path $EngineDir 'Engine\Binaries\Win64\Android\UnrealEditor-AndroidTargetPlatform.dll'
$EngineSourceDir = Join-Path $EngineDir 'Engine\Source'
$InstalledBuildMarker = Join-Path $EngineDir 'Engine\Build\InstalledBuild.txt'

# A source-built engine has an Engine\Source tree and is NOT an installed build.
# Installed builds ship a prebuilt Android runtime (UnrealGame.target); source
# engines do not, and instead cross-compile the engine + game for Android on
# demand when -build is passed to UAT.
$isSourceEngine = (Test-Path $EngineSourceDir) -and -not (Test-Path $InstalledBuildMarker)
$isCustomEngine = $EngineAssociation -match '[A-Za-z]'

# The AndroidTargetPlatform editor DLL is required in every case: the editor
# cannot cook or package for Android without it.
if (-not (Test-Path $AndroidTargetPlatform)) {
    if ($isCustomEngine -or $isSourceEngine) {
        $buildCmd = "`"$EngineDir\Engine\Build\BatchFiles\Build.bat`" $ProjectName Editor Win64 Development -Project=`"$ProjectFile`""
        throw @"
The Android Target Platform editor support is missing for engine '$EngineAssociation' at:
  $EngineDir
Expected: $AndroidTargetPlatform

For a source-built engine, rebuild the editor (it compiles the AndroidTargetPlatform module) e.g.:
  $buildCmd
If Android was never set up for this engine, run once:
  $EngineDir\Engine\Extras\Android\SetupAndroid.bat
then rebuild the editor.
"@
    }
    else {
        throw "The UE Android Target Platform component is missing. In Epic Games Launcher open $EngineAssociation Options and install Android."
    }
}

# The prebuilt Android runtime (UnrealGame.target) is only shipped with installed
# builds. A source engine compiles it from source during -build, so a missing
# target file is not fatal there.
if (-not (Test-Path $AndroidTarget)) {
    if ($isSourceEngine) {
        Write-Warning @"
No prebuilt Android runtime (UnrealGame.target) was found under:
  $EngineDir\Engine\Binaries\Android
This is a source-built engine, so the game and engine modules will be cross-compiled
for Android on demand when -build runs. The first packaging will be slower as it
compiles the engine for Android. Make sure the Android NDK is installed (the script
verifies the SDK/NDK next) and that SetupAndroid.bat has been run once:
  $EngineDir\Engine\Extras\Android\SetupAndroid.bat
"@
    }
    elseif ($isCustomEngine) {
        throw @"
Android runtime (UnrealGame.target) is missing for custom engine '$EngineAssociation' at:
  $EngineDir
Expected: $AndroidTarget
This engine has no Engine\Source tree visible, so UBT cannot cross-compile from source.
Provide a prebuilt engine by either:
  1. Running: $EngineDir\Engine\Extras\Android\SetupAndroid.bat, then:
     $EngineDir\Engine\Build\BatchFiles\RunUAT.bat BuildGraph -Target="Make Installed Build Win64" -set:WithAndroid=true
  2. Or pass a prebuilt engine directory with -EngineDir.
"@
    }
    else {
        throw "The UE Android runtime (UnrealGame.target) is missing. In Epic Games Launcher open $EngineAssociation Options and install Android > Android Target Support."
    }
}

# UE SDK toolchain requirements. The canonical versions for this engine are
# what Engine\Extras\Android\SetupAndroid.bat installs by default (and what
# UBT's DEFAULT_NDK_VERSION / build-tools auto-select expect). Matching them
# avoids false negatives when Android Studio has newer components installed.
$AndroidSdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }

# Versions matching this engine's SetupAndroid.bat defaults (UE 5.6).
$ReqPlatform   = 'android-34'
$ReqBuildTools = '35.0.1'
$ReqCmake      = '3.22.1'
$ReqNdk        = '27.2.12479018'

$RequiredSdkPaths = @(
    (Join-Path $AndroidSdkRoot "platforms\$ReqPlatform"),
    (Join-Path $AndroidSdkRoot "cmake\$ReqCmake"),
    (Join-Path $AndroidSdkRoot "ndk\$ReqNdk")
)
# build-tools: UBT auto-selects the highest installed version, so accept the
# canonical version OR any installed build-tools directory.
$BuildToolsRoot = Join-Path $AndroidSdkRoot 'build-tools'
$BuildToolsOk = (Test-Path (Join-Path $BuildToolsRoot $ReqBuildTools)) -or
    ((Test-Path $BuildToolsRoot) -and @(Get-ChildItem $BuildToolsRoot -Directory -ErrorAction SilentlyContinue).Count -gt 0)

$MissingSdkPaths = @($RequiredSdkPaths | Where-Object { -not (Test-Path $_) })
if (-not $BuildToolsOk) { $MissingSdkPaths += "build-tools\$ReqBuildTools (or any build-tools)" }

if ($MissingSdkPaths.Count -gt 0) {
    $SetupAndroid = Join-Path $EngineDir 'Engine\Extras\Android\SetupAndroid.bat'
    throw "The UE $UEVersion Android SDK toolchain is incomplete. Missing: $($MissingSdkPaths -join ', '). Install Android SDK Command-line Tools (latest), then run: `"$SetupAndroid`" $ReqPlatform $ReqBuildTools $ReqCmake $ReqNdk"
}

$env:ANDROID_HOME = $AndroidSdkRoot
$env:ANDROID_SDK_ROOT = $AndroidSdkRoot
# Point NDKROOT at the canonical NDK for this engine (DEFAULT_NDK_VERSION).
$env:NDKROOT = Join-Path $AndroidSdkRoot "ndk\$ReqNdk"
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
if ($SkipCook) {
    $uatArgs = $uatArgs | Where-Object { $_ -ne '-cook' }
    $uatArgs += '-skipcook'
}
if ($NoCompileEditor) { $uatArgs += '-nocompileeditor' }

# Select which maps to cook. Without -Map or -AllMaps, UAT defaults to cooking
# only the GameDefaultMap (Demo) and its referenced assets.
$LaunchMap = $null
if ($AllMaps) {
    $uatArgs += '-cook_all'
    Write-Host "Cook:   all maps" -ForegroundColor DarkGray
}
elseif ($Map) {
    # Always include the GameDefaultMap so the app can launch, even if the user
    # only named additional maps.
    $cookMaps = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    [void]$cookMaps.Add('/Game/Demo')   # GameDefaultMap from DefaultEngine.ini

    # -Map accepts comma-separated names: -Map LargeScene or -Map LargeScene,MobileDemo
    foreach ($m in ($Map -split ',')) {
        # Normalize: strip .umap extension, strip whitespace.
        $mapName = $m.Trim() -replace '\.umap$', ''
        if ($mapName -eq '') { continue }
        # Add /Game/ prefix if the user passed a bare name like "LargeScene".
        if ($mapName -notmatch '^/') { $mapName = "/Game/$mapName" }
        [void]$cookMaps.Add($mapName)
        # -MapsToCook only puts the map in the package; it does not change the
        # startup map. Remember the first explicitly requested map for launch.
        if (-not $LaunchMap) { $LaunchMap = $mapName }
    }
    $mapsArg = ($cookMaps -join '+')
    $uatArgs += "-MapsToCook=$mapsArg"
    Write-Host "Cook:   $mapsArg" -ForegroundColor DarkGray
    Write-Host "Launch: $LaunchMap" -ForegroundColor DarkGray
}

Write-Host "UE:     $EngineDir"
Write-Host "Project: $ProjectFile"
Write-Host "Output:  $OutputDir"
Write-Host "`n> $RunUAT $($uatArgs -join ' ')" -ForegroundColor Cyan
& $RunUAT @uatArgs
if ($LASTEXITCODE -ne 0) {
    $uatExitCode = $LASTEXITCODE
    $recovered = Invoke-GradleLoopbackRecovery `
        -EngineRoot $EngineDir `
        -ProjectRoot $ProjectDir `
        -ArchiveRoot $OutputDir `
        -BuildConfiguration $Configuration `
        -BuildPackageType $PackageType
    if (-not $recovered) {
        throw "Command failed with exit code $uatExitCode."
    }
}

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
        # Do not use UE's generated Install_*-arm64.bat here: that installer
        # uninstalls the package first and therefore triggers a full reinstall.
        # adb install -r replaces the APK in place, preserves app data, and
        # does not require tapping the Android package installer UI.
        Write-Host "Installing with APK replacement (preserving app data)..." -ForegroundColor DarkGray
        Invoke-Checked $adb ($adbPrefix + @('install', '-r', '-d', '-g', $package.FullName))
    }

    if ($Launch) {
        $packageName = (& $adb @adbPrefix shell pm list packages) |
            ForEach-Object { $_ -replace '^package:', '' } |
            Where-Object { $_ -match 'UEGaussianSplatting|GaussianSplatting' } |
            Select-Object -First 1
        if (-not $packageName) {
            throw 'Could not detect the application package name. Use -Install first or verify that the app is installed.'
        }

        # -Map/-MapsToCook controls cooking only; it does not override the
        # project's GameDefaultMap. Android reads UECommandLine.txt from
        # external storage at startup, so write the requested map there before
        # launching and remove it when no map was requested.
        $commandLineDir = "/sdcard/UnrealGame/$ProjectName"
        $commandLinePath = "$commandLineDir/UECommandLine.txt"
        Invoke-Checked $adb ($adbPrefix + @('shell', 'am', 'force-stop', $packageName))
        Invoke-Checked $adb ($adbPrefix + @('shell', 'rm', '-f', $commandLinePath))
        if ($LaunchMap) {
            $commandLineFile = Join-Path $env:TEMP "UECommandLine-$ProjectName.txt"
            [IO.File]::WriteAllText($commandLineFile, "-Map=$LaunchMap`n", [Text.UTF8Encoding]::new($false))
            try {
                Invoke-Checked $adb ($adbPrefix + @('shell', 'mkdir', '-p', $commandLineDir))
                Invoke-Checked $adb ($adbPrefix + @('push', $commandLineFile, $commandLinePath))
            }
            finally {
                Remove-Item $commandLineFile -Force -ErrorAction SilentlyContinue
            }
        }
        Invoke-Checked $adb ($adbPrefix + @('shell', 'monkey', '-p', $packageName, '-c', 'android.intent.category.LAUNCHER', '1'))
    }
}
