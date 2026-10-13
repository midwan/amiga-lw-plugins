# Agent Knowledge Base — Amiga LightWave 5.x Plugin

## Toolchain

- **Docker image**: `sacredbanana/amiga-compiler:m68k-amigaos`
- **Compiler**: `m68k-amigaos-gcc` 6.5.0b (GCC cross-compiler for m68k AmigaOS)
- **Build wrapper**: `./build.sh` runs `make` inside Docker, mounting the project at `/work`
- **Key compiler flags**: `-noixemul $(CPUFLAGS) -O2 -Wall` (`CPUFLAGS` defaults to `-m68020`, soft-float)
- **Key linker flags**: `-noixemul -nostartfiles $(CPUFLAGS)`
- **Required link libraries**: `-lm -lgcc` (math and GCC runtime)
- **FPU variants**: `make fpu` (or `020fpu`/`040fpu`/`060fpu`) builds into `build-<variant>/` with its own `sdklib/`

### FPU variant notes

- With `-m68881`, GCC returns `float`/`double` in `fp0` instead of `d0/d1`. Arguments are still
  passed on the stack, so host callbacks are ABI-compatible as long as their `double` return
  value is not used. No plugin currently consumes one (`pbr.c` ignores the `rayTrace` result).
  Verify the LightWave host convention before using a `double` return from `rayTrace`,
  `rayCast`, `rayShade`, `LWCameraInfo`, `LWFogInfo`, etc. in an FPU build.
- The libnix driver links all three variants against `libm020/libm881`. Its `sin`/`sqrt` call
  `mathieeedoubtrans.library` and return in `fp0`, so `slib4.c` must still open the math
  libraries in FPU builds (the `motion` plugin needs this).
- `020fpu` emits `fmovecr`/`fintrz` (68881/68882 only; trapped and emulated on 040/060).
  `040fpu`/`060fpu` emit `fd*`/`fs*` (040/060 FPU only). `060fpu` may emit `fintrz`.
- FPU code saves `fp2`–`fp7` (12 bytes each) in function prologues; keep this in mind for the
  callback stack budget below.

### Docker container paths

| Path | Contents |
|---|---|
| `/opt/amiga/bin/` | Cross-tools (`m68k-amigaos-gcc`, `ar`, `as`, `ld`, etc.) |
| `/opt/amiga/m68k-amigaos/ndk-include/` | AmigaOS NDK headers (`exec/`, `dos/`, `intuition/`, etc.) |
| `/opt/amiga/m68k-amigaos/lib/` | System libraries (`libamiga.a`, `libc.a`, `libm.a`) |
| `/opt/amiga/m68k-amigaos/libnix/` | libnix C runtime (standalone, no ixemul.library) |

### GCC compatibility notes

- GCC defines `__AMIGA__`, `__amigaos__`, `__amiga__` — NOT `_AMIGA` (SAS/C) or `AZTEC_C`
- `XCALL_(t)` and `XCALL_INIT` are defined as no-ops for GCC in `plug.h`
- SAS/C object files (`.o`, `.lib`) are NOT link-compatible with GCC — everything must be rebuilt from source
- `stubs.c` provides a dummy `exit()` because libnix references it but plugins never call it

### CRITICAL: No libnix runtime functions

**Problem (solved)**: Using `-nostartfiles` skips libnix's C startup, which initializes the
heap, stdio, ctype tables, and other runtime state. The following libnix functions are
**BROKEN** in our plugin environment and must NOT be used:

- **`malloc` / `free` / `realloc`** — heap not initialized → hangs or corrupts memory
- **`sprintf` / `printf` / `fprintf`** — stdio not initialized
- **`qsort`** — may internally use malloc
- **`strncasecmp` / `strcasecmp`** — needs ctype/locale tables → address error crash
- **Any function requiring locale, stdio, or heap**

**Safe alternatives**:
- Memory: `AllocMem`/`FreeMem` via exec.library (SysBase opened in `_Startup`)
- Use a `plugin_alloc`/`plugin_free` wrapper that stores size before the returned pointer:
  ```c
  static void *plugin_alloc(unsigned long size) {
      unsigned long *p = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
      if (!p) return 0;
      *p = size + 4;
      return (void *)(p + 1);
  }
  static void plugin_free(void *ptr) {
      unsigned long *p;
      if (!ptr) return;
      p = ((unsigned long *)ptr) - 1;
      FreeMem(p, *p);
  }
  ```
