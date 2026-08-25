# UEGaussianSplatting

[English](README.md) | [简体中文](README.zh-CN.md)

这是一个面向 Unreal Engine 5.5 的 `GaussianSplattingRenderer` 插件示例项目。

这个项目展示了一套在 UE 中使用 3D Gaussian Splatting 的基础流程：导入 3DGS `.ply` 文件，生成 Gaussian Splat Asset，将资产放入关卡，通过自定义 Gaussian renderer 渲染，并可选择导入 `cameras.json` 来对齐训练相机或批量截图。

![快速开始](Plugins/GaussianSplattingRenderer/Assets/get_start.gif)

## 功能特性

- 可直接打开的 UE 5.5 示例项目
- 已集成 `GaussianSplattingRenderer` 插件
- 支持导入和渲染 3DGS 风格 `.ply` 文件
- 使用运行时压缩 Gaussian 数据，降低显存带宽压力
- 支持按 GPU 实际可见数量执行 DeviceRadix 排序，radix pass 不再处理已剔除 splat
- 可选择 VS + PS 或 Mesh Shader + PS 几何路径，不支持 Mesh Shader 时自动回退 VS
- 支持屏幕尺寸剔除与 opacity-aware 光栅包围，降低几何和 fragment 成本
- 提供实验性的免排序 Stochastic Splat、时域累积与相机运动重投影路径
- GPU 侧全局 splat 排序，用于正确的半透明合成
- 支持最高 degree 3 的 higher-order SH
- 支持导入 `cameras.json`，用于视口对齐和批量截图
- 支持通过代理几何体进行编辑器选择、碰撞查询和投射阴影
- 提供 Demo 关卡，方便快速检查渲染效果

## 环境要求

- Unreal Engine 5.5
- Windows 编辑器 / 运行环境
- Visual Studio 2022 和 C++ 编译工具链

## 快速开始

1. 使用 Unreal Engine 5.5 打开 `UEGaussianSplatting.uproject`。
2. 如果 Unreal 提示 rebuild modules，允许它编译。
3. 打开 `Content/Demo.umap`。
4. 将 3DGS `.ply` 文件拖入 Content Browser，生成 Gaussian Splat Asset。
5. 将导入后的资产拖入关卡。
6. 选中生成的 `GaussianSplatActor`，根据需要调整：
   - `SplatScale`
   - `MaxSHDegree`
   - `AlphaCullThreshold`
7. 在编辑器视口中查看结果，或导入 `cameras.json` 进行相机对齐和图像导出。

## 项目结构

```text
Config/                              项目配置
Content/                             Demo 关卡和可选示例资产
Source/                              最小项目 C++ 模块
Plugins/GaussianSplattingRenderer/   Gaussian splatting 渲染插件
UEGaussianSplatting.uproject          Unreal 项目文件
```

## 插件文档

插件的详细使用说明、渲染原理、GPU 排序和压缩方案请阅读：


- [中文插件文档](Plugins/GaussianSplattingRenderer/Docs/README.zh-CN.md)
- [渲染原理](Plugins/GaussianSplattingRenderer/Docs/RenderingPrinciples.zh-CN.md)
- [GPU 排序](Plugins/GaussianSplattingRenderer/Docs/GPUSorting.zh-CN.md)
- [渲染路径与性能](Plugins/GaussianSplattingRenderer/Docs/RenderingPaths.zh-CN.md)
- [Stochastic 随机渲染](Plugins/GaussianSplattingRenderer/Docs/StochasticRendering.zh-CN.md)
- [模型压缩](Plugins/GaussianSplattingRenderer/Docs/Compression.zh-CN.md)
- [移动端支持](Plugins/GaussianSplattingRenderer/Docs/Mobile.zh-CN.md)
