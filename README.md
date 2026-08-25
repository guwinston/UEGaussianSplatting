# UEGaussianSplatting

[English](README.md) | [简体中文](README.zh-CN.md)

An Unreal Engine 5.5 sample project for the `GaussianSplattingRenderer` plugin.

This project demonstrates a practical 3D Gaussian Splatting workflow in Unreal Engine: import a 3DGS `.ply` file, generate a Gaussian Splat Asset, place it in a level, render it through the custom Gaussian renderer, and optionally align or capture views from imported `cameras.json` data.

![Quick start](Plugins/GaussianSplattingRenderer/Assets/get_start.gif)

## Features

- Ready-to-open UE 5.5 sample project
- Integrated `GaussianSplattingRenderer` plugin
- Import and render 3DGS-style `.ply` files
- Runtime compressed Gaussian data for lower memory bandwidth pressure
- GPU-visible-count DeviceRadix sorting; culled splats are not included in radix passes
- Selectable VS + PS and Mesh Shader + PS geometry paths with automatic VS fallback
- Screen-size culling and opacity-aware raster bounds for lower geometry and fragment cost
- Experimental no-sort stochastic splatting with temporal accumulation and camera-motion reprojection
- GPU-side global splat sorting for correct transparent composition
- Higher-order spherical harmonics up to degree 3
- Camera JSON import for viewport alignment and batch capture
- Optional proxy geometry for editor selection, collision queries, and shadow casting
- Demo level for quickly checking the renderer inside Unreal

## Requirements

- Unreal Engine 5.5
- Windows editor/runtime environment
- Visual Studio 2022 with C++ build tools

## Quick Start

1. Open `UEGaussianSplatting.uproject` with Unreal Engine 5.5.
2. Allow Unreal to rebuild modules if prompted.
3. Open `Content/Demo.umap`.
4. Drag a 3DGS `.ply` file into the Content Browser to create a Gaussian Splat Asset.
5. Drag the imported asset into the level.
6. Select the generated `GaussianSplatActor` and tune:
   - `SplatScale`
   - `MaxSHDegree`
   - `AlphaCullThreshold`
7. Use the editor viewport to inspect the result, or import `cameras.json` for camera alignment and image capture.

## Project Layout

```text
Config/                              Project configuration
Content/                             Demo level and optional sample assets
Source/                              Minimal project C++ module
Plugins/GaussianSplattingRenderer/   Gaussian splatting renderer plugin
UEGaussianSplatting.uproject          Unreal project file
```

## Plugin Documentation

For detailed plugin usage, rendering principles, GPU sorting, and compression notes, see:

- [GaussianSplattingRenderer README](Plugins/GaussianSplattingRenderer/README.md)
- [Rendering Principles](Plugins/GaussianSplattingRenderer/Docs/RenderingPrinciples.md)
- [GPU Sorting](Plugins/GaussianSplattingRenderer/Docs/GPUSorting.md)
- [Rendering Paths and Performance](Plugins/GaussianSplattingRenderer/Docs/RenderingPaths.md)
- [Stochastic Rendering](Plugins/GaussianSplattingRenderer/Docs/StochasticRendering.md)
- [Compression](Plugins/GaussianSplattingRenderer/Docs/Compression.md)
- [Mobile Platform Support](Plugins/GaussianSplattingRenderer/Docs/Mobile.md)
