# 移动端支持

[English](Mobile.md) | [简体中文](Mobile.zh-CN.md)

本文说明 `GaussianSplattingRenderer` 的移动端支持范围、Android 构建流程、真机性能分析、调优选项和当前限制。已经验证的移动渲染路径是 Android arm64 上的 Vulkan SM5。

## 支持状态

| 平台 | 图形后端 | 状态 | 说明 |
| --- | --- | --- | --- |
| Android arm64 | Vulkan SM5 (`SF_VULKAN_SM5_ANDROID`) | 已验证 | UE 5.5 Cook、APK 打包和 Adreno 650 真机运行已通过 |
| Android arm64 | Vulkan ES3.1 | 未支持 | Mobile Vulkan 禁止当前顶点阶段 Buffer SRV 用法，需要独立后端 |
| Android arm64 | OpenGL ES3.1 | 未支持 | 项目未启用 |
| iOS | Metal MRT / SM5 | 实验性 | 尚未完成真机验证和性能调优 |

Android 路径保留逐 splat GPU 处理和透明混合。DeviceRadix 会压缩并只排序实际可见数量，但 Key 生成仍需遍历合并数据流。目前没有自适应 LOD、分块排序或 compute tile rasterizer，因此大场景在手机 GPU 上的成本仍明显高于桌面平台。

## 渲染管线

每次需要更新排序时，管线执行：

1. 对 Gaussian 对象做包围盒视锥剔除。
2. 遍历合并后的 splat 数据，逐点剔除并生成 32 位深度 Key。
3. 默认压缩可见 key/value，并由 DeviceRadix 按 GPU 实际可见数量排序。
4. 根据可见计数生成 indirect draw 参数。
5. 按排序索引绘制旋转椭圆并透明混合。
6. Integrate With UE 模式将累积纹理合成到 SceneColor；Direct 模式直接写入 SceneColor。

静止相机默认复用上一次排序。相机或投影矩阵变化后会重新排序。`SortMethod 1` 排序压缩后的可见前缀；`SortMethod 0` 是仍排序完整分配流的兼容路径。实验性 `SortMethod 2` 跳过排序，改用 Stochastic depth 与时域累积。

`GeometryMode 1` 请求 Mesh Shader + PS，但移动 RHI 不提供 Mesh Shader 时会自动回退 VS + PS。当前已验证的 Android 硬件应按 VS 路径理解。

## Android 配置

`Config/DefaultEngine.ini` 中的关键设置：

```ini
[/Script/AndroidRuntimeSettings.AndroidRuntimeSettings]
bBuildForArm64=True
bBuildForES31=False
bSupportsVulkan=False
bSupportsVulkanSM5=True
bPackageDataInsideApk=True
```

`bPackageDataInsideApk=True` 会把 cooked Shader、地图和资源放入 APK。旧的 APK + OBB 产物不能只安装 APK，否则可能报告全局 Shader 或 cooked content 缺失。

UE 5.5 默认不为 Adreno 6xx 选择 Vulkan SM5。项目在 `Config/DefaultDeviceProfiles.ini` 中对已验证设备启用：

```ini
[Android_Adreno6xx_Vulkan DeviceProfile]
+CVars=r.Android.DisableVulkanSM5Support=0
+CVars=r.Android.SupportsTimestampQueries=1
+CVars=r.GPUStatsEnabled=1
```

强制 SM5 前应确认目标驱动支持所需 Vulkan 功能。当前验证设备为 Adreno 650、Vulkan 1.1.128。

## 工具链与打包

UE 5.5 使用 Android API 34、Build Tools 34.0.0、CMake 3.22.1 和 NDK r25b (`25.1.8937393`)。完整说明见 [Android 打包与安装](../../../Scripts/README.Android.zh-CN.md)。

```powershell
# Development APK
.\Scripts\Build-Android.bat -Configuration Development

# 打包、安装并启动
.\Scripts\Build-Android.bat -Configuration Development -Install -Launch
```

输出位置：

```text
Packaged/Android-Development/UEGaussianSplatting-arm64.apk
```

adb 报告 `unauthorized` 时，在手机上接受 USB 调试授权。出现 `INSTALL_FAILED_ABORTED: User rejected permissions` 时，保持手机解锁并允许安装。

## 移动端默认项

Android Device Profile 默认关闭高成本或未验证功能：

```ini
r.RayTracing=0
r.Lumen.DiffuseIndirect.Allow=0
r.Lumen.Reflections.Allow=0
r.Shadow.Virtual.Enable=0
r.GenerateMeshDistanceFields=0
r.GaussianSplat.EnableHigherOrderSH=0
r.GaussianSplat.RasterMode=0
r.GaussianSplat.CullMode=2
r.GaussianSplat.EnableAntialiasing=0
r.GaussianSplat.OpacityAwareBounds=1
```

