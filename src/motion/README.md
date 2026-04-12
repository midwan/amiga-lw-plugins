# Motion — Procedural Animation for LightWave 3D

A procedural motion plugin that adds automatic animation to any item
(object, light, or camera) without keyframing.  The effect layers on top
of existing keyframed motion, so it can be combined with hand-animated
movement.

## Modes

### Wiggle
Smooth, organic random movement using multi-octave value noise.  Great for
floating objects, flickering lights, gentle camera drift, and anything that
needs natural-looking randomness.

### Bounce
Clean sinusoidal oscillation.  Each axis is phase-offset for a pleasing
circular or figure-8 pattern.  Useful for bobbing objects, pulsing lights,
and rhythmic mechanical motion.

### Shake
A decaying noise burst that starts at a user-defined frame and dies away
over time.  Perfect for camera shake on explosions, impacts, or earthquake
effects.  Before the start frame, there is no motion; after it, the
amplitude decays exponentially.

## Installation

1. Copy `motion.p` to your LightWave plugins directory
2. Add these lines to your LW config file:

```
Plugin ItemMotionHandler Motion motion.p Motion
Plugin ItemMotionInterface Motion motion.p Motion
```

3. Restart LightWave

## Usage

1. Select an item (object, light, or camera) in Layout
2. Open the **Motion Options** panel (or **Add Motion Modifier**)
3. Add **Motion** from the motion plugin list
4. Click **Options** to configure mode, amplitude, and speed
5. Play back or render to see the effect

### Settings

| Setting | Range | Default | Description |
|---|---|---|---|
| Mode | Wiggle/Bounce/Shake | Wiggle | Motion generation algorithm |
| Speed | 1 - 1000 | 10 | Frequency in tenths of Hz (10 = 1.0 Hz) |
| Octaves | 1 - 4 | 2 | Noise complexity (Wiggle/Shake only) |
| Affect Position | on/off | on | Add offset to position |
| Pos Amp X/Y/Z | integer | 10 | Position amplitude (hundredths of a unit) |
| Affect Rotation | on/off | off | Add offset to rotation |
| Rot Amp H/P/B | integer | 50 | Rotation amplitude (tenths of a degree) |
| Shake Start Fr | integer | 0 | Frame where shake begins |
| Shake Decay | 0 - 100 | 50 | How quickly shake fades (higher = faster) |

### Tips

- **Camera shake**: Set Mode to Shake, enable Affect Rotation with small
  Rot Amp values (20-50 = 2-5 degrees), disable Affect Position.  Set
  Shake Start to the impact frame.

- **Floating object**: Mode = Wiggle, Pos Amp Y = 20 (gentle vertical bob),
  Speed = 5 (slow), Octaves = 2.

- **Flickering light**: Apply to a point light, Mode = Wiggle, Affect
  Position off, Affect Rotation off.  Instead, keyframe the light intensity
  and use this for subtle position jitter.

- **Mechanical bob**: Mode = Bounce, enable only Pos Amp Y for a clean
  up/down oscillation.

- **Multiple octaves** add high-frequency detail on top of low-frequency
  movement.  1 octave is smooth and simple; 4 octaves gives a more chaotic,
  turbulent feel.

## Scene Persistence

All settings are saved with the scene file and restored on reload.
