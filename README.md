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
| ObjMeshSwap | Surface-preserving object replacement | `objmeshswap.p` | [README](src/objmeshswap/README.md) |
| Fresnel | Shader | `fresnel.p` | [README](src/fresnel/README.md) |
| PBR | Shader | `pbr.p` | [README](src/pbr/README.md) |
| LensFlare | Image filter | `lensflare.p` | [README](src/lensflare/README.md) |
| PNGsaver | Image saver | `pngsaver.p` | [README](src/pngsaver/README.md) |
| PNGloader | Image loader | `pngloader.p` | [README](src/pngloader/README.md) |
| NormalMap | Shader | `normalmap.p` | [README](src/normalmap/README.md) |
| Motion | Item motion | `motion.p` | [README](src/motion/README.md) |
| Toon | Image filter | `toon.p` | [README](src/toon/README.md) |

## Installation

Most users should install a published release rather than build from source.

1. Download the latest release archive from
   [GitHub Releases](https://github.com/midwan/amiga-lw-plugin/releases).
2. Copy the `.p` plugin files you want to use to your LightWave plugins
   directory on the Amiga.
3. Start Layout without loading a scene or object.
4. Open the Options tab, choose **Add Plug-Ins**, and select the copied
   `.p` file.
5. Restart Layout so LightWave writes the plugin entry to its config file.

Each plugin has its own setup and usage notes. See the README linked in the
plugin table above for the plugin you are installing.

## Building From Source

Source builds require Docker and the
`sacredbanana/amiga-compiler:m68k-amigaos` image. The image provides
`m68k-amigaos-gcc` 6.5.0b, AmigaOS NDK headers and libraries, and libnix
for `-noixemul` builds.

Build everything:

```bash
./build.sh
```

Build a single plugin:

```bash
./build.sh objswap
./build.sh objmeshswap
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
    |-- objmeshswap/
    |-- fresnel/
    |-- pbr/
    |-- lensflare/
    |-- pngsaver/
    |-- pngloader/
    |-- normalmap/
    |-- motion/
    `-- toon/
```
