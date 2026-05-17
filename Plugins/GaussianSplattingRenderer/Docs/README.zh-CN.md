# Unreal Gaussian Splatting Renderer

[English](../README.md) | [简体中文](README.zh-CN.md)

这是一个面向 Unreal Engine 5.5 / 5.7 的 3D Gaussian Splatting 插件，支持 3DGS 场景的导入、压缩、高性能渲染、检查与图像导出。

目前 UE 中的 3DGS 渲染实现大致可以分为三类：基于 Niagara 粒子系统的方案、基于自定义渲染管线的方案，以及基于 CUDA 后端的方案。本插件采用自定义渲染管线方案，目标是在保持 UE 常规使用方式的同时，为大规模 Gaussian Splatting 场景提供更高性能的渲染路径。

整个工作流围绕 UE 的实际使用习惯设计：将 3DGS 的 `.ply` 文件拖入 Content Browser 后自动生成 3DGS 资产，再将该资产拖入关卡中自动生成 3DGS Actor 进行渲染。之后可以继续调整渲染参数，也可以通过 `cameras.json` 对齐编辑器视口，并导出对应相机图像用于对比或数据集检查。

在 RTX 5060 Ti 上以 1080P 分辨率测试时，小物体几十万高斯点实测可保持 `100 FPS` 以上，几百万高斯点实测可达到 `80 FPS` 以上，对于几千万高斯点模型实测仍可保持 `40 FPS` 以上的交互式渲染性能。

## 功能特性

- 支持 3DGS 风格 `.ply` 文件的导入与实时渲染
- 支持 `.ply` 数据压缩，降低模型存储占用，并减轻渲染时的 `memory-bound` 压力
- 支持 GPU 侧 splat 排序，适配大规模高斯点场景
- 支持最高 3 阶高阶球谐系数，实现视角相关颜色表现
- 支持导入 `cameras.json`，并将编辑器视口对齐到导入相机
- 支持保存当前视口图像，并批量渲染 `cameras.json` 中的全部相机视角
- 支持通过代理几何体近似投射阴影
- 能较好处理不同高斯物体之间，以及高斯物体与 UE Mesh 之间的遮挡关系
- 提供可选的 Actor 级碰撞代理开关

## 环境要求

- Unreal Engine 5.5 - 5.7
- Windows 编辑器 / 运行环境
- Visual Studio 2022
- 输入模型需为 3DGS 风格 `.ply` 格式
- 如需相机对齐与批量截图，可额外提供原始 3DGS `cameras.json`

## 插件目录结构

```text
GaussianSplattingRenderer/
  Source/GaussianSplatting/          Runtime 模块
  Source/GaussianSplattingEditor/    编辑器导入器与 Details 自定义
  Shaders/                           Gaussian splat 渲染着色器
  GaussianSplattingRenderer.uplugin
```

## 启用插件

1. 将 `GaussianSplattingRenderer` 复制到项目的 `Plugins/` 目录下。
2. 使用 Unreal Editor 打开项目。
3. 首次使用时，请在 `Edit > Plugins` 中确认 `GaussianSplattingRenderer` 已启用。
4. 如果编辑器提示重启，则重启编辑器。
5. 如果插件尚未编译，编译项目的 `Editor` 目标。

当前仓库已经将该插件包含在项目内，因此直接打开 `.uproject` 并编译编辑器目标即可。

## 快速开始

![快速开始](../Assets/get_start.gif)

1. 使用 Unreal Editor 打开项目。
2. 在 `Edit > Plugins` 中确认 `GaussianSplattingRenderer` 已启用。
3. 将 `.ply` 文件拖入 Content Browser，或使用 `Import` 导入。
4. 导入器会创建一个 `Gaussian Splat Asset`。
5. 将导入后的资产拖入关卡。
6. 选中自动生成的 `GaussianSplatActor`。
7. 调整主要组件参数：
   - `SplatScale`
   - `MaxSHDegree`
   - `AlphaCullThreshold`
8. 如有需要，可通过控制台变量调整全局渲染模式、Raster 模式、Cull 模式等全局行为。
9. 在编辑器视口或运行时查看结果。

## 运行时加载

可以通过以下 BlueprintCallable 接口在运行时加载 `.ply`：

