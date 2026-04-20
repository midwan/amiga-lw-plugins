# Fresnel Shader — Physically-Based Reflection for LightWave 3D

The Fresnel shader adds realistic angle-dependent reflectivity to surfaces.
Real-world materials reflect more light at glancing angles (edges) and less
at perpendicular angles (center). This effect is essential for convincing
glass, water, car paint, and any polished or transparent surface.

## How It Works

The shader uses **Schlick's approximation** of the Fresnel equations:

```
F = F0 + (1 - F0) × (1 - cos θ)^power
```

Where:
- **θ** is the angle between the view ray and surface normal
- **F0** is the base reflectivity at perpendicular incidence, derived from
  the Index of Refraction: `F0 = ((IOR - 1) / (IOR + 1))²`
- **power** controls the curve steepness (5 = physically correct)

The shader then:
- **Increases reflection** at glancing angles (edges become mirror-like)
- **Decreases transparency** at glancing angles (edges become opaque)
- **Decreases diffuse** at glancing angles (energy conservation)
- **Increases specular** at glancing angles (brighter highlights at edges)

## Installation

1. Copy `fresnel.p` to your LightWave plugins directory e.g Toaster:Programs/Plugins/layout
2. Run Layout and, without loading any scene or object, under Options tab click on 'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Layout so that the configuration file get updated with the new plug-in entry.

## Usage

1. Open the **Surfaces** panel in Layout
2. Select a surface and go to the **Shaders** section
3. Add **Fresnel** from the shader list
4. Click **Options** to adjust settings
5. Render to see the effect

### Settings

| Setting | Range | Default | Description |
|---|---|---|---|
| Index of Refraction | 1.0 – 5.0 | 1.5 | Controls base reflectivity. Higher = more reflective at all angles |
| Affect Reflection | on/off | on | Boost the surface's mirror/reflection at glancing angles |
| Reflection Power | 1 – 10 | 5 | Reflection curve steepness. 5 = physically correct |
| Affect Transparency | on/off | on | Reduce the surface's transparency at glancing angles |
| Affect Diffuse | on/off | on | Reduce the surface's diffuse at glancing angles (energy conservation) |
| Diffuse Power | 1 – 10 | 5 | Diffuse curve steepness. Independent from reflection power |
| Affect Specular | on/off | off | Boost specular highlights at glancing angles |

### Common IOR Values

| Material | IOR |
|---|---|
| Air | 1.00 |
| Water | 1.33 |
| Glass | 1.50 |
| Crystal | 2.00 |
| Diamond | 2.42 |

### Tips

- **Glass/water**: Set the surface transparency to a high value (e.g., 80–100%)
  and mirror/reflection to a low value (e.g., 10–20%). The Fresnel shader will
  blend between them based on viewing angle — transparent when looking straight
  through, reflective at the edges.

- **Metallic surfaces**: Use a higher IOR (2.0+) for metals, which have
  higher base reflectivity. You can disable "Affect Transparency" for opaque
  metals that only need edge reflection boost.

- **Artistic control**: Reduce the power below 5 for a broader, more
  pronounced Fresnel effect. Increase it above 5 for a tighter edge-only
  effect.

- **Subtle enhancement**: Even for surfaces that aren't obviously reflective,
  a subtle Fresnel (IOR 1.3–1.5) adds realism by brightening edges slightly.

## Scene Persistence

All settings are saved with the scene file and restored when the scene is
reloaded.
