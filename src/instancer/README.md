# Instancer — Geometry Copier for LightWave Modeler

Instancer copies source geometry onto background-layer points in LightWave 5.x
Modeler. It is a substitute for render-time instancing: LightWave 5 has no
renderer instancing API, so every result is ordinary geometry and uses memory
for every copy.

## Installation

Copy `instancer.p` from the release `Plugins/` drawer matching the machine to
the LightWave Modeler plug-ins drawer. In Modeler, use **Add Plug-Ins** in the
Menu layout, select the file, then restart Modeler. Instancer appears in the
**Custom** popup in the **Tools** menu.

## Workflow

1. Put the source mesh in the primary foreground layer. Its origin `(0,0,0)`
   is the placement point for each copy.
2. Put points in one or more background layers. Their polygons are ignored;
   every background point is a target.
3. Choose **Instancer** from **Tools > Custom**.
4. Set inclusive random ranges for Heading, Pitch, Bank, uniform Scale and a
   seed, then choose **OK**.
5. The source geometry remains in the primary layer and new copies are added
   there. The completion requester reports the number of copied polygons per
   instance. Use that number with the optional `InstanceColor` Layout shader.

Heading, Pitch and Bank use Modeler's H/P/B axes: Y, X and Z. A fixed value is
made by setting a range's minimum and maximum to the same value. Reusing the
same seed and source/target ordering gives the same transforms.

## Geometry Order and Limits

For each target, Instancer adds every copied point and then every copied
polygon before moving to the next target. Each copy therefore has the same
number of copied polygons and copies are contiguous in polygon creation order.
`InstanceColor` relies on this order when it derives an instance number from a
render-time polygon index.

Surfaces, curves and curve continuity flags are retained. The Modeler 5 mesh
editing API can read detail polygons but has no operation to create one or set
the detail flag, so detail polygons are not copied. There is no alignment to
point normals.

Instancer is not a memory-saving replacement for later LightWave instancing
systems. Save the output as an object if it is needed in Layout.

## Scripts and ARexx

Calling it from ARexx or a script with any argument, for example
`CMDSEQ('InstancerBatch', 'quiet')`, runs the copy step with the settings
remembered from the last interactive run (or the defaults) and shows no
dialogs, so it can run unattended. The polygons-per-instance count is then
the face count of the source mesh.

The argument can also carry nine numbers that override the settings:
heading minimum and maximum, pitch minimum and maximum, bank minimum and
maximum, scale minimum and maximum, and the seed, in that order. For
example `CMDSEQ('InstancerBatch', '15 75 -25 25 -40 10 0.8 1.2 7')` uses
heading 15-75, pitch -25-25, bank -40-10, scale 0.8-1.2 and seed 7. Fewer
than nine numbers leaves the remembered settings unchanged.
