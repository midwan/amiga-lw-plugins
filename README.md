# Amiga LightWave Plugins

LightWave 3D 5.x plugins for AmigaOS, cross-compiled with GCC.

This repository contains Layout and rendering plugins built as AmigaOS
LoadSeg-able `.p` modules. Release notes and version-specific changes belong
in GitHub Releases; this README documents the current project layout, build
workflow, and plugin set.

## Plugins

| Plugin | Class | Output | Documentation |
|---|---|---|---|
| ObjSwap | Object replacement | `objswap.p` | [README](src/objswap/README.md) |
| ObjSurfSwap | Surface-preserving object replacement | `objsurfswap.p` | [README](src/objsurfswap/README.md) |
| Fresnel | Shader | `fresnel.p` | [README](src/fresnel/README.md) |
| PBR | Shader | `pbr.p` | [README](src/pbr/README.md) |
| LensFlare | Image filter | `lensflare.p` | [README](src/lensflare/README.md) |
| PNGsaver | Image saver | `pngsaver.p` | [README](src/pngsaver/README.md) |
| PNGloader | Image loader | `pngloader.p` | [README](src/pngloader/README.md) |
| NormalMap | Shader | `normalmap.p` | [README](src/normalmap/README.md) |
| Motion | Item motion | `motion.p` | [README](src/motion/README.md) |
| Toon | Image filter | `toon.p` | [README](src/toon/README.md) |

## Requirements

- Docker
- `sacredbanana/amiga-compiler:m68k-amigaos`

The Docker image provides:

- `m68k-amigaos-gcc` 6.5.0b
- AmigaOS NDK headers and libraries
- libnix with `-noixemul` builds

## Building

Build everything:

```bash
./build.sh
```

Build a single plugin:

```bash
./build.sh objswap
./build.sh objsurfswap
./build.sh fresnel
./build.sh pbr
./build.sh lensflare
./build.sh pngsaver
./build.sh pngloader
./build.sh normalmap
./build.sh motion
./build.sh toon
```

Clean generated objects and plugin binaries:

```bash
./build.sh clean
```

Build outputs are written to `build/`.

## Installation

Copy the required `.p` files from `build/` to your LightWave plugins
directory on the Amiga. Then add the relevant plugin registration lines to
your LightWave config file.

```text
Plugin ObjReplacementHandler ObjSwap objswap.p ObjSwap
Plugin ObjReplacementInterface ObjSwap objswap.p ObjSwap
Plugin ObjReplacementHandler ObjSurfSwap objsurfswap.p ObjSurfSwap
Plugin ObjReplacementInterface ObjSurfSwap objsurfswap.p ObjSurfSwap
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

For plugin-specific setup and usage, see the README under each `src/`
subdirectory.

## SDK

The `sdk/` directory contains the LightWave 5.x SDK headers and support
library patched for GCC compatibility:

- `sdk/include/` - LW SDK headers
- `sdk/lib/` - built server library and startup object
- `sdk/source/` - server library source, GCC startup assembly, and stubs

## Project Structure

```text
.
|-- build.sh
|-- Makefile
|-- VERSION
|-- sdk/
|   |-- include/
|   |-- lib/
|   `-- source/
`-- src/
    |-- objswap/
    |-- objsurfswap/
    |-- fresnel/
    |-- pbr/
    |-- lensflare/
    |-- pngsaver/
    |-- pngloader/
    |-- normalmap/
    |-- motion/
    `-- toon/
```
