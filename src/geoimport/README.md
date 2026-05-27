# GEOimport — VideoScape/Aegis Modeler .geo Object Loader for LightWave 3D

Object loader plugin that adds VideoScape 3D geometry input support to LightWave
3D 5.x. Load `.geo` files created by Aegis Modeler 3D or VideoScape 3D directly
into Modeler as editable meshes.

## How It Works

1. LightWave calls the loader with a `.geo` filename
2. The plugin reads the first 4 bytes to identify the file variant (ASCII or
   binary)
3. Point coordinates are read and converted (Z-axis negated for coordinate
   system alignment)
4. Polygons are read with their per-polygon color values
5. Each unique color is mapped to a named surface (`Color_0` through `Color_15`)
6. The mesh is sent to LightWave via the ObjectImport callback interface

### Supported Formats

| Variant | Magic | Description |
|---|---|---|
| ASCII | `3DG1` | Text-based geometry with decimal vertex coordinates |
| Binary | `3DB1` | Binary geometry with Amiga FFP floating-point values |

Both variants store point positions and polygon indices with per-polygon color
values (0–15). Colors are mapped to named surfaces rather than preserved as
RGB values.

### Limitations

- Polygons only — curves, patches, and subdivision surfaces are not supported
- No UV coordinates, smoothing groups, or material properties
- No export functionality (import only)
- FFP floating-point conversion may lose precision for very large or very small
  coordinate values in binary files

## Installation

1. Copy `geoimport.p` to your LightWave plugins directory
2. Run Modeler and, without loading any object, under the Menu layout click on
   'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Modeler so that the configuration file gets updated with the new
   plug-in entry.

## Usage

Once installed, `.geo` files will appear in LightWave's object file requesters
when loading objects. Select a `.geo` file and the geometry is imported as an
editable mesh with surfaces named by their original polygon colors.

## Technical Notes

- **Memory**: Allocates temporary buffers for point and index data via
  AllocMem/FreeMem. Memory is freed after each polygon is submitted.
- **File I/O**: Uses AmigaOS DOS library calls (Open/Read/Close).
- **FFP conversion**: Binary files use Amiga fast floating-point format (FFP),
  converted to IEEE 754 single-precision for LightWave.
- **No UI panel**: The loader has no configurable settings.
