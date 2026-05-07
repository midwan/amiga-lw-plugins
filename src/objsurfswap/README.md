# ObjSurfSwap — Surface-Preserving Object Replacement Plugin for LightWave 3D

Copyright (c) 2026 Dimitris Panokostas

ObjSurfSwap automatically replaces objects during rendering based on frame numbers
encoded in filenames. This is useful for frame-by-frame animation sequences,
such as digital counters, flipbook-style animation, or any effect where different
geometry is needed at specific frames.

ObjSurfSwap is separate from ObjSwap. Use ObjSwap for direct object sequence
replacement, and use ObjSurfSwap when the replacement geometry should render
with the surface definitions from the object loaded in Layout.

## How It Works

1. You load a **base object** in Layout (e.g., `digit`)
2. You activate ObjSurfSwap on that object via **Obj Rep Plug-ins**
3. The plugin scans the same directory for files matching the pattern
   `basename_N` where N is a frame number
4. During preview or render, the object is automatically replaced with the
   correct file for each frame

ObjSurfSwap preserves the surface settings from the base object file.
For numbered replacement objects, it creates a sidecar copy next to the source
object with the replacement object's geometry and `SRFS` surface name list,
but with the base object's top-level `SURF` chunks. Polygon assignments still
work, and each replacement load carries the same colors, textures, shaders,
and other surface parameters as the base object.

### Frame Matching Rules

- **Exact match**: If `digit_5` exists, it is shown at frame 5
- **Carry forward**: If no file exists for the current frame, the most recent
  replacement before it is used. For example, if only `digit_0` and `digit_5`
  exist, frames 1–4 will show `digit_0`
- **Before first match**: Frames before the first numbered file show the
  original base object

### Filename Format

The plugin looks for files named `basename_N` where:

- `basename` matches the base object name (case-insensitive)
- `_` (underscore) separates the name from the number
- `N` is the frame number — any number of digits (`5`, `05`, `005` all mean frame 5)
- A file extension is optional (`digit_10.lwo` and `digit_10` both work)

**Examples** — base object `Spaceship`:

| File | Replaces at frame |
|---|---|
| `Spaceship_0` | 0 |
| `Spaceship_10.lwo` | 10 |
| `Spaceship_100` | 100 |
| `Spaceship_0250.lwo` | 250 |

## Installation

1. Copy `objsurfswap.p` to your LightWave plugins directory
2. Run Layout and, without loading any scene or object, under Options tab click on 'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Layout so that the configuration file get updated with the new plug-in entry.

## Usage

1. **Load the base object** in Layout. Use the un-numbered version of your
   object (e.g., `digit` rather than `digit_0`). This is the object that will
   appear in the viewport and in frames before the first numbered replacement.

2. **Select the object** in the Objects panel.

3. Click **Obj Rep Plug-ins** and select **ObjSurfSwap** from the popup menu.

4. (Optional) Click **Options** to see which replacement files were found and
   their frame assignments.

5. **Preview or Render** your animation. The object will be replaced at each
   frame according to the matching rules above.

## Known Limitations

- **Viewport after scene load**: When you load a saved scene, the viewport
  will display the object from the last rendered frame (e.g., `digit_9`),
  not the base object. This is a LightWave limitation — Object Replacement
  plugins only take effect during Preview or Render, and LightWave saves the
  last-replaced object in the scene file. To restore the correct display,
  simply render the current frame — the plugin will replace the object with
  the correct one for that frame. This behavior is shared by all LightWave
  Object Replacement plugins (including the LOD plugin).

## Tips

- **File organization**: Keep all numbered variants in the **same directory**
  as the base object. The plugin only scans that directory.

- **Loading any variant**: You can load any numbered variant as the base
  object (e.g., `digit_5`). The plugin will strip the `_5` suffix and
  discover all siblings automatically. When the current frame maps back to
  that loaded object file, ObjSurfSwap keeps it loaded as-is so its own surface
  settings remain available for later replacements.

- **Scene persistence**: The plugin settings are saved with the scene file.
  When you reload the scene, the plugin will rescan the directory for
  replacement files.

- **Maximum files**: The plugin supports up to 4096 replacement files per
  object.

- **Large sequences**: As of `0.7.0`, scan results are sorted and searched
  more efficiently, so bigger replacement sets remain responsive.

- **Surface preservation**: Keep the intended render surfaces on the base
  object. Numbered objects should use the same surface names when they need
  those base surface settings. ObjSurfSwap may create `ObjSurfSwapCache-*.lwo`
  sidecar files beside the sequence objects; these contain replacement
  geometry plus the base object's surface definitions. Keep these with the
  project if you save scenes after previewing or rendering swapped frames.
