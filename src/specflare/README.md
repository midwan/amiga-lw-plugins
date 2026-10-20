# SpecFlare — Specular Lens Flare for LightWave 3D

Post-render image filter that detects bright specular highlights and composites a glow, star filter, and optional anamorphic streak over the rendered image. Apply it as an Image Filter in Layout's Effects panel.

## Installation

1. Run `Install` from the `LW-Plugins` release archive, or copy `specflare.p` from the archive's `Plugins/` drawer matching your CPU (`Generic`, `020fpu`, `040fpu`, or `060fpu`) to the LightWave plugins directory.
2. Start Layout without loading a scene, choose **Options → Add Plug-Ins**, and select `specflare.p`.
3. Restart Layout after registering the plugin.

`SpecFlare` replaces `LensFlare`. Scenes saved with the old `LensFlare` server do not load it automatically; add **SpecFlare** again in the image filter list and save the scene.

## Usage

1. In Layout, open **Effects** (or **Windows → Image Processing**).
2. Add **SpecFlare** to the image filter list.
3. Render a scene with visible specular highlights.

## Settings

| Setting | Default | Description |
|---|---:|---|
| Threshold | 220 | Minimum RGB brightness (0–255) that starts a flare. |
| Glow Radius | 100 | Radius of the circular glow in pixels. |
| Streak Length | 200 | Length of a main star streak in pixels. |
| Intensity | 80 | Overall flare brightness (0–100). |
| Star Filter | 6 Point | `Off`, `2 Point`, `2+2 Point`, `4 Point`, `4+4 Point`, `6 Point`, `6+6 Point`, `8 Point`, `8+8 Point`, `10 Point`, or `10+10 Point`. `N+N` adds N shorter, dimmer streaks halfway between the N main streaks. |
| Rotation Angle | 0 | Star orientation in degrees. It is not wrapped, so negative values and values above 360 are valid. |
| Spin | 0 | Degrees added to the star orientation for each rendered frame. |
| Show Ring | On | Adds a chromatic ring at half the glow radius. |
| Anamorphic | On | Adds the horizontal blue-tinted anamorphic streak. It stays horizontal while the star filter rotates. |
| Max Flares | 8 | Bright flare sources to composite (1–250). |

## Saved settings

Scene data is a versioned, space-separated line:

```
2 threshold glowRadius streakLength intensity starFilter rotationAngle spin showRing anamorphic maxFlares
```

Object data starts with the same version (`2`) and stores the same fields as signed 32-bit values. `starFilter` is 0 through 10 in the order shown in the settings table. Rotation Angle and Spin are deliberately not wrapped.

## Tips

- Lower **Threshold** (150–180) to catch dimmer highlights.
- Increase **Streak Length** for more pronounced star points.
- Use an `N+N` filter for a denser star with subdued secondary points.
- Use **Spin** with an animation to rotate the star by a fixed number of degrees per frame.
- Lower **Intensity** (30–40) for subtle glints; higher settings suit dramatic highlights.
- High specular surfaces such as metal, glass, and wet objects produce the clearest sources.

## Performance notes

Detection and flare-source buffers for all 250 possible sources are allocated from the plugin heap, never the LightWave callback stack. The compositing pass rejects pixels outside each flare's maximum extent before evaluating glow or streaks. Composite cost still rises with the number of visible sources and the glow/streak sizes, especially on real 68k systems.