```cpp
AGaussianSplatActor::LoadFromFile(const FString& FilePath)
UGaussianSplatComponent::LoadFromFile(const FString& FilePath)
```

传入路径需要是目标机器上可访问的绝对路径。

## Camera JSON 工作流

插件支持导入原始 3DGS 的 `cameras.json`，并使用这些相机来对齐编辑器视口。

1. 将 `cameras.json` 导入到 Content Browser。
2. 选中 `GaussianSplatActor`。
3. 在 `ImportedCameraSet` 上指定导入后的相机资产。
4. 在 `SelectedImportedCamera` 中选择一个相机。
5. 使用 Actor Details 面板中的按钮：
   - `Snap Active Viewport To Selected Camera`
   - `Save Current Viewport Image`
   - `Render All Imported Camera Images`

批量渲染时会弹出编辑器进度框，并支持取消。

### 导出目录

PNG 图像默认保存到：

```text
Saved/GaussianSplatCameraRenders
```

也可以通过 Actor 上的 `CameraRenderOutputDirectory` 自定义输出目录。

当前截图逻辑基于活动的编辑器视口，因此输出分辨率等于当前视口分辨率，还不会强制使用 `cameras.json` 中原始的 `width` 和 `height`。

## 压缩方案概览

插件会在导入阶段构建渲染所需的压缩数据。

当前压缩流程主要包括：

- 对 `position`、`rotation`、`opacity`、`color`、`scale`、`SH` 等属性进行打包
- 使用 Morton 排序提升空间局部性
- 对 `scale` 使用 SPZ 风格固定 log-scale 量化压缩
- 对 `SH` 使用直接量化与 `bit packing`
- 在 Unreal 日志中输出压缩耗时与误差统计

当前方案相对于原始模型的压缩倍率约为 `3.5x`。如果采用 SPZ 或 SOG 这类更激进的方案，理论上可以获得更高的压缩倍率；同时由于进一步减轻了大场景下的 `memory-bound` 压力，渲染速度通常也会更有优势。

当前实现没有整体采用这些方案，主要原因有两点：SPZ 的高压缩率在很大程度上依赖 `gzip` 一类通用压缩流程，而 SOG 的核心压缩路径依赖 `k-means` 聚类。对于数百万到上千万高斯点的模型，尤其是 SH 这类高维属性，`k-means` 聚类构建速度非常慢，导入阶段成本过高。当前 scale 路径有意采用 SPZ 风格固定 log-scale 字节量化；需要注意的是，这也意味着 scale 存在固定上下界，如果资产中存在超过该范围的高斯，它们会被 clamp 到最近端点。

因此，当前版本选择了导入时间更稳定、实现更直接的压缩路径，用一部分压缩率上的让步换取更可控的构建时间。如果后续接受更长的离线构建时间，SPZ 或 SOG 路线仍然可以进一步提升压缩倍率，并在大场景中带来更好的带宽表现与渲染性能。

## 重要参数说明

`AGaussianSplatActor` 上的重要参数：

- `SourcePlyPath`：直接从磁盘加载 `.ply` 时使用的绝对路径
- `ImportedCameraSet`：导入后的 `cameras.json` 相机资产
- `SelectedImportedCamera`：用于视口对齐的当前相机
- `bApplySelectedCameraFOVToEditorViewport`：对齐相机时是否同步应用导入相机的水平 FOV
- `CameraRenderOutputDirectory`：PNG 截图输出目录
- `bEnableCollisionProxy`：是否启用由选择体 / 代理体生成的真实碰撞

`bEnableCollisionProxy` 默认关闭，这样 Gaussian splat 场景不会像一整块隐形网格那样挡住玩家。只有在确实需要碰撞查询或阻挡效果时才建议开启。

`UGaussianSplatComponent` 上的重要参数：

- `GaussianSplatAsset`：导入后的 splat 资产引用
- `SplatScale`：控制 splat 的投影尺寸
- `MaxSHDegree`：限制视角相关 SH 的最高阶数
- `AlphaCullThreshold`：剔除过低透明度的 splat

## 控制台变量

当前可用的主要运行时开关：

```text
r.GaussianSplat.RasterMode
r.GaussianSplat.RenderMode
r.GaussianSplat.EnableHigherOrderSH
r.GaussianSplat.EnableAntialiasing
r.GaussianSplat.CullMode
r.GaussianSplat.SplatFrustumSlack
```

