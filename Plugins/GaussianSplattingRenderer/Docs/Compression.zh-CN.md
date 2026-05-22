# 模型压缩

[English](Compression.md) | [简体中文](Compression.zh-CN.md)

## 1. 文档范围

本文只讨论当前插件中 `Gaussian Splat Asset` 的压缩构建与运行时压缩数据格式，重点说明：

- 为什么需要模型压缩
- 当前压缩格式的整体结构
- 各个高斯属性分别如何压缩
- 当前方案能达到的压缩倍数
- 当前方案与 SOG、SPZ 的主要差异
- 为什么有些属性没有沿用 SOG 或 SPZ 的压缩方式

## 2. 压缩目标

3DGS 原始 `.ply` 数据的问题并不只是磁盘体积大，更重要的是它在运行时通常会明显受到 `memory-bound` 限制。对几百万到几千万高斯点的场景而言，如果仍然按浮点格式直接保留 `position`、`rotation`、`scale`、`opacity`、`SH` 等属性，那么：

- 资产序列化体积大
- 导入后的 CPU 常驻内存大
- 上传到 GPU 的静态缓冲大
- 顶点着色阶段的解码带宽压力大
- 大场景下渲染速度更容易受显存带宽限制

因此，当前压缩方案的目标并不是追求极限离线压缩率，而是同时兼顾以下几点：

1. 运行时仍然可以高效随机访问每个 splat 的属性  
2. 导入阶段构建时间可控，不依赖极慢的高维聚类  
3. GPU 侧解码逻辑尽量直接，便于批量合并后渲染  
4. 在压缩率、误差和导入耗时之间取得稳定平衡

## 3. 当前压缩格式概览

当前运行时压缩数据的核心结构定义在 [GaussianSplatAsset.h](../Source/GaussianSplatting/Public/GaussianSplatAsset.h) 的 `FGaussianSplatCompressedData` 中。其核心流包括：

- `PackedPositions`
- `PackedColors`
- `PackedRotations`
- `PackedScales`
- `PackedNormals`
- `PackedSHData`
- `SHCodebook`
- `ChunkPositionMins / ChunkPositionMaxs`

整体思路可以概括为：

1. 导入 `.ply` 后先得到浮点形式的 `FGaussianSplatData`
2. 对全部 splat 做 Morton 排序，提高空间局部性
3. 按固定 chunk 大小划分，并记录每个 chunk 的局部包围盒
4. 将 `position`、`color`、`opacity`、`rotation`、`scale`、估计法线、`higher-order SH` 分别压缩到更紧凑的流
5. 运行时将多个对象的这些压缩流合并后上传到 GPU，shader 再按需解码

这意味着当前方案本质上是一种面向运行时随机访问和批量渲染的压缩数据格式，而不是面向离线归档的通用二进制压缩包。

## 4. 压缩前的数据基准

当前代码中，压缩倍率并不是拿 `.zip` 或 `gzip` 后的文件体积做比较，而是和一份 `Inria` 风格 3DGS `PLY` 的原始浮点属性数据做比较。对应实现见 [GaussianSplatAsset.cpp](../Source/GaussianSplatting/Private/GaussianSplatAsset.cpp) 中的：

- `EstimateInriaPlyFloatPayloadBytes`
- `EstimateCompressedRenderPayloadBytes`
- `LogCompressionStats`

其中原始浮点数据按以下属性估算：

- `xyz`
- 未使用的 `normal`
- `f_dc`
- `f_rest`
- `opacity`
- `scale`
- `rotation`

因此，当前文档中的压缩倍率应理解为：

> 相对于原始 3DGS 浮点属性数据的压缩倍率

而不是：

- 相对于经过 `gzip` 的归档体积
- 相对于特定外部格式的最终磁盘文件体积

## 5. 当前压缩倍率

按照当前代码的统计方式，当前方案相对于原始 3DGS 浮点属性负载，大致可以达到约 `3.5x` 的压缩倍率。

这个数值对应的是当前可直接渲染的压缩数据本身，不额外叠加 `gzip` 一类的通用压缩，同时保留运行时随机访问和 GPU 直接解码能力。

