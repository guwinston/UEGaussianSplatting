# GPU 排序

[English](GPUSorting.md) | [简体中文](GPUSorting.zh-CN.md)

本文说明 `GaussianSplattingRenderer` 当前使用的排序路径。渲染器把所有可见 Gaussian 对象合并为一条全局 splat 数据流，从而保证透明排序能够跨对象正确工作。

## 1. 为什么需要排序

常规渲染路径使用 source-alpha blending，并按从远到近的顺序绘制 splat。仅在每个 Actor 内部分别排序并不充分：两个相互重叠的 Actor 中的 splat 必须进入同一套视角相关顺序。

实验性 Stochastic 路径是例外。它用概率覆盖、独立 depth target 和时域重建替代有序 alpha blending。

## 2. 每帧流程

有序路径需要更新排序时，GPU 执行：

1. 对象级可见性测试；
2. 逐 splat 剔除并生成 32 位深度 Key；
3. DeviceRadix 模式下压缩可见 key/value；
4. GPU 排序；
5. 根据 GPU 可见数量生成 indirect 参数；
6. 按排序结果通过 VS 或 Mesh Shader 光栅化。

静止视图通常复用上一次结果。`r.GaussianSplat.ForceSortEveryFrame 1` 仅应在测量排序成本时临时开启，之后恢复为 `0`。

## 3. 排序方式

`r.GaussianSplat.SortMethod` 选择路径：

| 值 | 路径 | 输入数量 | 用途 |
| --- | --- | --- | --- |
| `0` | UE `SortGPUBuffers` | 分配的 splat 总数量 | 兼容与对照 |
| `1` | DeviceRadix | GPU 实际可见数量 | 默认高性能有序路径 |
| `2` | Stochastic 免排序 | 不做深度排序 | 实验性画质/性能取舍 |

如果当前 RHI 不支持 DeviceRadix，方式 `1` 会回退到 UE 内置 sorter。

### 3.1 UE 内置排序

兼容路径为每个已分配 splat 写入一个 key/value。被剔除项使用 sentinel key 并移动到尾部，但 `SortGPUBuffers` 仍会处理 `TotalSplatCount`。它适合作为正确性基线，但剔除不会降低 radix 工作量。

该路径不修改任何引擎源码；插件只调用 UE 已提供的公开渲染基础设施。

### 3.2 DeviceRadix 可见数量排序

默认路径通过原子计数把可见 key/value 压缩到 `[0, VisibleCount)` 前缀。可见数量始终保留在 GPU，并作为 active element count 传给 DeviceRadix，不发生 CPU readback 或帧阻塞。

缓冲容量仍按 splat 总数量分配，但 radix histogram、prefix 和 scatter 只处理实际可见前缀。因此对象剔除、逐 splat 视锥剔除和屏幕尺寸剔除都能直接降低排序工作量。

DeviceRadix 使用可配置的 8-bit LSD pass：

```text
r.GaussianSplat.DeviceRadixPasses 4
r.GaussianSplat.DeviceRadixWriteFinalKeys 0
```

Pass 数量限制在 `1..4`。四个 pass 排序完整 32 位 Key；减少 pass 时从 `32 - PassCount * 8` 位开始，用更低成本换取更低深度顺序精度。默认不输出最终排序 Key，因为光栅化只消费排序后的 value/索引。

### 3.3 Stochastic 免排序

方式 `2` 把可见 ID 直接压缩到最终索引缓冲，跳过全局深度排序。光栅化阶段通过随机接受和 depth 解决可见性，再做时域累积。详见 [Stochastic 随机渲染](StochasticRendering.zh-CN.md)。

## 4. Key 与 Value 布局

每个有序项包含：

- 一个 32 位可排序深度 Key；
- 一个 32 位全局 splat 索引。

索引指向合并后的属性缓冲，因此一条排序流可以同时包含所有可见 Gaussian 对象。Key 变换保持 alpha blending 所需的从远到近顺序。

## 5. 剔除与压缩

Key 生成 compute pass 可以执行：

- 对象级视锥剔除；
- 逐 splat XY 视锥剔除；
- 可调视锥松弛系数；
- 投影屏幕尺寸剔除。

主要控制项：

```text
r.GaussianSplat.CullMode 2
r.GaussianSplat.SplatFrustumSlack 1.3
r.GaussianSplat.ScreenSizeCull 1
r.GaussianSplat.ScreenSizeCullMinPixels 1.0
```

DeviceRadix 模式下，被拒绝的 splat 不会进入 active prefix。UE 内置 sorter 中它们仍是已分配的 sentinel 项。Stochastic 模式则把接受的可见 ID 直接压缩到绘制索引缓冲。

`r.GaussianSplat.OpacityAwareBounds` 与剔除相关但作用更晚：它缩小低 opacity splat 的光栅支持范围，减少 fragment overdraw，而不减少排序数量。

## 6. Indirect 参数

GPU 可见数量直接生成两种绘制参数，不需要 CPU 同步：

- VS + PS 使用的 indirect indexed-draw 参数；
- Mesh Shader + PS 使用的 indirect mesh-dispatch 参数。

因此剔除、排序和 draw submission 始终由 GPU 驱动。几何路径选择详见 [渲染路径与性能](RenderingPaths.zh-CN.md)。

## 7. 性能建议

常规有序渲染优先使用 DeviceRadix（`SortMethod 1`）。UE 内置 sorter 仅用于兼容或 A/B 验证。减少 DeviceRadix pass 前，应在大量半透明交叠的困难视角检查画质。

性能测试时固定相机、分辨率、RenderMode、剔除设置和 GeometryMode，并分别测量 Key 生成与排序 scope。降低可见数量会直接帮助 DeviceRadix；但如果场景受 fragment 限制，即使排序更快，总帧时间变化也可能很小。

## 8. 小结

- 排序跨所有 Gaussian 对象全局执行。
- DeviceRadix 是默认路径，并且只排序 GPU 生成的实际可见数量。
- UE 内置路径排序完整分配流，保留为兼容基线。
- Stochastic 模式跳过全局深度排序，依赖时域重建。
- Active count 和 indirect 参数都不需要 CPU readback。
- 整套实现不需要修改 Unreal Engine 源码。
