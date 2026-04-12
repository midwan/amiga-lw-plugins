# Toon — Cel-Shading Image Filter for LightWave 3D

A post-render image filter that transforms any LightWave render into a
cartoon / cel-shaded look.  It combines colour quantisation (posterisation
into discrete shading bands) with edge detection for ink-style outlines,
producing a hand-drawn aesthetic from standard 3D scenes.

## How It Works

### Colour Quantisation
Each RGB channel is reduced to a configurable number of discrete levels
(2-8 bands).  This creates the flat, poster-like shading characteristic of
cel animation.  Fewer bands = harder, more graphic look; more bands = subtler
gradation.

### Edge Detection
Outlines are detected using two complementary methods:

1. **Depth edges** — A Sobel-like gradient operator on the depth buffer
   detects silhouette edges where objects meet the background or overlap.
2. **Colour edges** — Luminance discontinuities in the rendered image catch
   material boundaries, shadow edges, and texture seams that depth alone
   would miss.

Both edge types are combined and drawn with the user-configurable outline
colour and width.

## Installation

1. Copy `toon.p` to your LightWave plugins directory
2. Add these lines to your LW config file:

```
Plugin ImageFilterHandler Toon toon.p Toon
Plugin ImageFilterInterface Toon toon.p Toon
```

3. Restart LightWave

## Usage

1. Open **Effects > Image Processing** in Layout
2. Add **Toon** from the image filter list
3. Click **Options** to configure
4. Render to see the effect

### Settings

| Setting | Range | Default | Description |
|---|---|---|---|
| Quantise Colours | on/off | on | Enable colour band reduction |
| Bands | 2 - 8 | 4 | Number of colour levels per channel |
| Draw Outlines | on/off | on | Enable ink-style edge lines |
| Depth Edge | 1 - 100 | 15 | Sensitivity to depth discontinuities |
| Color Edge | 1 - 100 | 30 | Sensitivity to luminance discontinuities |
| Outline Width | Thin/Thick/Heavy | Thin | Line thickness (1-3 pixels) |
| Outline R/G/B | 0 - 255 | 0,0,0 | Outline colour (default: black) |

### Style Presets

**Classic cel**: Bands 3, Depth Edge 15, Color Edge 30, black outlines
— the quintessential cartoon look.

**Soft toon**: Bands 5-6, Depth Edge 10, outlines off
— gentle posterisation without hard outlines, like a painted illustration.

**Comic book**: Bands 2-3, Depth Edge 20, Color Edge 40, Thick outlines
— bold graphic style with strong contrast.

**Pencil sketch**: Quantise off, outlines on, Depth Edge 8, Color Edge 15,
outline colour dark grey (60,60,60)
— preserves photorealistic shading but adds line-art edges.

**White outlines on dark**: Set outline R/G/B to 255,255,255 for a neon or
blueprint look against dark scenes.

### Tips

- Lower **Depth Edge** values detect more edges (more sensitive).  Start
  around 15 and adjust up if you see too many lines.

- **Color Edge** catches shadow boundaries and texture detail that depth
  alone misses.  Set it higher (50+) to only catch the strongest contrasts.

- The filter works with any surface settings, shaders, and lighting.
  No special materials are needed.

- For best results with outlines, use simple lighting setups.  Complex
  multi-light scenes may produce noisy edges at low thresholds.

- Thick outlines (2-3px) look better at higher resolutions (640x480+).
  At low resolution, thin (1px) outlines are usually sufficient.

## Performance

Colour quantisation is very fast (simple integer math per pixel).  Edge
detection reads neighbouring pixels from the depth and colour buffers,
adding modest overhead.  Thick outlines require an additional edge dilation
pass using a temporary buffer.  Overall the filter adds negligible time
compared to the render itself.

## Scene Persistence

All settings are saved with the scene file and restored on reload.