因此，它反映的是当前运行时压缩格式相对于原始 3DGS 浮点数据的压缩效果，而不是发布场景下尽量缩小后的最终文件体积。

## 6. 各属性的压缩方式

### 6.1 Position

`position` 的压缩不是直接对全局坐标做统一量化，而是采用了：

1. Morton 排序  
2. 按固定 `ChunkSize = 256` 划分  
3. 对每个 chunk 单独记录 `ChunkPositionMins / ChunkPositionMaxs`  
4. 在 chunk 局部范围内，将每个轴量化为 `uint16`

对应流为：

- `PackedPositions`：每个 splat 三个 `uint16`
- `ChunkPositionMins`
- `ChunkPositionMaxs`

对应实现见 [GaussianSplatAsset.cpp](../Source/GaussianSplatting/Private/GaussianSplatAsset.cpp) 中的 `BuildMortonSortedIndices` 和 `BuildCompressedDataFromRuntimeData`。

量化公式可以写为：

$$
u = round\left(65535 * \frac{p - p_{\min}^{(c)}}{p_{\max}^{(c)} - p_{\min}^{(c)}}\right)
$$

其中：

- $p$ 是原始 position
- $p_{\min}^{(c)}, p_{\max}^{(c)}$ 是当前 chunk 的局部包围盒
- $u$ 是三个轴各自的 `uint16` 量化结果

解码则为：

$$
\hat{p} = p_{\min}^{(c)} + \frac{u}{65535}\left(p_{\max}^{(c)} - p_{\min}^{(c)}\right)
$$

因此，单轴量化步长为：

$$
\Delta_x^{(c)} = \frac{p_{\max}^{(c)}.x - p_{\min}^{(c)}.x}{65535}
$$

对应的单轴最大绝对误差上界为：

$$
|e_x^{(c)}| \le \frac{1}{2}\Delta_x^{(c)}
$$

其余两个轴同理。Morton 排序的作用，就是让每个 chunk 的局部范围 $p_{\max}^{(c)} - p_{\min}^{(c)}$ 尽可能小，从而直接减小 position 的量化步长和绝对误差。

如果和 SOG 的 `position` 压缩相比，SOG 采用的是先做带符号对数变换，再在全局范围内做 `16-bit` 量化。其一维变换可写为：

$$
q(x)=sign(x)\log(|x|+1)
$$

然后对 $q(x)$ 做全局线性量化：

$$
u = round\left(65535 * \frac{q(x)-q_{\min}}{q_{\max}-q_{\min}}\right)
$$

解码时再做反变换：

$$
\hat{x} = sign(\hat{q})\left(e^{|\hat{q}|}-1\right)
$$

其中 $\hat{q}$ 是量化后的对数坐标。若记对数空间的量化步长为

$$
\Delta_q = \frac{q_{\max}-q_{\min}}{65535}
$$

则在原始坐标空间中，误差会随着位置幅值放大，其一阶近似可写为：

$$
|e_x| \approx \left|\frac{d}{dq}\left(sign(q)(e^{|q|}-1)\right)\right| * \frac{1}{2}\Delta_q
= e^{|q|}* \frac{1}{2}\Delta_q
$$

这说明：

- 当前方案的误差直接受局部 chunk 尺寸控制，属于线性空间中的局部绝对误差
- SOG 的误差先在对数空间近似均匀，但映射回原始坐标后，会随坐标幅值增大而放大

为了更直观地比较，可以看一个一维误差示例。

对于当前方案，若某个 chunk 在某一轴上的范围为 $L$ cm，则该轴的量化步长与最大绝对误差上界分别为：

$$
\Delta = \frac{L}{65535}, , |e|_{\max} \le \frac{L}{131070}
$$

因此：

- 若该 chunk 这一轴范围为 $L=1000$ cm，即 $10$ m，则：

$$
|e|_{\max} \le \frac{1000}{131070} \approx 0.0076\ cm
$$

- 若该 chunk 这一轴范围为 $L=10000$ cm，即 $100$ m，则：

