# Amiberry test harness

Runs the release package and plugins in a real AmigaOS guest without manual
input. It expects an Amiberry LightWave configuration whose boot drive runs
`LWTest:Startup` when a volume named `LWTest` is mounted, from the end of
`S:User-Startup`:

```
Assign >NIL: LWTest: EXISTS
If NOT WARN
  Execute LWTest:Startup
EndIf
```

Without that volume the hook does nothing.

## Files

| File | Purpose |
|---|---|
| `run.ps1` | Starts Amiberry with a config (`-f <config> -G`), screenshots the window every few seconds, waits for a marker line in a log on the `LWTest` volume, then stops the instance it started. Refuses to run while another Amiberry is open. |
| `Startup` | `LWTest:Startup`: logs `start` and runs `LWTest:Later` in the background, so Workbench can load first. |
| `installer-smoke.later` | Copy to `LWTest:Later`. Starts `zzpoke`, then the Installer on `LWTest:LW-Plugins/Install`, and logs `installer rc=<n>`. |
| `installer-smoke.poke` | Copy to `LWTest:poke`. Clicks through the OS 3.1 Installer (43.3) on a 1280x1024 Workbench and installs the 68040 FPU build (with GEOimport) plus docs. |
| `zzpoke.c` | Sends real `input.device` mouse/key events to a window found by title prefix. |
| `rtgmode.c` | Lists `uaegfx` (`UAE:`) modes and writes `ENVARC:`/`ENV:` `lwRTG.cfg` (LightwaveRTG screen mode) and, with `WB`, `screenmode.prefs`. |

`lwRTG.cfg` is 10 bytes, big-endian: UWORD width, UWORD height, UWORD depth,
ULONG DisplayID. P96 32-bit BGRA modes report depth 24 and do not set
`DIPF_IS_FOREIGN`, so `rtgmode` matches them by the `UAE:` name prefix.

## Build the guest helpers

```bash
docker run --rm -v "$PWD":/work -w /work sacredbanana/amiga-compiler:m68k-amigaos sh -c \
  'm68k-amigaos-gcc -noixemul -m68020 -O2 -Wall -o LWTest/zzpoke tools/amiberry/zzpoke.c &&
   m68k-amigaos-gcc -noixemul -m68020 -O2 -Wall -o LWTest/rtgmode tools/amiberry/rtgmode.c'
```

## Installer smoke test

1. `./build.sh package`, then extract `build/LW-Plugins-<version>.lha` into
   the `LWTest` directory.
2. Put the Installer binary (`Installer` from the OS 3.1 Install disk) in the
   `LWTest` directory, plus `zzpoke`, `Startup`, `Later` and `poke` from above.
3. Make a copy of the LightWave config that also mounts the directory:
   `filesystem2=rw,LWTEST:LWTest:<dir>,-128` and
   `uaehf2=dir,rw,LWTEST:LWTest:<dir>,-128`.
4. Run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools/amiberry/run.ps1 `
     -Config <test.uae> -Log <dir>\log -Marker "installer rc" `
     -TimeoutSec 120 -ShotDir <shots> -ShotEverySec 2 -AfterMarkerSec 2
   ```

5. Expect `installer rc=0` in `<dir>\log`, the Layout plug-ins drawer to hold
   copies byte-identical to `build-040fpu/*.p` (all but `geoimport.p`), and
   the Modeler plug-ins drawer to hold `build-040fpu/geoimport.p`.

Write every AmigaDOS script with LF line endings.
