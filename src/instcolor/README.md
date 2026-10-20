# InstanceColor — Per-Instance Colour Shader

InstanceColor is a LightWave 5.x Layout shader for geometry made with the
Modeler `Instancer` plugin. It uses the render-time polygon index to assign a
deterministic HSV variation to each contiguous copied instance.

## Installation

Copy `instcolor.p` from the release `Plugins/` drawer matching the machine to
the LightWave Layout plug-ins drawer. In Layout, choose **Options > Add
Plug-Ins**, select the file, then restart Layout.

## Workflow

1. In Modeler, create copies with **Instancer** and note the `polygons per
   instance` number reported when it completes.
2. Save the resulting object and load it into Layout.
3. Open the object's Surface panel, choose the surface used by the copies,
   add **InstanceColor** in the **Shaders** section, and open **Options**.
4. Set **Polygons per instance** to the number reported by Instancer. Set a
   seed and the hue, saturation and value ranges, then render.

All copies need the same source surface for one shader to affect them. When an
object has several source surfaces, add the shader to each surface with the
same settings.

## Settings

| Setting | Meaning |
|---|---|
| Polygons per instance | Number from Instancer's completion message. The shader divides `polNum` by this value. |
| Seed | Changes the deterministic variation. |
| Hue range +/- | Maximum hue displacement in degrees. |
| Saturation range +/- | Maximum saturation displacement in percent. |
| Value range +/- | Maximum brightness displacement in percent. |
| Colour mode | **Replace** writes the varied HSV colour. **Multiply** multiplies the incoming surface RGB by it. |

The shader has no memory-saving behaviour. The renderer receives the complete
copied mesh produced by Instancer; InstanceColor only changes the colour used
while shading it.