$$
|e|_{\max} \le \frac{10000}{131070} \approx 0.076\ cm
$$

也就是说，只要 Morton 排序后每个 chunk 的局部范围控制得较好，position 的量化误差通常仍然处在远小于 $1$ cm 的量级。

再看 SOG。假设某一轴的全局场景范围近似为对称区间 $[-R,R]$，则对数空间量化步长为：

$$
\Delta_q = \frac{2\log(R+1)}{65535}
$$

回到原始坐标空间后，在位置 $x$ 处的一阶误差近似为：

$$
|e(x)| \approx (|x|+1)\frac{\log(R+1)}{65535}
$$

这意味着同样的 `16-bit` 编码下，SOG 的中心区域精度高，而越靠近场景边缘误差越大。

例如，若该轴的全局半径为 $R=10000$ cm，即场景总跨度约为 $200$ m，则：

- 在中心附近 $x \approx 0$ 处：

$$
|e(0)| \approx \frac{\log(10001)}{65535} \approx 0.00014\ cm
$$

- 在边缘附近 $x \approx R$ 处：

$$
|e(R)| \approx (10000+1)\frac{\log(10001)}{65535} \approx 1.41\ cm
$$

如果场景半径进一步增大到 $R=100000$ cm，即约 $1$ km，则边缘误差会进一步上升到：

$$
|e(R)| \approx (100000+1)\frac{\log(100001)}{65535} \approx 17.6\ cm
$$

因此，二者的差别可以概括为：

- 当前方案：误差主要取决于每个 chunk 的局部范围，局部精度更稳定
- SOG：中心精度很高，但边缘误差会随着全局场景范围增大而明显上升

这也是为什么当前方案更适合直接面向 Unreal 运行时的大场景渲染，而 SOG 更适合强调大动态范围压缩的文件格式。

### 6.2 Color 与 Opacity

`color` 与 `opacity` 一起存放在 `PackedColors` 中，每个 splat 一个 `uint32`：

- `R`：8 bit
- `G`：8 bit
- `B`：8 bit
- `A / Opacity`：8 bit

其中：

- `RGB` 不是固定按 `[0, 1]` 量化
- 而是先统计整个资产的 `ColorQuantMin / ColorQuantMax`
- 再将每个通道量化到对应的 8 bit 范围

`opacity` 则直接按 `[0, 1]` 量化到 8 bit。

若记某个颜色通道的原始值为 $c$，该资产在该通道上的量化范围为 $[c_{\min}, c_{\max}]$，则其量化公式为：

$$
u_c = clamp\left(round\left(255* \frac{c-c_{\min}}{c_{\max}-c_{\min}}\right), 0, 255\right)
$$

解码则为：

$$
\hat{c}=c_{\min}+\frac{u_c}{255}(c_{\max}-c_{\min})
$$

因此，该通道的量化步长为：

$$
\Delta_c = \frac{c_{\max}-c_{\min}}{255}
$$

最大绝对误差上界为：

$$
|e_c|\le \frac{1}{2}\Delta_c = \frac{c_{\max}-c_{\min}}{510}
$$

对 `opacity` 而言，由于其量化区间固定为 $[0,1]$，因此：

$$
u_\alpha = round(255\alpha), , \hat{\alpha}=\frac{u_\alpha}{255}
$$

对应误差上界为：

$$
|e_\alpha| \le \frac{1}{510}\approx 0.00196
$$

这意味着 `opacity` 的最大绝对量化误差约为 `0.2%`。

这种做法的特点是：

- 对当前资产的颜色分布更自适应
- 比固定全局范围更能利用 8 bit 的有效精度
- 解码成本低，运行时只需一次线性反量化

### 6.3 Rotation

`rotation` 存放在 `PackedRotations` 中，每个 splat 一个 `uint32`，采用 `smallest-three quaternion` 打包方式：

- 2 bit：记录被省略的最大分量位置
- 10 bit + 10 bit + 10 bit：保存剩余三个分量

这种方案的优点是：

- 四元数天然适合表示旋转
- 相比直接存 4 个 float，体积从 16 字节降到 4 字节
- 解码后可以直接参与协方差重建

