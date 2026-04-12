# Amiga LightWave Plugin

LightWave 3D 5.x plugins for AmigaOS, cross-compiled with GCC.

Current version: `0.9.0`

## 0.9.0 Highlights

- New **NormalMap** shader plugin: samples a loaded image as a tangent-space
  normal map and perturbs surface normals at render time.  Supports planar
  projection (XY/XZ/YZ), tiling, adjustable strength, and DirectX Y-flip.
  Pairs with PBR and Fresnel for a full modern material stack.
- New **Motion** plugin: procedural animation for any item (object, light,
  camera).  Three modes: Wiggle (smooth noise), Bounce (sinusoidal), and
  Shake (decaying noise burst for camera shake).  Layers on top of
  keyframed motion with independent position/rotation control.
- New **Toon** image filter: post-render cel-shading with colour
  quantisation (2-8 bands) and ink-style outlines via depth and luminance
  edge detection.  Configurable outline colour, width, and edge sensitivity.

## 0.8.0 Highlights

- New **PNGsaver** plugin: saves rendered frames as compressed RGB PNG files
  using LZ77 + fixed Huffman deflate compression with Sub-filtered scanlines.
  No external libraries required.
- New **PNGloader** plugin: loads PNG files as textures, backgrounds, and
  foreground images. Includes a complete deflate decompressor supporting all
  standard PNG color types (grayscale, RGB, indexed, RGBA) and bit depths
  (1/2/4/8/16).

## 0.7.0 Highlights

- Fixed scene persistence for the **Fresnel** and **PBR** shaders so object
  property saves now stick correctly when the plugins are attached.
- Simplified **PBR Shader** to match its current implementation: AO and
  environment light are now documented and exposed as fast normal-based
  approximations, while blurred reflections remain the ray-traced feature.
- Expanded **LensFlare** to support a configurable flare-source cap up to 50.
- Optimized **ObjSwap** for larger replacement sets with faster sorting and
  frame lookup.

## Plugins

### ObjSwap

Object Replacement plugin for Layout. Automatically swaps objects based on
frame number derived from filename suffixes.

Given a base object `Ship.lwo`, the plugin scans the directory for
`Ship_010.lwo`, `Ship_100.lwo` etc. and replaces the object at the
corresponding frames. If no exact frame match exists, the most recent
replacement before the current frame is used.

### Fresnel

Physically-based Fresnel shader for Layout. Adds realistic angle-dependent
reflectivity to surfaces using Schlick's approximation. Edges become more
reflective and less transparent at glancing angles — essential for convincing
glass, water, and polished surfaces. Configurable IOR, power, and independent
control over reflection, diffuse and transparency effects.

### PBR Shader

Combined PBR-lite shader that brings modern material concepts to LightWave 5.x.
Includes variable metallic intensity (0-100), roughness (normal perturbation),
ambient occlusion and environment-light approximations based on the surface
normal, plus blurred reflections using multi-sample ray tracing. For
angle-dependent Fresnel effects, stack with the standalone Fresnel plugin.

### LensFlare

Post-render image filter that detects bright specular highlights and composites
glow and hexagonal star streaks over the rendered image. Finds the brightest
specular hotspots up to a configurable cap (default 8, maximum 50) and renders
warm-tinted flares with configurable threshold, radius, streak length, and
intensity. Applied via the Effects/Image Processing panel.

### PNGsaver

Image saver plugin that adds compressed PNG output support to LightWave's
rendering pipeline. When selected as the save format in render settings,
frames are written as standard RGB PNG files with LZ77 + fixed Huffman
compression and Sub-filtered scanlines. Typical compression ratios are
70-85% smaller than raw pixel data. No external libraries required.

### PNGloader

Image loader plugin that adds PNG input support to LightWave. Allows loading
PNG files as textures, background images, and foreground images. Includes a
complete deflate decompressor so it can read PNGs from any source (Photoshop,
GIMP, web, etc.). Supports all standard PNG color types (grayscale, RGB,
indexed, grayscale+alpha, RGBA) and bit depths (1/2/4/8/16).

### NormalMap

