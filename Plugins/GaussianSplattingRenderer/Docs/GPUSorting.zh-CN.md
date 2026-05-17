# GPU 排序

[English](GPUSorting.md) | [简体中文](GPUSorting.zh-CN.md)

本文档说明当前插件中的 GPU 排序实现，包括排序目标、排序数据结构、排序流程、设计依据与主要局限。

本文档不展开以下内容：

- 3D Gaussian 到 2D 椭圆的投影与光栅化
- 代理几何体投射阴影
- `.ply` 导入与压缩构建

## 1. 排序目标

当前渲染路径采用 back-to-front 的 alpha blending。该合成方式要求参与透明累积的 splat 按视角相关深度从远到近排列，否则最终颜色将与目标透明模型不一致。

在仅有单个 Gaussian 对象时，对象内部排序即可满足这一要求；但当前插件支持多个 Gaussian 对象同时参与同一视图下的透明合成，并要求 Gaussian 对象之间以及 Gaussian 对象与 UE Mesh 之间保持正确遮挡关系。因此，排序目标不是“单对象内部排序”，而是“对当前视图下所有参与绘制的 splat 进行统一的全局排序”。

## 2. 排序体系概述

当前排序路径由三部分组成：

1. 合并后的全局静态 splat 数据缓冲
2. 每帧基于当前相机构建的 GPU 剔除与深度 key 生成过程
3. 基于 UE 自带 GPU 排序接口完成的全局索引排序

渲染阶段并不重排完整的 splat 属性缓冲，而是仅生成一条“排序后的全局 splat 索引流”，后续顶点着色器再根据这条索引流回到全局属性缓冲中取数。

相关代码入口：

- [GaussianSplatViewExtension.cpp](../Source/GaussianSplatting/Private/GaussianSplatViewExtension.cpp)
- [GaussianSplatSorter.cpp](../Source/GaussianSplatting/Private/GaussianSplatSorter.cpp)
- [GaussianSplatCullAndSortKeyGen.usf](../Shaders/GaussianSplatCullAndSortKeyGen.usf)

## 3. 排序基础设施

当前插件并未从零实现一套完整的 GPU 排序器，而是复用了 UE 提供的 GPU 排序接口：

```cpp
SortGPUBuffers(...)
```

在这一结构下，插件侧负责：

- 准备排序所需的 key/value 缓冲
- 执行对象级和逐 splat 剔除
- 维护可见 splat 计数
- 生成最终 indirect draw args

UE 侧负责：

- 对 key/value 流执行真正的 GPU 排序

对应接口定义见 UE 源码中的 `GPUSort.h`，实现见 `GPUSort.cpp`。从 `GPUSort.cpp` 可见，该排序器采用的是 GPU **radix sort**，并显式定义了：

```cpp
#define RADIX_BITS 4
```

这里的 `RADIX_BITS = 4` 并不是说它只能排序 `4 bit`，而是说每一轮 pass 处理 `4 bit`。对一个 `uint32` key 而言，完整排序最多需要 `32 / 4 = 8` 轮 pass。

因此，当前插件使用的并不是一套来源不明的 GPU 排序器，而是 UE 自带的 GPU 基数排序实现。

## 4. 排序数据结构

### 4.1 全局 splat 索引流

静态缓冲合并完成后，每个 splat 在全局数据流中具有唯一的 `global splat index`。该索引用于访问：

- 全局位置缓冲
- 全局颜色缓冲
- 全局旋转缓冲
- 全局尺度缓冲
- 全局 normal 缓冲
- 全局 SH 缓冲

后续 GPU 排序的目标不是重排这些属性缓冲本身，而是重排这组全局 splat 索引。

### 4.2 排序 key

当前排序 key 来自 splat 中心在 view space 下的深度。其写入形式为：

$$
k_i = SortableUint(-z_i)
$$

其中：

- $z_i$ 为第 $i$ 个 splat 中心的 view-space 深度
- `SortableUint` 表示将浮点深度编码为可参与无符号整数排序的形式

对应 shader 代码见 [GaussianSplatCullAndSortKeyGen.usf](../Shaders/GaussianSplatCullAndSortKeyGen.usf)：

```hlsl
OutDepthKeys[globalSplatIndex] = FloatToSortableUint(-viewPos.z);
```

该设计直接服务于当前的 back-to-front 合成目标，即使用视图空间深度作为排序依据。

### 4.3 排序 value

当前排序 value 为全局 splat 索引本身，即一条 identity index 流：

$$
v_i = i,\quad i \in [0, N-1]
$$

对应实现见 [GaussianSplatSorter.cpp](../Source/GaussianSplatting/Private/GaussianSplatSorter.cpp) 中的 `EnsureIdentityIndexBuffer`。排序完成后，真正保留下来的结果不是 key，而是 value 的重排结果，也就是最终的：

- `SortedVisibleIndexBuffer`

该缓冲即为后续绘制阶段使用的全局绘制顺序。

## 5. key / value 设计依据

当前 key/value 设计遵循“最小排序输入”和“静态属性不重排”两条原则。

