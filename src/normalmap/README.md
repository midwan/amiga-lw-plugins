# Normal Map Shader — Texture-Based Normal Perturbation for LightWave 3D

A shader plugin that samples a tangent-space normal map image and perturbs
surface normals at render time.  This brings a modern material technique to
LightWave 5.x, allowing fine surface detail (bricks, rivets, fabric weave,
skin pores) to appear in lighting and reflections without adding geometry.

## How It Works

1. The user loads a standard RGB normal map image into LightWave's image list
   (via the PNGloader plugin or any supported format).
2. The shader derives UV coordinates from the object-space position using a
   selectable planar projection (XY, XZ, or YZ).
3. At each shading point, the normal map is sampled with anti-aliased
   filtering (`RGBSpot`).
4. The RGB colour is decoded into a tangent-space normal vector:
   `X = R*2-1`, `Y = G*2-1`, `Z = B`.
5. A tangent frame is constructed from the surface normal, and the
   tangent-space perturbation is transformed into world space.
6. The perturbed normal is blended with the original based on the Strength
   setting and written to `wNorm` for all subsequent shading.

## Installation

1. Copy `normalmap.p` to your LightWave plugins directory
2. Run Layout and, without loading any scene or object, under Options tab click on 'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Layout so that the configuration file get updated with the new plug-in entry.

## Usage

1. Load a normal map image into LightWave (Images panel or via PNGloader)
2. Open the **Surfaces** panel in Layout
3. Select a surface and go to the **Shaders** section
4. Add **NormalMap** from the shader list
5. Click **Options** to configure:
   - Select the normal map image from the dropdown
   - Choose the projection axis that matches your model's orientation
   - Adjust tiling and strength
6. Render to see the effect

### Settings

| Setting | Range | Default | Description |
|---|---|---|---|
| Image | (dropdown) | None | Select a loaded image as the normal map |
| Projection | XY / XZ / YZ | Planar XY | Axis pair used to derive UV coordinates |
| Tile X % | 1 - 10000 | 100 | Horizontal tiling factor (100 = 1x, 200 = 2x) |
| Tile Y % | 1 - 10000 | 100 | Vertical tiling factor |
| Strength | 0 - 100 | 100 | How strongly the normal map affects the surface |
| Flip Y | on/off | off | Flip the green channel for DirectX-convention maps |

### Choosing a Projection

- **Planar XY (front)**: Best for walls, screens, and front-facing surfaces
- **Planar XZ (top)**: Best for floors, ceilings, and ground planes
- **Planar YZ (side)**: Best for side walls and vertical surfaces

The projection determines how the 2D image maps onto the 3D surface.  For
best results, choose the axis that most closely faces the surface's primary
orientation.

### Normal Map Conventions

Most normal maps use the **OpenGL convention** (Y-up in tangent space) by
default.  If your normal map was created for DirectX (Y-down), enable
**Flip Y** to correct the green channel.

Normal maps should have a dominant blue colour (the Z component pointing
outward).  Pure flat normals are encoded as RGB (128, 128, 255).

### Stacking with Other Shaders

The Normal Map shader pairs well with:
- **Fresnel** — angle-dependent reflection now follows the perturbed normals
- **PBR Shader** — metallic/roughness responds to normal-mapped detail
- **LensFlare** — specular highlights from perturbed normals create more
  interesting flare patterns

Apply Normal Map **before** other shaders in the stack so they see the
modified normals.

## Scene Persistence

All settings including the image name are saved with the scene.  On reload
the shader looks up the image by name in LightWave's image list.  If the
image has not been loaded yet, the shader has no effect until it is.
