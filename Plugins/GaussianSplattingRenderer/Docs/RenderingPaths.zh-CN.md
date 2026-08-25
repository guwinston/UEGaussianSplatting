# 渲染路径与性能

[English](RenderingPaths.md) | [简体中文](RenderingPaths.zh-CN.md)

当前渲染器有两个相互独立的选择：如何排列可见 splat，以及如何为每个可见 splat 生成四边形。当前实现中没有 compute 生成几何体的路径。

## 路径选择

| 控制项 | 值 | 路径 | 说明 |
| --- | --- | --- | --- |
| `r.GaussianSplat.SortMethod` | `0` | UE 内置排序 | 兼容/对照路径；排序分配的完整数据流 |
|  | `1` | DeviceRadix | 默认；压缩并仅排序 GPU 实际可见数量 |
|  | `2` | Stochastic | 实验性；压缩可见 ID 并跳过深度排序 |
| `r.GaussianSplat.GeometryMode` | `0` | VS + PS | 兼容性最广 |
|  | `1` | Mesh Shader + PS | 默认请求；不支持 Mesh Shader 时自动回退 VS |

两个控制项可以自由组合。例如 DeviceRadix + Mesh Shader 是桌面端常用高性能路径，而不支持 Mesh Shader 的 Vulkan SM5 移动设备预计使用 DeviceRadix + VS。

## Vertex Shader 路径

VS 路径为每个可见 splat 间接绘制一个带索引四边形。每个顶点读取 splat ID，加载压缩属性，将三维协方差投影为二维椭圆并输出一个 quad corner。它实现简单且平台兼容性好，但四个顶点之间可能重复部分逐 splat 工作。

显式选择：

```text
r.GaussianSplat.GeometryMode 0
```

## Mesh Shader 路径

Mesh 路径通过 indirect dispatch 让 Mesh Shader workgroup 生成 splat primitive。它能更有效地共享逐 splat 计算，并绕过传统 vertex/index 管线的部分固定流程。实际收益取决于 GPU 架构、驱动、splat 数量、屏幕覆盖率，以及帧是否受排序或 fragment overdraw 限制。

请求 Mesh Shader：

```text
r.GaussianSplat.GeometryMode 1
```

该值只是请求，并不保证最终使用 Mesh Shader。渲染器会检查 RHI 支持，不可用时自动使用 VS。即使运行 Vulkan SM5，大多数当前移动端目标也应按仅支持 VS 处理。

## 排序与光栅化之前的剔除

Key 生成 pass 可以组合：

- 对象级视锥剔除；
- 逐 splat XY 视锥剔除；
- 对亚像素 splat 的屏幕尺寸剔除；
- 缩小低 opacity 椭圆四边形的 opacity-aware 光栅包围。

相关控制项：

```text
r.GaussianSplat.CullMode 2
r.GaussianSplat.SplatFrustumSlack 1.3
r.GaussianSplat.ScreenSizeCull 1
r.GaussianSplat.ScreenSizeCullMinPixels 1.0
r.GaussianSplat.OpacityAwareBounds 1
```

屏幕尺寸剔除会在 DeviceRadix 排序和 indirect rendering 前降低可见数量。Opacity-aware bounds 主要减少光栅化 fragment 与 overdraw，不会让原本可见的 splat 退出排序。

## 推荐配置

桌面有序路径：

```text
r.GaussianSplat.SortMethod 1
r.GaussianSplat.GeometryMode 1
r.GaussianSplat.DeviceRadixPasses 4
r.GaussianSplat.DeviceRadixWriteFinalKeys 0
```

兼容/对照路径：

```text
r.GaussianSplat.SortMethod 0
r.GaussianSplat.GeometryMode 0
```

实验性 Stochastic 路径：

```text
r.GaussianSplat.SortMethod 2
r.GaussianSplat.GeometryMode 1
r.GaussianSplat.StochasticTemporalSamples 1000
r.GaussianSplat.StochasticReprojection 1
r.GaussianSplat.StochasticMotionSamples 8
```

## 正确进行性能对比

对比路径时应固定相机、分辨率、场景、RenderMode 和预热状态，并记录 `stat unit` 与 `stat gpu`，不要只比较 FPS。相机静止时通常会复用排序结果，因此静态机位可能掩盖排序成本。

测试有序模式时可临时强制逐帧排序：

```text
r.GaussianSplat.ForceSortEveryFrame 1
```

测试后恢复：

```text
r.GaussianSplat.ForceSortEveryFrame 0
```

需要按阶段解读结果：如果瓶颈是 pixel overdraw，更快的 Mesh Shader 不会显著改变总帧时间；复用排序结果时，更快的 sorter 也不会带来明显收益。减少 DeviceRadix pass 或提高屏幕尺寸阈值时，必须同时检查画质等价性。