这些设置降低 Shader 和像素成本。渲染器默认使用 DeviceRadix 可见数量排序、`1.0` 像素阈值的屏幕尺寸剔除，并在 Mesh Shader 不可用时自动回退 VS。

## 性能分析

使用 Development 包执行：

```text
stat fps
stat unit
stat gpu
```

`stat gpu` 显示 GPU 时间（毫秒），不是显存。插件的主要阶段包括：

| GPU Stat | 含义 |
| --- | --- |
| Gaussian Splat | 整个 3DGS GPU 范围 |
| Gaussian Splat Object Cull | 对象级视锥剔除 |
| Gaussian Splat Sort Key Gen | 逐点剔除和深度 Key 生成 |
| Gaussian Splat GPU Sort | 全局 radix sort |
| Gaussian Splat Indirect Args | indirect draw 参数生成 |
| Gaussian Splat Direct | Direct 模式的投影、光栅化、像素计算和透明混合 |
| Gaussian Splat Accumulate | UE 集成模式的 Gaussian 累积 |
| Gaussian Splat Composite | 累积纹理合成到 SceneColor |

Android Vulkan GPU 计时必须在 RHI 初始化前启用：

```ini
r.Android.SupportsTimestampQueries=1
r.GPUStatsEnabled=1
```

`r.GPUStatsChildTimesIncluded=1` 可让父项包含子项。GPU timestamp 有额外开销，只建议用于 Development 性能测试。

为稳定观察排序项，可临时执行：

```text
r.GaussianSplat.ForceSortEveryFrame 1
```

测试后必须恢复：

```text
r.GaussianSplat.ForceSortEveryFrame 0
```

否则相机静止或画面没有 Gaussian 时也会持续全量排序，显著降低 FPS。

## 性能调优

在相同机位做 A/B 测试：

```text
r.ScreenPercentage 50
r.GaussianSplat.SortMethod 0
r.GaussianSplat.SortMethod 1
r.GaussianSplat.ScreenSizeCull 0
r.GaussianSplat.ScreenSizeCull 1
r.GaussianSplat.ScreenSizeCullMinPixels 1.0
r.GaussianSplat.OpacityAwareBounds 0
r.GaussianSplat.OpacityAwareBounds 1
r.GaussianSplat.CullMode 0
r.GaussianSplat.CullMode 2
```

Opacity-aware bounds 参考 `vksplat`：

```text
extent = sqrt(2 * log(opacity / alphaThreshold))
```

本项目把结果限制在原有 `sqrt(8)` sigma 上限内，因此只缩小低 opacity splat 的旋转椭圆包围四边形，不扩大高 opacity splat。它减少 `Gaussian Splat Direct` 的无效 fragment 和 overdraw，但不降低排序数量。

判断瓶颈：

- 降低 `r.ScreenPercentage` 后 Direct 明显下降：主要受 fragment、带宽或 overdraw 限制。
- 强制逐帧排序后 GPU Sort 明显上升：主要受全局排序限制。
- 静止相机明显快于移动相机：排序复用有效，移动重排成本较高。
- 画面无 Gaussian 但移动相机仍慢：瓶颈来自 Key 生成或其他逐帧工作；DeviceRadix 本身接收零可见数量，不再排序总 splat 流。

## 已知限制与后续方向

- Key 生成仍会覆盖合并后的 splat 数据流，但 DeviceRadix 只排序可见前缀。
- 屏幕尺寸剔除是阈值开关，还不是连续的屏幕贡献度 LOD 系统。
- 没有 tile binning 或 compute tile rasterizer，透明 overdraw 较高。
- 大型近景 splat 可能覆盖大量像素。
- Stochastic 渲染能够免排序，但需要全分辨率 depth/history；运动重投影还会增加一个 `PF_FloatRGBA` motion target，在移动端仍属实验性。
- Adreno 650 不支持 `R64_UINT`，当前保持 32 位 Key/索引路径。
- Vulkan timestamp 在部分旧驱动上可能不稳定。

后续重点是减少 Key 生成工作量、自适应屏幕空间 LOD、分块/compute rasterization，以及验证 Stochastic history 在移动端的带宽与显存成本。

## 排错

### 启动报告 cooked content 缺失

确认使用最新 APK，并启用 `bPackageDataInsideApk=True`。

### 真机回退 Vulkan ES3.1

若日志出现 `VULKAN_ES3_1_ANDROID`，检查 Device Profile 是否将 `r.Android.DisableVulkanSM5Support` 设为 1。正确日志应包含：

```text
Vulkan SM5 RHI will be used!
Vulkan RHI ShaderPlatform for SM5: VULKAN_SM5_ANDROID
```

### `stat gpu` 没有数据

确认使用 Development 包，并在启动前通过 Device Profile 设置 `r.Android.SupportsTimestampQueries=1`。运行后再设置通常太晚。
