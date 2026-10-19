# SurfList - Frame-Based Surface Replacement Shader

SurfList is a Layout surface shader that replaces the supported material values
of its assigned surface from LightWave surface files at selected frames.

## Installation

1. Run `Install` from the release archive, or copy `surflist.p` from the
   matching `Plugins/` drawer to the LightWave Layout plug-ins drawer.
2. Start Layout without loading a scene, choose **Options > Add Plug-Ins**, and
   add `surflist.p`.
3. Restart Layout.

## Usage

1. Assign **SurfList** in the Shader list of the surface to animate.
2. Open **Options** for SurfList and choose the list file. A new instance has
   no list selected and leaves its assigned surface unchanged.
3. Render or preview the scene.

The list is saved in the scene with the shader. Surface files are read when the
list is loaded, not while pixels are rendered.

## List format

A list alternates a frame number and a LightWave surface file path. Blank lines,
CR/LF line endings, and lines beginning with `#` are ignored.

```
#LW Surface Replacement List
0
3d:Surfaces/blue.srf
5
3d:Surfaces/asteroid.srf
23
3d:Surfaces/gold
```

The entry for frame N remains active until the next entry. Before the first
entry, SurfList leaves the assigned surface unchanged. A path without an
extension is also tried with `.srf` appended.

## Surface values applied

SurfList reads the first `SURF` chunk from each listed LightWave surface file
and applies:

- colour
- luminosity
- diffuse reflection
- specular reflection
- mirror reflection
- transparency
- refraction index
- roughness, derived from the file's glossiness

It supports the fixed-point and floating-point variants of the five percentage
values (`LUMI`/`VLUM`, `DIFF`/`VDIF`, `SPEC`/`VSPC`, `REFL`/`VRFL`, and
`TRAN`/`VTRN`).

Smoothing, double-sided state, textures and images, glossiness envelopes, edge
falloff, and reflection environments cannot be changed through `ShaderAccess`
and are not applied.

## Limits

SurfList accepts up to 256 valid list entries. The Options panel reports the
number of parsed entries and list or surface-file errors.