若记单位四元数为

$$
q=(q_x,q_y,q_z,q_w), , \|q\|_2 = 1
$$

则 `smallest-three quaternion` 的基本思想是：

1. 找到绝对值最大的分量下标：

$$
m = \arg\max_i |q_i|
$$

2. 省略该分量，只保存其余三个分量
3. 利用单位四元数约束在解码时恢复被省略分量

这里代码中出现的 $\sqrt{2}/2$ 来自一个重要约束。由于被省略的是绝对值最大的分量，而单位四元数满足

$$
q_x^2+q_y^2+q_z^2+q_w^2=1
$$

因此最大的那个分量至少满足

$$
|q_m| \ge \frac{1}{2}
$$

于是剩余三个分量的平方和至多为：

$$
r_0^2+r_1^2+r_2^2 = 1-q_m^2 \le 1-\frac{1}{4} = \frac{3}{4}
$$

但对单个保留分量，还可以得到更紧的界。因为 $q_m$ 是绝对值最大的分量，所以任一保留分量 $r_i$ 都满足

$$
|r_i| \le |q_m|
$$

若某个保留分量超过 $\sqrt{2}/2$，则至少有两个分量的平方和会超过 $1$，与单位长度约束矛盾。因此保留分量的有效范围可以限制为：

$$
r_i \in \left[-\frac{\sqrt{2}}{2},\thinspace\frac{\sqrt{2}}{2}\right]
$$

这也是代码里用 $\sqrt{2}/2$ 而不是直接用 $[-1,1]$ 量化区间的原因：它利用了 `smallest-three quaternion` 的先验约束，把同样的 `10 bit` 精度集中到更小、更合理的数值范围内。

当前实现中，一个 `uint32` 被划分为：

- `2 bit`：保存被省略分量的下标 $m$
- `10 bit + 10 bit + 10 bit`：保存其余三个分量的量化结果

若记被保留的三个分量为 $r_0,r_1,r_2$，则其量化可更准确地写为：

$$
u_i = round\left(1023 * \frac{r_i+\frac{\sqrt{2}}{2}}{\sqrt{2}}\right), , i\in\{0,1,2\}
$$

解码后得到

$$
\hat{r}_i = \frac{\sqrt{2}}{1023}u_i - \frac{\sqrt{2}}{2}
$$

因此，每个保留分量的量化步长为：

$$
\Delta_r = \frac{\sqrt{2}}{1023}
$$

单个保留分量的最大绝对误差上界为：

$$
|e_r| \le \frac{1}{2}\Delta_r = \frac{\sqrt{2}}{2046} \approx 6.91\times 10^{-4}
$$

再由单位长度约束恢复被省略分量：

$$
\hat{q}_m = sign(q_m)\sqrt{\max\left(0,\thinspace1-\hat{r}_0^2-\hat{r}_1^2-\hat{r}_2^2\right)}
$$

最后再按记录的下标把四个分量放回原始顺序，得到解码后的四元数 $\hat{q}$。

因此，`rotation` 的压缩误差并不是单一标量步长，而是体现在解码后四元数与原始四元数之间的角度偏差上。若记二者夹角为 $\theta$，则可写为：

$$
\theta = 2\arccos\left(|\langle q, \hat{q} \rangle|\right)
$$

若仅从三个保留分量的量化误差出发做一个保守估计，则有：

$$
\|\delta r\|_2 \le \sqrt{3}* \frac{\sqrt{2}}{2046} \approx 1.20\times 10^{-3}
$$

对单位四元数而言，小误差下的旋转角偏差近似满足：

$$
\theta \approx 2\|\delta q\|_2
$$

因此可以得到一个保守的角度误差量级：

$$
\theta \lesssim 2.39\times 10^{-3}\ rad \approx 0.137^\circ
$$

这个数值不是逐资产严格实测误差，而是依据当前 `10-bit + 10-bit + 10-bit` 量化步长得到的保守上界量级。实际场景中，由于最大分量被省略、再通过单位长度约束恢复，平均误差通常还会低于这个值。

