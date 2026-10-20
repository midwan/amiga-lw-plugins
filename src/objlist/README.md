# ObjList — Object Replacement Plugin for LightWave 3D

ObjList replaces an object's geometry from a text list of frame numbers and
object files. Each entry remains active until the next listed frame. Before
the first entry, the base object remains loaded.

The plugin server is named `ObjList`, but its file is `objlist2.p`. Classic
AmigaOS filenames are case-insensitive, and the original `OBJLIST.P` also
installs in Layout's plug-ins drawer and registers a different server named
`ObjectList`. The different filename lets both plugins coexist.

## List Format

A list alternates a frame line and an object path line. Blank lines and text
following `#` are ignored.

```text
#LW Object Replacement List
0
3d:Objects/flower.lwo
26
3d:Objects/car.lwo
51
3d:Objects/armadillo.lwo
```

The example uses `3d:` paths, which are passed to Layout unchanged. A path
without a volume name is relative to the directory containing the list file.
For example, if `Work:Lists/replacements.txt` contains `models/flower.lwo`,
ObjList loads `Work:Lists/models/flower.lwo`.

## Installation

1. Run `Install` from the `LW-Plugins` release archive, or copy `objlist2.p`
   from the archive's `Plugins/` drawer matching your CPU to your LightWave
   Layout plug-ins drawer.
2. Start Layout without loading a scene or object, choose **Options > Add
   Plug-Ins**, select `objlist2.p`, then restart Layout.

## Usage

1. Load the object that should appear before the first list entry.
2. Select that object and choose **Obj Rep Plug-ins > ObjList**.
3. Click **Options**, then **Pick List...** and select the list file.
4. The panel shows each parsed `Frame -> file` entry. Render or preview the
   animation.

The list file path is saved in the scene and is parsed again when the scene is
loaded. The list requester starts in the selected list's directory. Before a
list has been selected, it starts in the base object's directory after Layout
has evaluated the object, rather than at the content directory root.

## Limits

- Up to 4096 entries are read.
- Entries are ordered by frame before rendering.
- ObjList changes geometry only when the selected list entry differs from the
  currently loaded object.
