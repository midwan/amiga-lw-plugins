# GEOimport — VideoScape 3D .geo Object Loader for LightWave 3D

Object loader plugin that lets LightWave Modeler 5.x load VideoScape 3D object
files (`.geo`), as written by VideoScape 3D, Aegis Modeler 3D, Blender's old
VideoScape exporter and various converters. The object loads as an ordinary
editable mesh, and can then be saved as a LightWave object.

## How It Works

1. Modeler offers every object file it does not recognise to the installed
   object loaders, in order of their names
2. The plugin checks the first four bytes for `3DG1` (text) or `3DB1`
   (binary); any other file is left to the other loaders. The loader is
   named `GEO(.geo)` so that it comes before `Translator3D-Import`, which
   ships with LightwaveRTG and fails on `.geo` files instead of passing them on
3. Points are passed to LightWave unchanged: VideoScape and LightWave use the
   same axes (+X right, +Y up, +Z forward) and the same clockwise polygon
   order, so objects keep their orientation and their visible sides
4. Polygons are passed with their colour codes turned into surfaces
5. Detail polygons stay detail polygons

### Supported Formats

| Variant | Magic | Written by |
|---|---|---|
| Text | `3DG1` | VideoScape 3D, Modeler 3D "Save As - Text", Blender, converters |
| Binary | `3DB1` | Modeler 3D "Save As - Binary" |

The text format is read as a stream of numbers, so LF, CR and CRLF line ends,
blank lines and indentation all work. Coordinates may use exponents
(`1.5e-3`). The binary format stores big-endian words and Motorola FFP
floating point coordinates.

### Colours and Surfaces

Each VideoScape colour code becomes one surface. The surface name holds the
colour, the shading type and the original code, for example
`Lt.Red Glossy (28)`. The code is decoded as in the Modeler 3D colour chart:

| Code | Meaning | LightWave surface |
|---|---|---|
| 0-15 | Matte colour (Black, Dk.Blue, ... Yellow, White) | Colour, 100% diffuse |
| +16 | Glossy | 50% specular, medium glossiness |
| +32 | Unshaded | 100% luminous, 0% diffuse |
| +48 | Outline | Outline option |
| +64 | Transparent | 50% transparency |
| +128 | Smooth | Smoothing, 89.5° maximum angle |
| 256 | Invisible | 100% transparency |
| 257 | Invisible, darkens what it covers | Black, 50% transparency |
| 258 | Invisible, brightens what it covers | White, 50% additive transparency |
| 259 | Chrome | Grey, 100% reflection and specular |
| 260 | Chrome, smoothed | As 259 with smoothing |

A negative code marks a polygon with detail polygons, as in VideoScape.
Codes above 260 get a grey surface named `Code (<n>)`.

Blender writes hexadecimal face colours (`0xcccccc`, blue-green-red order,
emission in the top byte). These become surfaces named after the RGB value,
for example `RGB CCCCCC`, with that colour and the emission as luminosity.

Surface settings are only applied to surfaces that do not exist yet. When an
object is loaded into a Modeler session that already has a surface with the
same name, the existing surface is kept.

The palette RGB values are those used by the AmigaWorld GEO ray tracer;
VideoScape itself let you replace its palette, so the colours may not match
the original renders exactly.

### Limitations

- Only mesh objects (`3DG1`, `3DB1`). Vertex-coloured meshes (`GOUR`), lamp
  files (`3DG2`) and curve/NURBS files (`3DG3`) are not loaded
- Polygons with more than 200 points are rejected (LightWave 5 limit)
- Detail polygons of detail polygons are rejected (LightWave cannot store them)
- No export (import only)

## Installation

1. Run `Install` from the `LW-Plugins` release archive, or copy `geoimport.p`
   from the archive's `Plugins/` drawer that matches your CPU (`Generic`,
   `020fpu`, `040fpu` or `060fpu`) to your LightWave Modeler plugins directory
2. Run Modeler and, without loading any object, under the Menu layout click on
   'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Modeler so that the configuration file gets updated with the new
   plug-in entry.

Layout reads `.geo` files with its own built-in reader and never passes them
to object loaders, so GEOimport has no effect there. That reader does not
handle detail polygons; load such objects in Modeler and save them as
LightWave objects.

## Usage

Load a `.geo` file through Modeler's normal object load requester (or the
ARexx `LOAD` command). If the file is damaged (it ends early, refers to a
point that does not exist, or contains text that is not a number), the
loader rejects it and hands Modeler a message that says what is wrong.

## Technical Notes

- **Memory**: One block of about 6 KB holds the read buffer and loader state.
  Points and polygons are passed to LightWave as they are read, so memory use
  does not grow with the object size.
- **File I/O**: Uses AmigaOS DOS library calls (Open/Read/Seek/Close) with a
  4 KB read buffer.
- **Progress**: When the host passes a progress monitor, the loader advances
  it per 4 KB read and stops if the user aborts.
- **No UI panel**: The loader has no configurable settings.