- `r.GaussianSplat.RasterMode`
  `0` 为 unit-circle 像素评估，`1` 为 conic / CUDA-like 像素评估。
- `r.GaussianSplat.RenderMode`
  `0` 为通过中间累积纹理在 tonemap 前合成，`1` 为在 tonemap 后直接混合并尽量匹配原始 3DGS 渲染结果。
- `r.GaussianSplat.EnableHigherOrderSH`
  设为 `0` 可以关闭高阶 SH，只保留常量颜色，便于调试性能与观察视觉差异。
- `r.GaussianSplat.EnableAntialiasing`
  启用 Mip-Splatting 风格的 opacity compensation，用来配合 2D covariance 的 low-pass filter，减少小 splat 或远处 splat 的锯齿与跳变。
- `r.GaussianSplat.CullMode`
  `0` 为关闭剔除，`1` 为仅物体级剔除，`2` 为物体级剔除加逐 splat XY 视锥剔除。
- `r.GaussianSplat.SplatFrustumSlack`
  用于调节逐 splat 视锥剔除的松弛系数，较大的值可以减少边缘 splat 被误剔除的概率。

## 选择体与碰撞代理

插件会为高斯模型保留一个生成的选择体 / 代理体，使得半透明的 Gaussian splat 在编辑器中仍然可以被可靠选中和检查；在配置了 `ShadowProxyMesh` 的情况下，这个代理体还可以用于近似投射阴影。

这个代理体可能来自：

- 显式指定的 `ShadowProxyMesh`
- 或 Gaussian 资产内部生成的 selection hull 数据

默认情况下，这个代理体不会参与真实游戏碰撞。如果希望它参与阻挡或碰撞查询，可以在 Actor 上开启 `bEnableCollisionProxy`。如果为资产指定了 `ShadowProxyMesh`，该代理网格还可以作为高斯模型的近似阴影投射体使用。

## 数据流

插件当前的主要数据流可以概括为：

1. 将原始 `.ply` 导入 Content Browser，构建 `UGaussianSplatAsset`，并在导入阶段生成压缩后的高斯属性数据。
2. 将生成的资产拖入关卡后，创建 `AGaussianSplatActor` 与对应的 `UGaussianSplatComponent`，运行时从资产中读取压缩数据。
3. 在渲染阶段，将多个 Gaussian 对象的压缩数据合并为全局渲染所需的缓冲布局，并上传到 GPU。
4. 在 `SceneViewExtension` 中结合当前视图、全局 CVar、对象状态与可见性信息，准备本帧渲染参数与相关 GPU 资源。
5. 通过 GPU 排序流程生成当前视角下的 splat 排序结果，并构建后续绘制所需的索引与间接参数。
6. 最终进入 Gaussian splat 渲染阶段，与 UE 场景中的其他 Gaussian 对象和常规 Mesh 一起完成遮挡、排序与最终成像。

## 详细文档

如果希望进一步了解当前渲染路径的数学原理和 shader 实现细节，可以继续阅读：

- [渲染原理](RenderingPrinciples.zh-CN.md)
- [GPU 排序](GPUSorting.zh-CN.md)
- [模型压缩](Compression.zh-CN.md)

其中包括：

- 3DGS 到 UE 的坐标系转换
- 3D Gaussian 到 2D 椭圆投影
- VS / PS 的职责划分
- `RASTER_MODE=0/1` 的等价求值方式
- alpha blending 与排序方向
- Gaussian 与 Gaussian、Gaussian 与 UE Mesh 的遮挡关系
- 当前可选的代理投射阴影路径
- 当前合并全局 GPU 排序路径，包括 key/value 布局与间接绘制参数生成
- 当前运行时压缩数据格式、各属性压缩方式，以及与 SOG、SPZ 的取舍差异

## 参考项目

- [3D Gaussian Splatting 官方仓库](https://github.com/graphdeco-inria/gaussian-splatting)
- [SuperSplat](https://github.com/playcanvas/supersplat)
- [SPZ](https://github.com/nianticlabs/spz)
- [vk_gaussian_splatting](https://github.com/nvpro-samples/vk_gaussian_splatting)

## License / Distribution

本插件采用 MIT License 发布，详细条款见插件目录下的 `LICENSE` 文件。
