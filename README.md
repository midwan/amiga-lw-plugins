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

1. Download `LW-Plugins-<version>.lha` from
   [GitHub Releases](https://github.com/midwan/amiga-lw-plugins/releases)
   and extract it on the Amiga.
2. Run the `Install` icon in the `LW-Plugins` drawer (needs the Commodore
   Installer). It detects the CPU and FPU, preselects the matching build,
   and copies the plugins you choose into your LightWave plug-ins drawer,
   plus optional documentation.
3. Start Layout without loading a scene or object.
4. Open the Options tab, choose **Add Plug-Ins**, and select the copied
   `.p` file.
5. Restart Layout so LightWave writes the plugin entry to its config file.

The archive holds four builds of every plugin under `LW-Plugins/Plugins/`.
To install by hand, copy the `.p` files from the drawer matching your CPU:

| Drawer | CPU |
|---|---|
| `Generic` | Any 68020 or better, with or without FPU (68EC020/030, 68LC040/060) |
| `020fpu` | 68020/68030 with a 68881/68882 |
| `040fpu` | 68040 with FPU |
| `060fpu` | 68060 with FPU |

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

Build the release archive (`build/LW-Plugins-<version>.lha`, all four builds,
Installer script and Latin-1 docs):

```bash
./build.sh package
```

### FPU builds

The default build targets a 68020 without an FPU, so all floating-point math
goes through software routines. FPU builds use the FPU directly and render
faster with the shader, filter, and motion plugins:

```bash
./build.sh fpu       # all FPU variants
./build.sh 040fpu    # a single variant
```

| Variant | Flags | Use on | Output |
|---|---|---|---|
| `020fpu` | `-m68020 -m68881` | 68020/68030 with a 68881/68882 | `build-020fpu/` |
| `040fpu` | `-m68040 -m68881` | 68040 | `build-040fpu/` |
| `060fpu` | `-m68060 -m68881` | 68060 | `build-060fpu/` |

The `040fpu` and `060fpu` builds use `fd*`/`fs*` instructions that exist only
in the 68040/68060 on-chip FPU. Do not use them with a 68881/68882. The
`020fpu` build runs on a 68040/68060 only through the 68040/68060.library
emulation traps, so pick the variant that matches your CPU.

## SDK

The `sdk/` directory contains the LightWave 5.x SDK headers and support
library patched for GCC compatibility:

- `sdk/include/` - LW SDK headers
- `sdk/lib/` - built server library and startup object (FPU variants build
  their own copy in `build-<variant>/sdklib/`)
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