这也是为什么 `rotation` 更适合用“压缩到单个 `uint32` 后的角度误差是否足够小”来评价，而不是像 `position` 或 `color` 那样只看单轴绝对误差。


### 6.4 Scale

`scale` 当前已经改为与 SPZ 基本一致的固定 `8-bit` log-scale 量化方式。主要差别只是当前运行时使用 UE 厘米单位，因此整体加上了 `ln(100)` 的单位偏移。

当前实现中：

- `scale` 在导入阶段已经转为 UE 语义的 `log-scale`
- 三个轴分别使用固定范围、固定步长的 `8 bit` 对数空间量化
- 量化方式沿用 SPZ 的 scale 约定，并整体平移到 UE 厘米单位

对应数据为：

- `PackedScales`

源 `.ply` 中的 scale 本身就是 `log-scale`。导入时为了把米转换到 UE 厘米，会在对数空间加上：

$$
\ln(100)
$$

因此：

$$
s_{ue}=s_{src}+\ln(100)
$$

编码时每个轴独立量化：

$$
q=clamp
\left(
round
\left(
\frac{s_{ue}-s_{min}}{\Delta_s}
\right),
0,255
\right)
$$

其中：

$$
s_{min}=-10+\ln(100),, \Delta_s=\frac{1}{16}
$$

三个轴的 `uint8` 结果会打包进一个 `uint32`：

$$
\texttt{PackedScale}=q_x | (q_y \ll 8) | (q_z \ll 16)
$$

shader 中解码为：

$$
\hat{s}=s_{min}+q\Delta_s,, \hat{\sigma}=e^{\hat{s}}
$$

这里的对数空间步长固定为 `0.0625`，因此未发生 clamp 时，最大对数误差为半个步长：

$$
|e_s|\le 0.03125
$$

换算到线性 scale 上，大约是：

$$
e^{0.03125}\approx 1.0317
$$

也就是约 `3.2%` 的相对误差。

由于范围是固定的，它也存在固定上下界：

$$
s_{min}=-10+\ln(100),,
s_{max}=-10+\ln(100)+\frac{255}{16}=5.9375+\ln(100)
$$

也就是说，只有落在这个 log-scale 区间内的高斯才会按上述固定步长正常量化。若资产中存在 scale 超过该范围的高斯，需要注意它们会被 clamp 到最近的端点。

### 6.5 Normal Payload

当前压缩流中保留 `PackedNormals`，用于给后续基于 normal 的扩展留接口。当前渲染器暂时不会使用这条流做光照。

PLY 导入阶段会根据 Gaussian 自身形状估计法线：取 log-scale 最小的局部轴，再用导入后的 UE-space quaternion 旋转到世界方向。压缩时使用 octahedral encoding 打包到一个 `uint32` 中；目前低 16 bit 保存两个 8-bit octahedral 坐标，高位暂时保留给后续扩展。

### 6.6 Higher-order SH

SH 的压缩策略是当前方案与 SOG / SPZ 差异最大的部分。

首先需要明确：

- `degree 0` 的 DC 项已经并入 `PackedColors`
- `PackedSHData` 只存 `higher-order SH`，也就是视角相关部分

当前方案中，`higher-order SH` 的压缩流程为：

1. 统计当前资产全部 `higher-order SH` 标量的最小值 `SHMin` 和最大值 `SHMax`
2. 在线性区间 `[SHMin, SHMax]` 上构建 `256` 项标量 `codebook`
3. 将每个 SH 标量量化为一个 `8 bit` 索引
4. 每 `4` 个索引打包为一个 `uint32`

对应实现为 [GaussianSplatAsset.cpp](../Source/GaussianSplatting/Private/GaussianSplatAsset.cpp) 中的 `BuildDirectSHStreamFromSource`。

若记某个 `higher-order SH` 标量为 $h$，则当前 `codebook` 实际上是一个均匀采样的线性表：

$$
c_k = SH_{\min} + \frac{k}{255}\left(SH_{\max}-SH_{\min}\right), , k \in [0,255]
$$

量化标签可写为：

