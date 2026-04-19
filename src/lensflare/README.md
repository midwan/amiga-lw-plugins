# LensFlare — Specular Lens Flare for LightWave 3D

Post-render image filter that detects bright specular highlights and
composites glow and star streaks over the rendered image. Applied as
an Image Filter in Layout's Effects panel.

## How It Works

1. Scans the rendered RGB buffer for pixels above a brightness threshold
2. Identifies the brightest hotspots with depth-based scaling, up to the
   configured **Max Flares** limit (default 8, maximum 50)
3. Single-pass compositing renders per pixel:
   - Bright core at the source point
   - Circular glow with quadratic falloff
   - Chromatic ring at half-glow radius
   - Horizontal anamorphic streak
   - 6-point hexagonal star streaks with fade
4. Uses max-per-flare blending (brightest flare wins at each pixel,
   preventing blowout when flares overlap)
5. Depth-aware: further highlights produce smaller, subtler flares

The flare has a warm color tint (white center fading to amber) for a
natural camera lens look.

## Installation

1. Copy `lensflare.p` to your LightWave plugins directory
2. Run Layout and, without loading any scene or object, under Options tab click on 'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Layout so that the configuration file get updated with the new plug-in entry.

## Usage

1. In Layout, go to **Effects** panel (or Windows → Image Processing)
2. Add **LensFlare** to the image filter list
3. Ensure your scene has objects with visible specular highlights
4. Render — flares appear automatically on bright specular areas

### Default Settings

| Setting | Default | Description |
|---|---|---|
| Threshold | 220 | Minimum brightness (0-255) to trigger flare |
| Glow Radius | 100 | Radius of circular glow in pixels |
| Streak Length | 200 | Length of star streaks in pixels |
| Intensity | 80 | Overall flare brightness (0-100) |
| Streaks | 6 | Number of star streak arms (2/4/6) |
| Max Flares | 8 | Maximum number of flare sources to composite (1-50) |

### Tips

- **Lower the threshold** (150-180) for more flares on dimmer highlights
- **Increase glow radius** (60-100) for larger, softer bloom effects
- **Increase streak length** (120-200) for dramatic star patterns
- **Reduce intensity** (30-40) for subtle glints, increase (80-100) for dramatic flares
- Works best with high specular surfaces (metals, glass, wet surfaces)
- Multiple flare sources are supported (default 8, configurable up to 50)

### Performance Notes

- Pass 1 (brightness scan): O(width x height) — fast integer math
- Pass 2 (composite): single pass over entire image, checks each pixel
  against all flare sources with early distance-skip for fast rejection
- Composite cost scales with **Max Flares** and glow/streak size
- On JIT emulators: fast. On real 68k: depends on image resolution and
  glow radius (larger radius = more pixels within range per flare)

As of `0.7.0`, the higher flare-source limit is configurable in the UI and the
filter uses heap buffers instead of large callback-stack arrays.
