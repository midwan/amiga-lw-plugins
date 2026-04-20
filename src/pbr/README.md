# PBR Shader — Physically-Based Rendering for LightWave 3D

A combined PBR-lite shader that brings modern material concepts to LightWave 5.x
on AmigaOS. Includes variable metallic intensity, roughness, a normal-based
ambient occlusion approximation, multi-sample blurred reflections, and a
normal-based environment light approximation in a single plugin. For
angle-dependent Fresnel effects, stack with the standalone Fresnel plugin.

## Features

### Roughness
Perturbs surface normals based on object-space position using a deterministic
hash function. This breaks up perfect mirror reflections and sharp specular
highlights, simulating micro-surface detail without requiring bump maps.

### Ambient Occlusion
Uses a fast normal-based approximation to darken downward-facing and
glancing-angle surfaces. It does not ray trace nearby geometry, but it still
adds useful contact-shadow shaping and depth at very low cost.

### Metallic
Variable intensity (0-100) blending between dielectric and metallic behavior.
Higher values increase reflectivity, reduce diffuse, and boost specular using
an IOR-based Fresnel curve. At 100%, the surface is fully metallic with
near-zero diffuse — appearance dominated by reflections.

### Blurred Reflections
Replaces LightWave's single-ray mirror reflections with multi-sample cone
tracing. Casts 4/8/16 rays around the reflection direction, with cone spread
controlled by the Blur Spread setting (independent of roughness). The averaged
result is blended into the surface color, producing soft, spread-out
reflections for frosted, rough, or brushed materials.

### Environment Light
Uses a fast sky-facing normal approximation to add indirect fill light and a
small luminosity boost. This is not ray-traced global illumination, but it
helps keep surfaces from looking too flat.

## Installation

1. Copy `pbr.p` to your LightWave plugins directory
2. Run Layout and, without loading any scene or object, under Options tab click on 'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Layout so that the configuration file get updated with the new plug-in entry.

## Usage

1. Open the **Surfaces** panel in Layout
2. Select a surface and go to the **Shaders** section
3. Add **PBR Shader** from the shader list
4. Click **Options** to adjust settings
5. Render to see the effect

### Settings

| Setting | Range | Default | Description |
|---|---|---|---|
| IOR | 1.0 – 5.0 | 1.5 | Index of Refraction for metallic F0 |
| Metallic | 0 – 100 | 0 | Metallic intensity (0=dielectric, 100=full metal) |
| Roughness | on/off | off | Perturb normals for rough surfaces |
| Roughness Amount | 0 – 100 | 20 | Intensity of normal perturbation |
| AO | Off/On | Off | Enable the normal-based ambient occlusion approximation |
| AO Strength | 0 – 100 | 50 | Occlusion darkening intensity |
| Blur Refl | Off/2/4/8 | Off | Blurred reflection ray samples |
| Blur Spread | 0 – 100 | 30 | Cone spread angle |
| Env Light | Off/On | Off | Enable the sky-facing environment light approximation |
| Env Strength | 0 – 100 | 50 | Indirect lighting intensity |

For angle-dependent Fresnel effects (reflection, transparency, diffuse, specular),
stack the **Fresnel** plugin on the same surface.

### Material Presets

**Polished metal**: Metallic 100, IOR 2.0+, Roughness off + Fresnel plugin
- Set surface color to metal tint, high mirror

**Brushed metal**: Metallic 80, IOR 2.0+, Roughness on (30-50), Blur Refl 2-4

**Plastic**: Metallic 0, low Roughness (10-20) + Fresnel plugin (IOR 1.45)

**Concrete/stone**: Metallic 0, high Roughness (60-80), AO on, Env Light on (30-50)

### Performance Notes

- **Fresnel, Roughness, Metallic**: Very fast — no extra rays, pure math
- **Ambient Occlusion**: Very fast — normal-based approximation, no extra rays
- **Blurred Reflections**: Similar cost to AO — casts 4-16 rays per reflective
  surface point. Only active on surfaces with mirror > 0
- **Environment Sampling**: Very fast — normal-based approximation, no extra rays
- Only **Blurred Reflections** uses extra ray tracing in the current shader
- On JIT-equipped emulators blurred reflections are quite manageable; on
  real 68k hardware, keep blur samples low for final renders
- All features can be independently enabled/disabled

## Scene Persistence

All settings are saved with the scene file and restored on reload.
Older scenes with legacy AO radius/sample and environment sample values still
load, but the current shader only uses the on/off and strength controls for
those two approximations.

As of `0.7.0`, the shader no longer exposes the old unused AO radius/sample and
environment sample controls in the UI.
