# Amiga LightWave Plugin

LightWave 3D 5.x plugins for AmigaOS, cross-compiled with GCC.

Current version: `0.8.0`

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
    └── pngloader/        # PNG image loader source
```
