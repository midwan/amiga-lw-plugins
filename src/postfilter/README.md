# PostFilter — Image Filter for LightWave 3D

PostFilter is a configurable post-render image filter for LightWave 3D 5.x.
It combines colour correction, grain, blur, edge-preserving smoothing and
unsharp masking in one filter instance.

This is an original GPL implementation inspired by the public feature set of
WaveFilter. It is not derived from WaveFilter source code.

## Installation

1. Run `Install` from the `LW-Plugins` archive, or copy `postfilter.p` from
   the `Plugins/Generic`, `Plugins/020fpu`, `Plugins/040fpu` or
   `Plugins/060fpu` drawer that matches the CPU.
2. In Layout, with no scene or object loaded, choose **Options > Add Plug-Ins**
   and select `postfilter.p`.
3. Restart Layout so it records the plug-in.

## Usage

1. Open **Effects > Image Processing** in Layout.
2. Add **PostFilter** from the image-filter list.
3. Click **Options**, choose a region and enable the operations to use.
4. Render the scene.

All settings are saved in the scene.

## Region

| Region | Effected pixels |
|---|---|
| Full frame | The complete render |
| Objects only | Pixels with a non-zero render alpha value |
| Background only | Pixels with a zero render alpha value |

The region mask is also respected while sampling blur neighbours, so an
objects-only blur does not pull backdrop colours onto an object edge, and a
background-only blur does not soften an object into the backdrop.

## Operations and order

Enabled operations run in this fixed order:

1. **Colour** — RGB balance, brightness, contrast, gamma, saturation,
   greyscale, negative and posterize.
2. **Grain** — deterministic monochrome noise.
3. **Blur** — separable box or weighted Gaussian-style blur.
4. **Smooth** — a 3×3 edge-preserving pass that averages only neighbours
   whose luminance differs by no more than **Smooth Variance**.
5. **Sharpen** — an unsharp-mask pass.

**Passes** applies to blur and smooth. It does not repeat colour, grain or
sharpen.

### Colour

The balance, brightness, contrast, gamma and saturation controls use 100% as
the neutral value. Posterize is disabled at 0; select 2–32 levels to quantise
each colour channel. Greyscale and negative are applied after saturation and
before posterize.

### Blur and sharpen

Blur Radius is 1–12 pixels. Box gives equal weight to samples; Gaussian uses
a triangular weight centred on the pixel. Sharpen Amount is 0–200%.

## Limitations

LightWave 5.0 gives image filters no depth buffer, so PostFilter has no
depth-of-field pass. Use the camera's built-in Depth of Field for DOF.

## Performance

Colour, grain and the neighbourhood filters use integer arithmetic where it
is useful. Gamma is calculated once per frame into a 256-entry lookup table.
Blur is separable, and all full-frame working buffers are allocated from the
Amiga public heap rather than the LightWave callback stack. Large blur radii
and multiple passes increase render time.
