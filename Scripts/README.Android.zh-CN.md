# Android 打包与安装

在项目根目录打开终端。推荐使用 `.bat` 入口，它会仅对本次进程绕过 PowerShell Execution Policy：

```powershell
# Shipping APK
.\Scripts\Build-Android.bat

# Development APK，打包后安装并启动已连接的真机
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch

# 使用已在项目设置中配置的正式签名生成应用商店 AAB
.\Scripts\Build-Android.bat -PackageType AAB -Distribution

# 多台设备时指定 adb serial
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch -Device R5CTxxxxxxx
```

脚本默认查找 `D:\Software\Epic Games\UE_5.5`，也可以传入 `-EngineDir` 或设置 `UE_ENGINE_DIR`。默认输出目录为 `Packaged\Android-<Configuration>`。

首次运行前需配置 SDK/NDK/JDK，并确保 Android 真机开启 USB 调试。当前项目只打包 Vulkan SM5 arm64；普通 Mobile Vulkan ES3.1 后端仍在开发。

UE 5.5 要求 `android-34`、Build Tools `34.0.0`、CMake `3.22.1` 和 NDK `25.1.8937393`。只安装 Android Studio 最新版本（例如 Android 36/NDK 27）还不够。先在 SDK Manager 安装 **Android SDK Command-line Tools (latest)**，再运行：

```bat
"D:\Software\Epic Games\UE_5.5\Engine\Extras\Android\SetupAndroid.bat" android-34 34.0.0 3.22.1 25.1.8937393
```

`-Distribution` 要求先在 Unreal 的 Android 项目设置中配置 keystore、alias 和密码；日常真机测试不要传这个参数。

项目已启用 `bPackageDataInsideApk=True`，因此新生成的 APK 包含 cooked Shader、地图和游戏资源，可以直接安装运行。安装脚本仍会优先调用 UE 生成的 `Install_*-arm64.bat`，以兼容旧的独立 OBB 产物。运行前用下面的命令确认手机状态为 `device`，而不是空、`offline` 或 `unauthorized`：

```bat
%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe devices -l
```