$$
\ell(h)=clamp\left(round\left(255* \frac{h-SH_{\min}}{SH_{\max}-SH_{\min}}\right), 0, 255\right)
$$

解码则为：

$$
\hat{h}=c_{\ell(h)}
$$

对应的量化步长为：

$$
\Delta_h = \frac{SH_{\max}-SH_{\min}}{255}
$$

因此，单个 SH 标量的最大绝对误差上界为：

$$
|e_h| \le \frac{1}{2}\Delta_h = \frac{SH_{\max}-SH_{\min}}{510}
$$

当前实现再把四个 `8-bit` 标签打包到一个 `uint32` 中，即：

$$
w = \ell_0 + (\ell_1 \ll 8) + (\ell_2 \ll 16) + (\ell_3 \ll 24)
$$

这意味着当前 SH 压缩是：

- 直接量化
- 每资产自适应动态范围
- 不做 `palette label` 聚类
- 不做跨 splat 的 SH 向量聚类

这样做的直接收益是：

- 导入阶段构建时间可控
- 误差行为更稳定
- shader 解码逻辑简单

代价则是：

- 压缩倍率不如更激进的 SH 聚类方案
- SH 数据仍然是当前压缩格式中比较大的组成部分

## 7. 为什么需要 Morton 排序

Morton 排序本身不是压缩编码，但它直接影响 `position` 压缩效果，因此在模型压缩流程中非常重要。

如果不先做 Morton 排序，而只是按原始导入顺序固定每 `256` 个 splat 组成一个 chunk，那么同一个 chunk 内的 splat 可能分布在很大的空间范围内。这样会导致：

- chunk 局部包围盒过大
- `position` 的 `16-bit` 归一化量化误差变大

Morton 排序后的 chunk 更具有空间局部性，因此：

- `ChunkPositionMins / Maxs` 更紧
- 同样是 `16-bit position`，实际精度更高
- 渲染阶段的空间访问局部性也更好，连续访问到相邻 splat 时，命中连续局部内存区域的概率更高，从而更有利于 GPU 的缓存与带宽利用

所以，Morton 排序是当前 `position` 压缩成立的重要前提之一。

## 8. 当前方案与 SOG 的差异

相对于 [SOG](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/)，当前方案最主要的区别在于：

### 8.1 没有采用 SOG 的聚类压缩路线

SOG 路线的一个核心特点，是通过更激进的聚类或 `palette` 化来压缩 SH 等高维属性，并可进一步结合更紧凑的整体格式来减轻运行时的 `memory-bound` 压力。从运行时角度看，这种路线本身是有意义的；如果直接采用 SOG 格式进行运行时，通常还会进一步减小显存带宽压力，这对大场景渲染本身是有利的。

当前实现没有采用这条路线，主要原因并不是它不适合运行时解码，而是它在大规模数据上的聚类构建太慢，尤其是在几百万到几千万 splat、并且 SH 维度较高的情况下，`k-means` 会显著拉长模型导入时间，使导入流程变得非常沉重。

因此，当前方案在 `higher-order SH` 上选择直接标量量化，在 `scale` 上选择固定 `8 bit` 对数空间量化，而没有继续沿用 SOG 那种更激进的聚类压缩路线。这样做牺牲了一部分压缩率和潜在的运行时带宽收益，但换来了更可控的导入时间和更稳定的构建流程。

## 9. 当前方案与 SPZ 的差异

相对于 SPZ，当前方案的主要区别在于：

### 9.1 当前统计的压缩倍率不包含 gzip 一类的通用压缩

SPZ 的最终文件体积通常会结合更通用的压缩流程来进一步降低大小。当前插件中的 `3.5x` 压缩倍率并不是按这种方式统计的，而是按“可直接渲染的压缩数据”统计的。

因此，当前格式和 SPZ 的倍率不能简单按一个数字直接对齐。SPZ 在最终文件体积上通常会更有优势，但那一部分优势并不完全来自本文所讨论的属性压缩本身。

### 9.2 SH 没有采用 SPZ 的固定 signed-byte 方案

