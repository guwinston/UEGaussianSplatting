# Stochastic Splat 随机渲染

[English](StochasticRendering.md) | [简体中文](StochasticRendering.zh-CN.md)

Stochastic splatting 是全局透明 Gaussian 排序的一条实验性替代路径。选择方式：

```text
r.GaussianSplat.SortMethod 2
```

## 管线

1. Key 生成 pass 执行常规剔除，但把可见 splat ID 直接压缩到最终索引缓冲。
2. 跳过全局深度排序。
3. Pixel Shader 使用每帧变化的随机样本，把 Gaussian alpha 转换为随机接受/拒绝结果。
4. 接受的样本写入颜色和独立 Stochastic depth target，在当前帧中作为不透明样本解决可见性。
5. 时域 pass 对多帧求平均，重建期望的半透明结果。

单帧估计器天然包含噪声。类似老电视雪花的现象是 Monte Carlo 方差，而不是缺少某个空间去噪器；时域累积才是主要去噪手段。

## 时域累积

`r.GaussianSplat.StochasticTemporalSamples` 控制最大历史长度：

```text
r.GaussianSplat.StochasticTemporalSamples 1000
```

设为 `0` 会关闭历史并直接暴露原始随机噪声。值越大，静止画面越平滑，但变化后重新收敛所需时间也越长。稳定视图达到设定样本数后，可以继续复用已收敛历史。

历史不再有效时会重置，包括 camera cut，以及相关场景、视图、对象描述或渲染 CVar 改变。

## 相机运动重投影

开启重投影后，VS/MS 会输出每个 splat 中心从上一视图到当前视图的屏幕空间运动。Pixel Shader 将运动写入第二个 render target，时域 pass 再从重投影位置采样历史。

```text
r.GaussianSplat.StochasticReprojection 1
r.GaussianSplat.StochasticMotionSamples 8
```

如果当前像素被拒绝，时域 Shader 还会在 3x3 邻域搜索 motion；附近没有有效 motion 时，则使用更高当前帧权重的短零运动回退。相机移动期间，`StochasticMotionSamples` 会限制有效历史长度，避免陈旧样本占据过高权重；相机停止后，累积可以再次向更大的时域上限收敛。

做 A/B 测试时可将 `StochasticReprojection` 设为 `0`。此时相机变化会使历史失效，而不是对历史做重投影。

## 几何路径

Stochastic 渲染与几何生成方式相互独立：

```text
r.GaussianSplat.GeometryMode 0  # VS + PS
r.GaussianSplat.GeometryMode 1  # 请求 Mesh Shader + PS
```

不支持 Mesh Shader 的 RHI 会自动回退 VS。

## 性能与显存

该路径移除了全局深度排序，但增加了 Stochastic depth、时域累积，以及开启重投影时的 motion render target，因此并不保证在所有场景中都比 DeviceRadix 更快。

Motion texture 使用 `PF_FloatRGBA`，约为每像素 8 字节：1920x1080 约 15.8 MiB，3840x2160 约 63.3 MiB；这还没有包含其他 Stochastic history/depth 资源，也未计入 RDG aliasing 的影响。

建议分别测量：

- Key 生成与压缩；
- Stochastic 光栅化与 overdraw；
- 时域/重投影 pass；
- 相机移动期间和收敛后的总 GPU 时间。

## 建议测试组合

原始单帧估计器：

```text
r.GaussianSplat.SortMethod 2
r.GaussianSplat.StochasticTemporalSamples 0
```

静止收敛：

```text
r.GaussianSplat.StochasticTemporalSamples 1000
r.GaussianSplat.StochasticReprojection 1
```

运动画质 A/B：

```text
r.GaussianSplat.StochasticReprojection 0
r.GaussianSplat.StochasticReprojection 1
r.GaussianSplat.StochasticMotionSamples 4
r.GaussianSplat.StochasticMotionSamples 8
r.GaussianSplat.StochasticMotionSamples 16
```

## 当前限制

- Motion vector 表示 splat 中心由相机产生的运动；独立动画或形变 Gaussian 尚未通过逐对象运动重投影。
- 反遮挡、极细结构和大幅屏幕空间运动仍可能出现噪声或短拖影。
- 估计器需要多帧；camera cut 后立即截图可能仍有噪声。
- 额外的全分辨率 target 在带宽和显存受限的移动 GPU 上成本可能较高。
- 这是实验性的画质/性能取舍，DeviceRadix 仍是默认路径。
