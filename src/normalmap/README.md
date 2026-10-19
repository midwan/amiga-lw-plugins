# Normal Map Shader — Projected Normal and Material Maps for LightWave 3D

NormalMap projects a normal map and optional PBR-style material maps from object-space coordinates. It is intended for LWOB objects, which do not store UV coordinates. Every enabled map uses the same projection, tile values, and object-space origin, so a map set stays aligned.

## Installation

1. Run `Install` from the release archive, or copy `normalmap.p` from the matching `Plugins/` CPU drawer to the LightWave plugin directory.
2. In Layout, add the plugin with **Options > Add Plug-Ins** and restart Layout.
3. Add **NormalMap** in a surface's **Shaders** panel.

## Workflow for a map set

1. Load the normal map and any albedo, roughness, metallic, ambient-occlusion, and specular images into LightWave's image list.
2. Open **NormalMap Options** for the surface and select the images from their individual dropdowns. `(None)` disables a slot.
3. Choose an axis projection for a flat surface, or **Triplanar (box)** for automatic seamless projection over several faces.
4. Set tile values once. They apply to every slot, which keeps the source map set registered.
5. Choose an Albedo mode and adjust each map amount. Render after changing image selections; the shader prepares its image copies when Layout initializes the surface.

## Settings

| Setting | Range | Default | Description |
|---|---:|---:|---|
| Normal image | Image list / None | None | Tangent-space normal map. |
| Albedo / Colour | Image list / None | None | RGB base colour map. Replace blends toward the map; Multiply scales the existing surface colour. |
| Roughness | Image list / None | None | Sets `ShaderAccess.roughness` from the map luminance. |
| Metallic | Image list / None | None | Uses map luminance to raise mirror and specular, and reduce diffuse, matching the PBR shader's metallic treatment. |
| Ambient occlusion | Image list / None | None | Darkens surface colour and diffuse by map luminance. White is unoccluded. |
| Specular | Image list / None | None | Sets `ShaderAccess.specular` from map luminance. |
| Projection | XY / XZ / YZ / Triplanar (box) | XY | Common projection for all slots. |
| Tile X %, Tile Y % | 1–10000 | 100 | Common repeat factors; 100 is one repeat. |
| Normal strength | 0–100 | 100 | Blend from the unmodified normal to the normal-map result. |
| Flip normal Y | on/off | off | Use for DirectX-convention normal maps. |
| Box blend sharpness | 1–8 | 2 | Higher values retain each box face longer; lower values soften transitions. Applies only in Triplanar mode. |
| Albedo mode | Multiply / Replace | Replace | How the Albedo map combines with the surface's existing colour. |
| Map amounts | 0–100 | 100 | Per-map influence for Albedo, Roughness, Metallic, AO, and Specular. |

Single-channel maps are read as Rec. 709 luminance (`0.2126 R + 0.7152 G + 0.0722 B`). Grayscale images therefore work without conversion; a red-only map is also usable, but its luminance is lower than the red value.

## Projection modes

- **Planar XY, XZ, YZ** derive UVs from the corresponding object-space coordinates. Select the plane that faces the surface.
- **Triplanar (box)** samples XY, XZ, and YZ and blends them from the absolute object-space normal. It builds an axis-specific tangent frame for the normal map, so normal detail is oriented correctly on each box face. It is useful on cubes, rocks, and rounded meshes without UVs.

There are no UVs, projection offsets, or separate per-map transforms. This is deliberate: the shader makes an LWOB map set line up by sharing one projection. Use an image whose features tile at the object-space scale you select.

## Shader stacking

NormalMap changes its requested shader attributes only when the corresponding map slot is enabled: normal, colour, roughness, mirror/diffuse/specular, AO colour/diffuse, and specular. Put it before shaders that should use its perturbed normal or material attributes.

The included PBR shader has its own procedural roughness, metallic, and AO settings. Do not enable both plugins' controls for the same property unless their combined effect is wanted. NormalMap's metallic map follows PBR's scalar LightWave 5 treatment: it raises the scalar mirror/specular values and reduces diffuse. LightWave 5 exposes no independent RGB specular colour in `ShaderAccess`; use the Albedo map to supply the material colour associated with a metal.

## Persistence and compatibility

Version 2 saves the six image names and new settings. It also reads version 1 scene and LWOB `SDAT` data unchanged: the original normal image, planar projection, tiling, strength, and Flip Y values are retained, while new map slots remain disabled.