Shader plugin that samples a tangent-space normal map image to perturb surface
normals at render time.  Adds fine surface detail (bricks, rivets, fabric,
skin pores) to lighting and reflections without extra geometry.  Selects from
LightWave's loaded image list, supports planar projection (XY/XZ/YZ) with
tiling, and includes a DirectX Y-flip option.  Stack before Fresnel and PBR
for best results.

### Motion

Procedural motion plugin for objects, lights, and cameras.  Three modes:
**Wiggle** (smooth multi-octave noise), **Bounce** (sinusoidal oscillation),
and **Shake** (decaying noise burst for impacts/explosions).  Affects
position and/or rotation independently, layers on top of keyframed animation,
with configurable speed, amplitude per axis, and noise octaves.

### Toon

Post-render image filter that converts any render into a cel-shaded cartoon
look.  Quantises colours into 2-8 discrete bands and detects edges via depth
and luminance gradients for ink-style outlines.  Configurable outline colour,
width (1-3px), and separate depth/colour edge sensitivity controls.

## Toolchain

Uses `sacredbanana/amiga-compiler:m68k-amigaos` Docker image providing:
- `m68k-amigaos-gcc` 6.5.0b (GCC cross-compiler)
- Full AmigaOS NDK headers and libraries
- libnix (no ixemul.library dependency)

## Building

```bash
./build.sh          # Build SDK library + all plugins
./build.sh objswap  # Build ObjSwap only
./build.sh fresnel  # Build Fresnel only
./build.sh pbr      # Build PBR Shader only
./build.sh lensflare # Build LensFlare only
./build.sh pngsaver  # Build PNGsaver only
./build.sh pngloader # Build PNGloader only
./build.sh normalmap # Build NormalMap only
./build.sh motion    # Build Motion only
./build.sh toon      # Build Toon only
./build.sh clean    # Clean build artifacts
```

CI builds run automatically via GitHub Actions using the same Docker image.

## Installation

Copy the `.p` file(s) from `build/` to your LightWave plugins directory
on the Amiga, then add the plugin lines to your LW config file:

```
Plugin ObjReplacementHandler ObjSwap objswap.p ObjSwap
Plugin ObjReplacementInterface ObjSwap objswap.p ObjSwap
Plugin ShaderHandler Fresnel fresnel.p Fresnel
Plugin ShaderInterface Fresnel fresnel.p Fresnel
Plugin ShaderHandler PBR pbr.p PBR Shader
Plugin ShaderInterface PBR pbr.p PBR Shader
Plugin ImageFilterHandler LensFlare lensflare.p LensFlare
Plugin ImageSaver PNG(.png) pngsaver.p PNG(.png)
Plugin ImageLoader PNG(.png) pngloader.p PNG(.png)
Plugin ShaderHandler NormalMap normalmap.p NormalMap
Plugin ShaderInterface NormalMap normalmap.p NormalMap
Plugin ItemMotionHandler Motion motion.p Motion
Plugin ItemMotionInterface Motion motion.p Motion
Plugin ImageFilterHandler Toon toon.p Toon
Plugin ImageFilterInterface Toon toon.p Toon
```

## SDK

The `sdk/` directory contains the LightWave 5.x SDK headers and support
library, patched for GCC compatibility:

- `sdk/include/` — LW SDK headers (with GCC XCALL_ support added to `plug.h`)
- `sdk/lib/` — Built server library (`server.a`) and startup code (`serv_gcc.o`)
- `sdk/source/` — Server library source, GCC startup assembly, stubs

## Project Structure

```
├── build.sh              # Docker build wrapper
├── Makefile              # Build system
├── VERSION               # Semver version
├── .github/workflows/    # CI
├── sdk/
│   ├── include/          # LW 5.x SDK headers
│   ├── lib/              # Built libraries
│   └── source/           # Library source
└── src/
    ├── objswap/          # ObjSwap plugin source
    ├── fresnel/          # Fresnel shader source
    ├── pbr/              # PBR shader source
    ├── lensflare/        # Lens flare image filter source
    ├── pngsaver/         # PNG image saver source
    ├── pngloader/        # PNG image loader source
    ├── normalmap/        # Normal map shader source
    ├── motion/           # Procedural motion source
    └── toon/             # Toon cel-shading filter source
```