首先，当前排序仅服务于透明合成顺序，因此 key 只需要表达当前视图下的远近关系，不需要携带颜色、尺度、旋转或其他属性。将非深度属性混入 key 并不会提高透明排序正确性，只会增加排序输入复杂度。

其次，静态 splat 属性数据量远大于单个索引。若每帧直接重排整份属性缓冲，将显著增加带宽和实现复杂度。当前设计只对一条 `uint -> uint` 的 key/value 流排序，而把完整属性缓冲保持静态不动。排序完成后，顶点着色器根据排序后的全局 splat 索引回查静态缓冲。这种方式在代价和灵活性之间取得了较好的平衡。

最后，这一设计天然适配 UE 的 `SortGPUBuffers` 接口，因为该接口本身即面向 key/value 结构，而不是面向任意复杂结构体排序。

## 6. 剔除与 key 生成

排序之前，当前实现会先执行两层 GPU 剔除。

### 6.1 对象级剔除

第一层剔除对每个对象的局部包围盒执行保守的视锥测试，并写出 `ObjectVisibilityBuffer`。其作用是为后续逐 splat 阶段提供一个粗粒度早退条件。当整个对象都不可见时，该对象所属的所有 splat 都可以在后续阶段快速跳过。

### 6.2 逐 splat 剔除

第二层剔除在全局 splat 流上运行，对每个 splat 依次检查：

1. 对象级可见性
2. alpha 是否高于阈值
3. splat 中心是否位于相机前方
4. 若启用更细粒度剔除，是否满足 XY frustum slack 条件

若 splat 通过测试，则：

- 递增 `VisibleCount`
- 写入正常深度 key

若 splat 未通过测试，则：

- 将 key 写为

```hlsl
0xFFFFFFFFu
```

这一写法意味着被剔除的 splat 在排序结果中被推到尾部。之后 indirect draw 只绘制前 `VisibleCount` 个 splat，因此这些尾部元素不会真正参与光栅化。

## 7. GPU 排序过程

在剔除与 key 生成完成后，当前实现将以下缓冲交给 UE 的 `SortGPUBuffers`：

- 两组 ping-pong key buffer
- 两组 ping-pong value buffer
- 初始 identity value 流
- 最终输出 value buffer

其中：

- key 流表示当前视图下的排序依据
- value 流表示当前全局 splat 的身份

排序完成后，最终输出的是已按 key 重排的 value 流，也就是排序后的全局 splat 索引排列。该结果随后以 `SortedVisibleIndexBuffer` 的形式供绘制阶段使用。

## 8. 间接绘制参数生成

排序完成后，当前实现不会将可见数量读回 CPU，而是在 GPU 上直接根据 `VisibleCountBuffer[0]` 构造 indirect draw args。

由于每个 splat 的 billboard quad 由两个三角形构成，因此每个 splat 需要 6 个顶点。最终顶点数为：

$$
VertexCount = VisibleCount \times 6
$$

对应 shader 代码见 [GaussianSplatCullAndSortKeyGen.usf](../Shaders/GaussianSplatCullAndSortKeyGen.usf)：

```hlsl
OutDrawIndirectArgs[0] = visibleCount * 6u;
```

因此，最终 raster pass 绘制的是：

- 排序后索引流中的前 `VisibleCount` 个 splat

而不是整个全局 splat 集合。

## 9. 多对象透明遮挡的处理方式

当前实现并不对每个 Gaussian 对象分别排序、分别绘制，而是先把所有对象的 splat 合并到统一的全局缓冲，再对整个全局 splat 流排序，最后以单次 merged draw call 绘制。

这一点非常关键。若采用逐对象排序并逐对象绘制，则只能保证对象内部透明顺序正确，无法保证对象 A 的 splat 与对象 B 的 splat 在全局深度上正确交错，最终会导致对象间透明遮挡错误。

当前单次全局排序与单次合并绘制的设计，使得：

- 所有对象的 splat 共享同一套视图相关排序结果
- 不同对象的 splat 可以在统一深度序列中正确交错
- 绘制阶段只需一次 merged draw call

因此，这一方案同时解决了多对象透明遮挡与 draw call 控制两个问题。

## 10. 相关参数

与排序路径直接相关的全局参数包括：

- `r.GaussianSplat.CullMode`
  - `0`：关闭剔除
  - `1`：仅对象级剔除
  - `2`：对象级剔除加逐 splat XY frustum 剔除

- `r.GaussianSplat.SplatFrustumSlack`
  - 控制逐 splat 视锥测试的松弛范围

这些参数决定了：

- 哪些 splat 会进入可见前缀
- 排序阶段需要处理多少有效元素
- 剔除激进程度与边缘误剔除概率之间的折中

## 11. 小结

当前插件中的 GPU 排序可以概括为：

**对所有对象的 splat 建立统一的全局索引流；以 view-space 深度作为 key，以全局 splat 索引作为 value；复用 UE 自带的 GPU radix sort 完成统一排序；再以排序后的全局索引前缀驱动一次 merged indirect draw。**

这一路径的核心特征是：

- 排序对象是全局 splat 流，而不是单对象局部流
- key 与 value 明确分离
- 排序器复用 UE 自带 GPU 基数排序
- 最终结果同时服务于多对象透明遮挡和 draw call 压缩
