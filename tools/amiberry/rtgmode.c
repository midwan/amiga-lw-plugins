/* rtgmode WIDTH HEIGHT DEPTH [WB]
 * Lists uaegfx ("UAE:") modes; picks the one matching WIDTH x HEIGHT with
 * MaxDepth == DEPTH (32-bit BGRA modes report 24), writes ENVARC:/ENV: lwRTG.cfg (LightwaveRTG screen
 * prefs: UWORD w,h,depth + ULONG DisplayID) and, with WB, ENVARC:/ENV: Sys/
 * screenmode.prefs for Workbench. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <prefs/prefhdr.h>
#include <prefs/screenmode.h>
#include <proto/graphics.h>
#include <proto/dos.h>

static int write_file(const char *path, const void *buf, LONG len)
{
	BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
	LONG n;
	if (!fh) return 0;
	n = Write(fh, (APTR)buf, len);
	Close(fh);
	return n == len;
}

int main(int argc, char **argv)
{
	ULONG id = INVALID_ID, pick = INVALID_ID;
	struct NameInfo name;
	struct DimensionInfo dims;
	struct DisplayInfo disp;
	int w, h, d, wb;
	if (argc < 4) { printf("usage: rtgmode W H D [WB]\n"); return 10; }
	w = atoi(argv[1]); h = atoi(argv[2]); d = atoi(argv[3]);
	wb = argc > 4 && !strcmp(argv[4], "WB");
	while ((id = NextDisplayInfo(id)) != INVALID_ID) {
		int mw, mh, md, foreign;
		if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, id)) continue;
		if (!GetDisplayInfoData(NULL, (UBYTE *)&disp, sizeof disp, DTAG_DISP, id)) continue;
		if (!GetDisplayInfoData(NULL, (UBYTE *)&name, sizeof name, DTAG_NAME, id)) name.Name[0] = 0;
		mw = dims.Nominal.MaxX - dims.Nominal.MinX + 1;
		mh = dims.Nominal.MaxY - dims.Nominal.MinY + 1;
		md = dims.MaxDepth;
		foreign = !strncmp((char *)name.Name, "UAE:", 4); /* P96 does not set DIPF_IS_FOREIGN */
		if (!foreign) continue;
		printf("0x%08lx %4dx%-4d d%-2d %s\n", (unsigned long)id, mw, mh, md, name.Name);
		if (mw == w && mh == h && md == d && pick == INVALID_ID) pick = id;
	}
	if (pick == INVALID_ID) { printf("no RTG mode %dx%dx%d\n", w, h, d); return 10; }
	printf("picked 0x%08lx\n", (unsigned long)pick);
	{
		UBYTE cfg[10];
		cfg[0] = w >> 8; cfg[1] = w; cfg[2] = h >> 8; cfg[3] = h; cfg[4] = 0; cfg[5] = d;
		cfg[6] = pick >> 24; cfg[7] = pick >> 16; cfg[8] = pick >> 8; cfg[9] = pick;
		if (!write_file("ENVARC:lwRTG.cfg", cfg, 10) || !write_file("ENV:lwRTG.cfg", cfg, 10)) {
			printf("writing lwRTG.cfg failed\n"); return 10;
		}
		printf("wrote lwRTG.cfg\n");
	}
	if (wb) {
		/* FORM PREF { PRHD(6) SCRM(28) } */
		static UBYTE iff[12 + 8 + 6 + 8 + 28];
		UBYTE *p = iff;
		struct ScreenModePrefs smp;
		memset(iff, 0, sizeof iff);
		memcpy(p, "FORM", 4); p[7] = sizeof iff - 8; memcpy(p + 8, "PREF", 4); p += 12;
		memcpy(p, "PRHD", 4); p[7] = 6; p += 8 + 6;
		memcpy(p, "SCRM", 4); p[7] = sizeof smp; p += 8;
		memset(&smp, 0, sizeof smp);
		smp.smp_DisplayID = pick; smp.smp_Width = w; smp.smp_Height = h;
		smp.smp_Depth = d; smp.smp_Control = SMF_AUTOSCROLL;
		memcpy(p, &smp, sizeof smp);
		if (!write_file("ENVARC:Sys/screenmode.prefs", iff, sizeof iff) ||
		    !write_file("ENV:Sys/screenmode.prefs", iff, sizeof iff)) {
			printf("writing screenmode.prefs failed\n"); return 10;
		}
		printf("wrote screenmode.prefs\n");
	}
	return 0;
}
