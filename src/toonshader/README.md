# ToonShader — Per-Surface Cel Shading for LightWave 3D

ToonShader is a Layout surface shader for LightWave 5.x on AmigaOS. Apply it
only to the surfaces that need a cartoon look; other objects and surfaces keep
LightWave's normal rendering. It samples LightWave's shadowed light rays,
quantises the result into bands, and writes that result as luminous colour.
Cast shadows therefore remain visible inside the cel shading.

## Installation

1. Run `Install` from the `LW-Plugins` release archive, or copy `toonshader.p`
   from the archive's `Plugins/` drawer matching your CPU to the LightWave
   Layout plug-ins directory.
2. Start Layout without loading a scene or object, choose **Options > Add
   Plug-Ins**, and select `toonshader.p`.
3. Restart Layout so it records the plug-in.

## Usage

1. Open Layout's **Surfaces** panel and select the surface to stylise.
2. Add **ToonShader** in the surface's **Shaders** section.
3. Open **Options** and choose the bands, edge treatment and colours.
4. Render. Only surfaces carrying ToonShader are changed.

## Settings

| Setting | Range | Default | Description |
|---|---:|---:|---|
| Bands | 2–8 | 4 | Number of direct-lighting bands. |
| Band Softness | 0–100 | 0 | Softens the transition at the top of each band. Zero gives hard cel bands. |
| Outline | off/on | on | Enables silhouette darkening on the shaded surface. |
| Edge Width | 1–100 | 28 | Width of the dark silhouette region as a view-angle threshold. |
| Use Geometric Normal | off/on | off | Uses unsmoothed polygon normals for the edge test. Enable it for faceted outlines. |
| Edge Colour | RGB | black | Colour used for the silhouette region. |
| Highlight | off/on | on | Enables the hard Blinn highlight. |
| Highlight Size | 0–100 | 20 | Controls the angular size of the hard specular highlight spot. |
| Highlight Intensity | 0–100 | 100 | Strength of the highlight colour. |
| Highlight Colour | RGB | white | Colour of the hard highlight. |
| Rim Amount | 0–100 | 0 | Adds an optional view-edge glow before outlines are applied. |
| Rim Colour | RGB | pale blue | Colour of the rim light. |
| Shadow Colour | RGB | dark cool tint | Dark tint applied to the base colour for the unlit shadow band. |

## Notes

- ToonShader intentionally disables LightWave's regular diffuse, specular and
  mirror contributions for the affected surface, preventing it from relighting
  the already quantised result.
- `illuminate()` includes light falloff, spotlight cones, transparent blockers
  and cast shadows. A surface in shadow falls into the darkened shadow band.
- The shader does not trace rays and only loops over the scene's lights. On
  classic 68k machines, its cost rises with the number of enabled lights.
- It works on opaque surfaces. Do not apply it to a surface that depends on
  mirror reflections for its appearance.

All settings are stored with both scenes and object surfaces.
