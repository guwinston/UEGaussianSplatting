# Android 打包、安装与地图 Cook

在项目根目录打开 PowerShell。推荐使用 `Scripts\Build-Android.bat` 入口；它会以独立进程调用 PowerShell，并仅对本次执行绕过 Execution Policy。

## 常用命令

```powershell
# Shipping APK，默认 Cook GameDefaultMap（当前为 Demo）
.\Scripts\Build-Android.bat

# Development APK，打包后安装并启动唯一一台已授权的真机
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch

# Cook LargeScene，同时保留默认地图 Demo；然后安装并启动
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch -Map LargeScene

# Cook 多个地图，地图名用逗号分隔
.\Scripts\Build-Android.bat -Configuration Development -Map LargeScene,MobileDemo

# Cook 项目中的全部地图
.\Scripts\Build-Android.bat -Configuration Development -AllMaps

# 多台设备时指定 adb serial
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch -Device R5CTxxxxxxx

# 清理后重新打包
.\Scripts\Build-Android.bat -Configuration Development -Clean

# 使用项目 Android 设置中配置的正式签名生成应用商店 AAB
.\Scripts\Build-Android.bat -PackageType AAB -Distribution
```

脚本默认从 `UEGaussianSplatting.uproject` 的 `EngineAssociation` 自动识别引擎。本项目当前使用 `UE_5.6.1_TSplat`，脚本会依次尝试：

1. `-EngineDir <引擎目录>`；
2. 环境变量 `UE_ENGINE_DIR`；
3. Unreal 注册表中的引擎路径；
4. 常见安装目录。

如果自动检测不到自定义引擎，可显式指定：

```powershell
.\Scripts\Build-Android.bat -EngineDir D:\UGit\Engine -Configuration Development -Map LargeScene
```

也可以在当前终端设置：

```powershell
$env:UE_ENGINE_DIR = 'D:\UGit\Engine'
```

默认输出目录为 `Packaged\Android-<Configuration>`，也可以使用 `-OutputDir <目录>` 覆盖。

## 添加和切换地图

`-Map` 会把地图 Cook 进 APK，并在同时使用 `-Launch` 时通过 Android 的 `UECommandLine.txt` 将第一个指定地图作为本次启动地图。当前脚本会始终把默认地图 `/Game/Demo` 一起加入 Cook 列表，因此：

```powershell
# Content\LargeScene.umap -> /Game/LargeScene
.\Scripts\Build-Android.bat -Configuration Development -Map LargeScene

# Content\Maps\LargeScene.umap -> /Game/Maps/LargeScene
.\Scripts\Build-Android.bat -Configuration Development -Map /Game/Maps/LargeScene
```

也可以省略 `.umap` 后缀；脚本支持裸名称、`/Game/` 路径以及逗号分隔的多个地图。`-AllMaps` 会使用 UAT 的 `-cook_all` Cook 项目全部地图。

如果只执行打包而不传 `-Launch`，应用下次启动仍使用 `Config\DefaultEngine.ini` 中的 `GameDefaultMap`，当前值为 `/Game/Demo.Demo`。使用下面的命令可以让本次安装后启动时直接进入 LargeScene，无需修改默认地图：

```powershell
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch -Map LargeScene
```

如果希望永久把应用默认启动地图改为 LargeScene，也可以修改：

```ini
GameDefaultMap=/Game/LargeScene.LargeScene
```

然后重新打包。若 LargeScene 已经 Cook 进包，也可以通过项目中的控制台或调试入口执行：

```text
open LargeScene
```

## Android 环境要求

首次运行前需配置 Android SDK、NDK、CMake、Build Tools、JDK，并确保 Android 真机开启 USB 调试。当前项目只打包 **arm64 + Vulkan SM5**；`bBuildForES31=False`，普通 Mobile Vulkan ES3.1 后端尚未启用。

项目启用了 `bUseExternalFilesDir=True`，并通过 Android UPL 清空 UE 的 `StartupPermissions`、移除过时的存储权限声明。这样 Development APK 启动时不会再弹出 `Storage permission required`。修改 Android 配置或 UPL 后必须重新 Cook/打包；建议首次验证时使用 `-Clean`，避免复用旧的 manifest。

本项目当前使用 UE 5.6 自定义源码引擎，脚本检查以下 Android SDK 组件：

- Android Platform `android-34`；
- CMake `3.22.1`；
- NDK `27.2.12479018`；
- Build Tools `35.0.1`，或任意已安装的 Build Tools 版本（UBT 会自动选择）。

先在 Android Studio 的 SDK Manager 安装 **Android SDK Command-line Tools (latest)**，再执行引擎自带的配置脚本：

```bat
"D:\UGit\Engine\Engine\Extras\Android\SetupAndroid.bat" android-34 35.0.1 3.22.1 27.2.12479018
```

源码构建引擎可能显示以下警告：

```text
No prebuilt Android runtime (UnrealGame.target) was found...
```

对于包含 `Engine\Source` 的源码引擎，这是正常提示。执行 UAT 的 `-build` 时会按需交叉编译 Android 引擎模块；第一次打包会明显更慢。若脚本报告缺少 `AndroidTargetPlatform` DLL，则需要先运行 `SetupAndroid.bat`，并重新编译编辑器。

## 安装、启动与签名

项目已启用 `bPackageDataInsideApk=True`，新生成的 APK 会包含 Cooked Shader、地图和游戏资源。`-Install` 现在直接执行 `adb install -r -d -g`：覆盖安装 APK、保留应用数据、允许版本号回退并尝试授予运行时权限。脚本不会再调用 UE 生成的 `Install_*-arm64.bat`，因为该批处理文件会先卸载旧包，导致每次都变成全新安装。

`-Install` 和 `-Launch` 只支持 APK。AAB 不能直接通过 adb 安装，应上传到应用商店测试轨道；日常真机测试使用默认的 APK。第一次连接设备时仍需在手机上确认 USB 调试授权；如果系统开启了“通过 USB 安装/USB 安装确认”，该系统级确认也必须按手机要求完成，ADB 命令无法绕过它。`-Distribution` 需要先在 Unreal 的 Android 项目设置中配置 keystore、alias 和密码。

如果覆盖安装失败并出现 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`，说明新旧 APK 的签名不同，需要卸载旧包后再安装一次；这通常发生在切换 Debug/Development 与正式 Distribution 签名时。

运行前确认手机状态为 `device`，而不是 `offline` 或 `unauthorized`：

```bat
%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe devices -l
```

如果连接了多台已授权设备，必须传入对应序列号：

```powershell
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch -Device R5CTxxxxxxx
```