SPZ 的 SH 压缩更偏向固定格式和固定范围的紧凑存储。当前实现没有采用那条路线，而是使用每资产自适应的标量 `codebook` 来压缩 `higher-order SH`。

如果不考虑 `gzip`，只比较运行时压缩数据本身，则 `degree 3` 下 SPZ 的单 splat 打包大小约为：

- `position`：`9` bytes
- `alpha`：`1` byte
- `color`：`3` bytes
- `scale`：`3` bytes
- `rotation`：`4` bytes
- `SH`：`45` bytes

合计约 `65 bytes / splat`。相对于原始 `Inria PLY` 风格约 `248 bytes / splat` 的浮点数据，其压缩倍率约为：

$$
\frac{248}{65} \approx 3.8\times
$$

这与当前方案约 `3.5x` 的压缩倍率已经比较接近。SPZ 真正进一步拉开体积差距，主要还是依赖 `gzip` 对低熵字节流做二次压缩。

在 SH 精度上，当前方案通常高于 SPZ 默认设置。当前实现使用每资产自适应的 `8-bit` 标量 `codebook`；而 SPZ 默认采用固定 `[-1,1]` 范围，并将 `degree 1` 压到 `5 bit`、`degree 2+` 压到 `4 bit` 的有效精度。因此，若不考虑 `gzip` 和熵压缩收益，SPZ 在文件体积上的优势并不大，但默认 SH 精度通常更低。

在 `scale` 上，当前实现已经有意采用 SPZ 风格的固定 `log-scale` 字节量化，只是因为运行时使用 UE 厘米单位，所以整体加上了 `ln(100)` 的偏移。需要注意的是，这也意味着 scale 存在固定上下界；如果资产中存在超过该范围的高斯，它们会被 clamp 到最近端点。

### 9.3 当前格式更偏运行时 GPU 访问，而不是最小磁盘体积

当前 `FGaussianSplatCompressedData` 的流布局非常强调：

- 每个 splat 固定步长访问
- 方便多对象合并为统一 GPU 静态缓冲
- shader 中直接按索引解码

这是一种明显偏运行时访问效率的设计。因此，它与 SPZ 面向更紧凑磁盘存储和分发的目标并不完全相同。当前没有采用 SPZ 的主要原因也在这里：SPZ 更偏向磁盘压缩，并且存在额外的 `gzip` 依赖；当前方案则更强调导入后直接用于运行时上传、合并与解码。

## 10. 当前方案的优势与代价

当前压缩方案的主要优势是：

- 相对于原始浮点数据，已有较明显的压缩收益
- 运行时缓冲更小，能减轻大场景下的 `memory-bound` 压力
- 导入阶段构建时间相对稳定
- 解码逻辑直接，便于 GPU 合并渲染
- 不依赖极慢的 SH `k-means`

主要代价则是：

- 压缩倍率低于更激进的 SOG / SPZ 路线
- `higher-order SH` 仍然占据较大比例
- 当前格式更像面向运行时的格式，而不是追求极致磁盘体积的格式

## 11. 小结

当前插件的模型压缩并不是追求“最小文件”，而是追求一种更适合 Unreal 实时渲染链路的折中方案：

- `position` 通过 Morton 排序 + 分块局部 `16-bit` 量化压缩
- `color / opacity` 通过 `8-bit` 打包压缩
- `rotation` 通过 `smallest-three quaternion` 压缩到单个 `uint32`
- `scale` 通过 SPZ 风格固定 `8-bit` 对数空间量化压缩
- `higher-order SH` 通过每资产自适应的 `8-bit` 直接量化与 `bit packing` 压缩

这套方案当前大致可以达到约 `3.5x` 的压缩倍率。`scale` 已经基本对齐 SPZ 的固定 log-scale 量化；当前方案与 SOG / SPZ 的主要差异更多体现在 `position` 分块量化、`higher-order SH` 编码、运行时 buffer 布局，以及是否叠加 `gzip` 或聚类压缩等方面。整体取舍是牺牲一部分极限压缩率，换来更稳定的导入时间、更直接的运行时解码路径，以及更适合当前 Unreal 插件工程目标的实现。