- Strings: `strcpy`/`strncpy`/`strcat`/`strncat`/`strlen`/`memcpy`/`memset` are safe (inline/no state)
- Case-insensitive compare: use custom `ci_strncmp` instead of `strncasecmp`
- Integer to string: use custom `int_to_str` instead of `sprintf`
- Sorting: use custom sort (bubble/insertion with byte-swap) instead of `qsort`

### CRITICAL: Minimize stack usage in plugin callbacks

LightWave plugin callbacks may run with limited stack. Large local arrays cause
address error crashes (guru $80000003). Rules:
- **Never** put large buffers on the stack (>64 bytes)
- Write directly into `plugin_alloc`'d buffers or struct fields instead
- Use byte-level swap for sorting structs (don't copy whole struct to stack local)
- Total stack usage per callback should stay under ~200 bytes

### CRITICAL: Math library initialization

**Problem (solved)**: Using `-nostartfiles` skips libnix's C startup, which normally opens
`mathieeedoubbas.library` and `mathieeedoubtrans.library`. GCC's soft-float math stubs
(`__adddf3`, `__muldf3`, `__divdf3`, `sqrt`, etc.) call through `MathIeeeDoubBasBase` and
`MathIeeeDoubTransBase` library base pointers. Without initialization, these contain garbage
→ address error (guru $80000003) on first floating-point operation.

**Fix**: `slib4.c` `_Startup()` now opens both math libraries via weak references, and
`_Shutdown()` closes them. Plugins that don't use floating-point won't pull in these symbols
and are unaffected.

**Verified**: spikey.p (uses `sqrt`, multiply, divide on doubles) runs correctly in LightWave
Modeler on real AmigaOS after this fix.

---

### Handler version numbers (LW 5.x)

LightWave 5.x sends these version numbers to Activate:

| Handler class | Version sent |
|---|---|
| `ObjReplacementHandler` | 2 |
| `ObjReplacementInterface` | 1 |
| `ImageFilterHandler` | 1 (assumed, negative sample checks `!=1`) |

Accept version >= 1 and fill the V1 fields. For version 2, the extra V2 fields
(descln, useItems, changeID) appear to be pre-zeroed by LW and can be left unfilled.

---

## LightWave 5.x Plugin Architecture

### Binary format

Plugins are AmigaOS `LoadSeg()`-able executables (`.p` files) with a specific header:

```
moveq  #0,d0           ; Return 0 if run from CLI
rts
dc.l   $04121994        ; Magic number
dc.l   $2               ; Flags
dc.l   $100             ; Version
dc.l   _Startup         ; Pointer to startup function
dc.l   _Shutdown        ; Pointer to shutdown function
dc.l   ServerDesc       ; Pointer to server description array
```

This header is provided by `sdk/source/serv_gcc.s` (assembled to `sdk/lib/serv_gcc.o`).

### Plugin lifecycle

1. LightWave calls `_Startup()` when the plugin is first loaded → returns `serverData` (non-null = success)
2. LightWave reads `ServerDesc[]` to discover server class/name/activate entries
3. For each server, LightWave calls the `activate` function with `(version, global, local, serverData)`
4. LightWave calls `_Shutdown(serverData)` before unloading the plugin

### Single-server vs multi-server plugins

**Single-server**: Export `ServerClass[]`, `ServerName[]`, and `Activate()`. The default `slib3.c` constructs the `ServerDesc[]` automatically.

```c
char ServerClass[] = "MeshDataEdit";
char ServerName[]  = "MyPlugin";

XCALL_(int) Activate(long version, GlobalFunc *global, void *local, void *serverData) {
    XCALL_INIT;
    if (version != 1) return AFUNC_BADVERSION;
    /* ... */
    return AFUNC_OK;
}
```

**Multi-server**: Export `ServerDesc[]` directly. Must provide custom `Startup()` and `Shutdown()` (or use defaults from `slib1.c`/`slib2.c`).

```c
ServerRecord ServerDesc[] = {
    { "CommandSequence", "MyCmd1", Cmd1Activate },
    { "CommandSequence", "MyCmd2", Cmd2Activate },
    { NULL }
};
```

### Activation function

```c
typedef int ActivateFunc(
    long        version,      // Interface version (check this!)
    GlobalFunc *global,       // Access to host globals
    void       *local,        // Class-specific data (cast to handler struct)
    void       *serverData    // From Startup()
);

// Return codes:
#define AFUNC_OK          0   // Success
#define AFUNC_BADVERSION  1   // Version mismatch
#define AFUNC_BADGLOBAL   2   // Required global not available
#define AFUNC_BADLOCAL    3   // Problem with local data
```

### Global function

```c
typedef void *GlobalFunc(const char *id, int useCode);

#define GFUSE_TRANSIENT  0  // Data used only during activation
#define GFUSE_ACQUIRE    1  // Data kept after activation (must RELEASE later)
#define GFUSE_RELEASE    2  // Release previously acquired data
```

Common global IDs:
- `"Info Messages"` → `MessageFuncs *` (info/error/warning dialogs)
- `"Host Display Info"` → `HostDisplayInfo *` (Amiga Screen/Window pointers)
- `"LWM: Dynamic Monitor"` → `DynaMonitorFuncs *` (progress bars)
- `"LWM: Dynamic Request"` → `DynaReqFuncs *` (dialog requesters)
- `"LWM: State Query"` → `StateQueryFuncs *` (layer/surface state)

### Instance handlers (Layout plugins)

Layout plugins (shaders, filters, displacement, motion) use an instance pattern:

```c
typedef struct LWInstHandler {
    LWInstance (*create)(LWError *);
    void      (*destroy)(LWInstance);
    LWError   (*copy)(LWInstance from, LWInstance to);
    LWError   (*load)(LWInstance, const LWLoadState *);
    LWError   (*save)(LWInstance, const LWSaveState *);
} LWInstHandler;
```

Each handler type embeds `LWInstHandler inst` plus class-specific callbacks.

---

## Server Classes Reference

### Layout (Animation) — header: `lwran.h`

| Class | Local data | Purpose |
|---|---|---|
| `ShaderHandler` | `ShaderHandler *` | Procedural surface textures |
| `ShaderInterface` | instance pointer | UI for shader (same name as handler) |
| `ImageFilterHandler` | `ImageFilterHandler *` | Post-render image processing |
| `DisplacementHandler` | `DisplacementHandler *` | Procedural displacement maps |
| `ItemMotionHandler` | `ItemMotionHandler *` | Procedural item animation |
| `ObjReplacementHandler` | `ObjReplacementHandler *` | Object replacement per frame |
| `FrameBufferHandler` | `FrameBufferHandler *` | Custom frame buffer display |
| `AnimSaverHandler` | `AnimSaverHandler *` | Animation file output |
| `SceneConverter` | `SceneConverter *` | Scene file format conversion |

### Modeler — header: `lwmod.h`

| Class | Local data | Purpose |
|---|---|---|
| `MeshDataEdit` | `MeshEditBegin *` | Direct mesh point/polygon editing |
| `CommandSequence` | `ModCommand *` | Execute modeler commands programmatically |

### Common — headers: `lwbase.h`, `image.h`

| Class | Local data | Purpose |
|---|---|---|
| `ObjectLoader` | `ObjectImport *` | Load 3D object file formats |
| `ImageLoader` | `ImLoaderLocal *` | Load image file formats |
| `ImageSaver` | `ImSaverLocal *` | Save image file formats |
| `Global` | `GlobalService *` | Provide shared data to other plugins |

### File requester — header: `freq.h`

| Class | Local data | Purpose |
|---|---|---|
| `FileRequester` | `FileReq_Local *` | Custom file selection dialog |

---

## Shader (procedural texture) details

The `ShaderAccess` struct provides per-pixel data during rendering:

**Read-only geometry**:
- `oPos[3]` — Object-space position
- `wPos[3]` — World-space position
- `gNorm[3]`, `wNorm[3]` — Geometric/world normal
- `oXfrm[9]`, `wXfrm[9]` — Object/world transform matrices
- `raySource[3]`, `rayLength` — Ray info
- `cosine` — Surface angle cosine
- `spotSize` — Anti-aliasing spot size
- `sx`, `sy` — Screen coordinates
- `objID` — Item ID of the object
- `polNum` — Polygon number (non-prerelease only)

**Modifiable surface properties**:
- `color[3]` — Surface color (0.0–1.0)
- `luminous`, `diffuse`, `specular`, `mirror`, `transparency` — Surface attributes
- `eta` — Index of refraction
- `roughness` — Specular roughness

**Shading functions**:
- `illuminate(light, position, direction, color)` — Query light contribution
- `rayTrace(position, direction, color)` — Cast a ray

**Flags** (from `flags()` callback): `LWSHF_COLOR`, `LWSHF_LUMINOUS`, `LWSHF_DIFFUSE`, etc. — declare which properties the shader modifies.

---

## Mesh editing details

`MeshEditBegin` is called to start an edit operation:
```c
MeshEditOp *op = (*local)(pointBufSize, polyBufSize, OPSEL_USER);
```

`MeshEditOp` provides:
- **Query**: `pointCount()`, `polyCount()`, `pointInfo()`, `polyInfo()`, `polyNormal()`
- **Scan**: `pointScan()`, `polyScan()` — iterate with callbacks
- **Create**: `addPoint()`, `addPoly()`, `addCurve()`, `addQuad()`, `addTri()`, `addPatch()`
- **Modify**: `pntMove()`, `polSurf()`, `polPnts()`, `polFlag()`, `remPoint()`, `remPoly()`
- **Complete**: `done(state, error, selectionMode)`

Layer filters: `OPLYR_PRIMARY`, `OPLYR_FG`, `OPLYR_BG`, `OPLYR_ALL`, `OPLYR_EMPTY`, `OPLYR_NONEMPTY`
Selection filters: `OPSEL_GLOBAL`, `OPSEL_USER`, `OPSEL_DIRECT`

---

## Config file registration

Plugins are registered in LightWave's config file (e.g., `MOD-config` on Amiga):

```
Plugin <class> <name> <module> <user visible name>
```

Example:
```
Plugin MeshDataEdit Demo_MakeSpikey spikey.p Spikey Subdivide
Plugin CommandSequence Demo_AllBGLayers layerset.p Include Background
```

---

## LWPanel UI System (Layout requesters)

Accessed via `"LWPanelServices"` global. Requires `lwpanels.p` plugin installed.

```c
#include <lwpanel.h>

// Required globals for control macros
static LWPanControlDesc desc;
static LWValue ival={LWT_INTEGER}, ivecval={LWT_VINT},
  fval={LWT_FLOAT}, fvecval={LWT_VFLOAT}, sval={LWT_STRING};

// In Interface function:
LWPanelFuncs *panl = (*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
LWPanelID pan = PAN_CREATE(panl, "My Panel Title");

// Add controls via macros:
LWControl *c1 = INT_CTL(panl, pan, "Count");
LWControl *c2 = FLOAT_CTL(panl, pan, "Scale");
LWControl *c3 = STR_CTL(panl, pan, "Name", 40);
LWControl *c4 = RGB_CTL(panl, pan, "Color");
LWControl *c5 = BOOL_CTL(panl, pan, "Enable");
LWControl *c6 = SLIDER_CTL(panl, pan, "Amount", 200, 0, 100);
LWControl *c7 = POPUP_CTL(panl, pan, "Mode", itemList);

// Set initial values
SET_INT(c1, 5);
SET_FLOAT(c2, 1.0);

// Show panel (blocking with OK/Cancel)
if (PAN_POST(panl, pan)) {
    // User clicked OK — read values
    GET_INT(c1, myCount);
    GET_FLOAT(c2, myScale);
}
PAN_KILL(panl, pan);
```

Available control macros: `INT_CTL`, `FLOAT_CTL`, `STR_CTL`, `BOOL_CTL`, `RGB_CTL`,
`FVEC_CTL`, `IVEC_CTL`, `SLIDER_CTL`, `HSLIDER_CTL`, `VSLIDER_CTL`, `POPUP_CTL`,
`HCHOICE_CTL`, `VCHOICE_CTL`, `BUTTON_CTL`, `FILE_CTL`, `TEXT_CTL`, `AREA_CTL`,
`CANVAS_CTL`, `CUSTPOPUP_CTL`, `ITEM_CTL`, `LISTBOX_CTL`, `PERCENT_CTL`,
`TABCHOICE_CTL`, `BORDER_CTL`, `DRAGBUT_CTL`, etc.

---

## 5.x SDK Changes from 4.0

Key differences when using the 5.x SDK headers:

1. **`splug.h`**: `ServerRecord.class` renamed to `ServerRecord._class` (C++ keyword)
2. **Handler structs flattened**: `LWInstHandler inst` sub-struct removed — `create`, `destroy`, `copy`, `load`, `save` are now direct members of each handler struct. Old: `local->inst.create`, New: `local->create`
3. **New handler fields**: `descln` (description line), `useItems`/`changeID` (item dependency tracking)
4. **`_V1` compat structs**: `DisplacementHandler_V1`, `ItemMotionHandler_V1`, `ObjReplacementHandler_V1` preserve old signatures
5. **New handler types**: `PixelFilterHandler` (per-pixel filter with raycasting), `LayoutGeneric` (scene load/save), `EnvelopeHandler`
6. **New shader functions**: `rayCast()` (distance only), `rayShade()` (full shading)
7. **New light types**: `LWLIGHT_LINEAR`, `LWLIGHT_AREA`
8. **New globals**: `LWBackdropInfo`, `LWFogInfo`, `LWLightInfo.flags`/`.range`
9. **New headers**: `lwpanel.h`, `lwtypes.h`, `lwmath.h`, `lwobjacc.h`, `lwenvlp.h`, `lweval.h`, `gui_help.h`

---

## SDK source reference

Located at: `/Users/midwan/Library/CloudStorage/OneDrive-Personal/Projects/Lightwave/Documentation/`

- `LW-SDK_5.x/` — **Primary SDK** (5.x headers, includes lwpanel.h and extended APIs)
- `SDK/` — Original 4.0 SDK (sample source code, reference only)
- `LWPlugDocHTML/` — HTML documentation (Plug.HTML = architecture, LW.HTML = modeling/animation, Image.HTML = image I/O)
- `LW-SDK_5.x/doc/` — 5.x docs (lwpanels.html, objacces.html, laymon.html)
- `The.Art.of.LW.Plugins-Bman/` — PDF reference + gradient.cpp example with LWPanel UI

## PBR Shader - Feature Reference

### Save/Load field order (20 fields total)
ior*1000, reflPower, affectMirror, affectTrans, affectDiffuse, diffPower,
roughEnabled, roughAmount, aoEnabled, aoSamples, aoRadius, aoStrength,
metallic, affectSpecular, blurReflEnabled, blurReflSamples, blurReflAmount,
envEnabled, envSamples, envStrength

Note: Fresnel controls (affectMirror, affectTrans, affectDiffuse, reflPower,
diffPower) preserved in save/load for backward compat but removed from PBR UI.
Use standalone Fresnel plugin for angle-dependent effects.
Metallic is 0-100 (old scenes with metallic=1 auto-convert to 100 on load).

### Blurred Reflections (implemented)
- Reflection direction: R = V - 2*dot(V,N)*N where V = normalize(wPos - raySource)
- Casts N rays (4/8/16) via sa->rayTrace() in a cone around R
- Cone spread: blurReflAmount / 200.0 (independent of roughness), perturbed via hash3d with per-sample seeds
- Averages returned colors, blends into sa->color weighted by sa->mirror
- Sets sa->mirror = 0 to prevent LW doubling the reflection
- Instance fields: blurReflEnabled (int), blurReflSamples (int 4/8/16), blurReflAmount (int 0-100)
- Flags: LWSHF_COLOR | LWSHF_MIRROR | LWSHF_RAYTRACE

### Environment Sampling (implemented)
- Casts N rays (4/8/16) via sa->rayTrace() in hemisphere around normal using hemi_dirs[]
- Flips rays facing away from normal, cosine-weighted by dot(dir, normal)
- Averages colors, adds to sa->color scaled by envStrength/100
- Boosts sa->luminous by envStr * 0.3 for self-illumination
- Instance fields: envEnabled (int), envSamples (int 4/8/16), envStrength (int 0-100)
- Flags: LWSHF_COLOR | LWSHF_LUMINOUS | LWSHF_RAYTRACE
