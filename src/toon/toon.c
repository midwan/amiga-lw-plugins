/*
 * TOON.C -- Toon / Cel-Shading Image Filter for LightWave 3D
 *
 * Post-render filter that converts any LightWave render into a
 * cartoon/cel-shaded look with configurable colour quantisation
 * and ink outlines.
 *
 * Outlines are detected via Sobel-like gradient operators on the
 * depth buffer and luminance, catching both silhouette edges and
 * material/shadow boundaries.
 *
 * Uses ImageFilterHandler for full frame-buffer access.
 *
 * Uses AllocMem/FreeMem and custom helpers -- no libnix runtime.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

#include <string.h>

#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;

/* ----------------------------------------------------------------
 * Memory helpers
 * ---------------------------------------------------------------- */

static void *
plugin_alloc(unsigned long size)
{
	unsigned long *p;
	p = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!p) return 0;
	*p = size + 4;
	return (void *)(p + 1);
}

static void
plugin_free(void *ptr)
{
	unsigned long *p;
	if (!ptr) return;
	p = ((unsigned long *)ptr) - 1;
	FreeMem(p, *p);
}

/* ----------------------------------------------------------------
 * Integer/string helpers
 * ---------------------------------------------------------------- */

static void
int_to_str(int val, char *buf, int buflen)
{
	char tmp[12];
	int  i = 0, neg = 0, len;

	if (val < 0) { neg = 1; val = -val; }
	if (val == 0) { tmp[i++] = '0'; }
	else {
		while (val > 0 && i < 11) {
			tmp[i++] = (char)('0' + (val % 10));
			val /= 10;
		}
	}
	len = neg + i;
	if (len >= buflen) len = buflen - 1;
	if (neg) buf[0] = '-';
	{
		int j;
		for (j = 0; j < i && (neg + j) < buflen - 1; j++)
			buf[neg + j] = tmp[i - 1 - j];
	}
	buf[len] = '\0';
}

static const char *
parse_int(const char *s, int *val)
{
	int neg = 0;
	*val = 0;
	while (*s == ' ') s++;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') {
		*val = *val * 10 + (*s - '0');
		s++;
	}
	if (neg) *val = -*val;
	return s;
}

static void
append_int(char *buf, int *pos, int val)
{
	char tmp[12];
	int i;
	int_to_str(val, tmp, 12);
	if (*pos > 0) buf[(*pos)++] = ' ';
	for (i = 0; tmp[i]; i++)
		buf[(*pos)++] = tmp[i];
	buf[*pos] = '\0';
}

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

#define IABS(x)  ((x) < 0 ? -(x) : (x))

/* Quantise a 0-255 value into N equal bands */
static int
quantise(int val, int bands)
{
	int step, q;
	if (bands <= 1) return val;
	step = 256 / bands;
	if (step < 1) step = 1;
	q = (val / step) * (255 / (bands - 1));
	if (q > 255) q = 255;
	return q;
}

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

#define TOON_OBJECT_VERSION  1

typedef struct {
	int   bands;            /* colour quantisation levels  (2 - 8) */
	int   edgeThreshold;    /* depth edge sensitivity      (1 - 100) */
	int   colorEdgeThresh;  /* luminance edge sensitivity  (1 - 100) */
	int   outlineR, outlineG, outlineB;  /* outline colour (0 - 255) */
	int   outlineWidth;     /* 1 = thin, 2 = thick */
	int   enableQuant;      /* 1 = quantise colours */
	int   enableOutline;    /* 1 = draw outlines */
} ToonInst;

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static MessageFuncs *msg;

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err)
{
	ToonInst *inst;
	XCALL_INIT;

	inst = (ToonInst *)plugin_alloc(sizeof(ToonInst));
	if (!inst) return 0;

	inst->bands          = 4;
	inst->edgeThreshold  = 15;
	inst->colorEdgeThresh = 30;
	inst->outlineR       = 0;
	inst->outlineG       = 0;
	inst->outlineB       = 0;
	inst->outlineWidth   = 1;
	inst->enableQuant    = 1;
	inst->enableOutline  = 1;

	return inst;
}

XCALL_(static void)
Destroy(ToonInst *inst)
{
	XCALL_INIT;
	if (inst) plugin_free(inst);
}

XCALL_(static LWError)
Copy(ToonInst *from, ToonInst *to)
{
	XCALL_INIT;
	*to = *from;
	return 0;
}

XCALL_(static LWError)
Load(ToonInst *inst, const LWLoadState *ls)
{
	char buf[128];
	const char *p;
	int v;
	XCALL_INIT;

	if (ls->ioMode == LWIO_SCENE) {
		if (!spi_read_line(ls, buf, sizeof(buf)) || !buf[0])
			return 0;
		p = buf;
		p = parse_int(p, &v);
		if (v != TOON_OBJECT_VERSION) return 0;
		p = parse_int(p, &inst->bands);
		p = parse_int(p, &inst->edgeThreshold);
		p = parse_int(p, &inst->colorEdgeThresh);
		p = parse_int(p, &inst->outlineR);
		p = parse_int(p, &inst->outlineG);
		p = parse_int(p, &inst->outlineB);
		p = parse_int(p, &inst->outlineWidth);
		p = parse_int(p, &inst->enableQuant);
		p = parse_int(p, &inst->enableOutline);
	} else {
		if (!spi_read_i32be(ls, &v) || v != TOON_OBJECT_VERSION)
			return 0;
		if (!spi_read_i32be(ls, &inst->bands)) return 0;
		if (!spi_read_i32be(ls, &inst->edgeThreshold)) return 0;
		if (!spi_read_i32be(ls, &inst->colorEdgeThresh)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineR)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineG)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineB)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineWidth)) return 0;
		if (!spi_read_i32be(ls, &inst->enableQuant)) return 0;
		if (!spi_read_i32be(ls, &inst->enableOutline)) return 0;
	}

	if (inst->bands < 2) inst->bands = 2;
	if (inst->bands > 8) inst->bands = 8;
	if (inst->edgeThreshold < 1) inst->edgeThreshold = 1;
	if (inst->edgeThreshold > 100) inst->edgeThreshold = 100;

	return 0;
}

XCALL_(static LWError)
Save(ToonInst *inst, const LWSaveState *ss)
{
	char buf[128];
	int pos = 0;
	XCALL_INIT;

	append_int(buf, &pos, TOON_OBJECT_VERSION);
	append_int(buf, &pos, inst->bands);
	append_int(buf, &pos, inst->edgeThreshold);
	append_int(buf, &pos, inst->colorEdgeThresh);
	append_int(buf, &pos, inst->outlineR);
	append_int(buf, &pos, inst->outlineG);
	append_int(buf, &pos, inst->outlineB);
	append_int(buf, &pos, inst->outlineWidth);
	append_int(buf, &pos, inst->enableQuant);
	append_int(buf, &pos, inst->enableOutline);

	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
	} else {
		spi_write_i32be(ss, TOON_OBJECT_VERSION);
		spi_write_i32be(ss, inst->bands);
		spi_write_i32be(ss, inst->edgeThreshold);
		spi_write_i32be(ss, inst->colorEdgeThresh);
		spi_write_i32be(ss, inst->outlineR);
		spi_write_i32be(ss, inst->outlineG);
		spi_write_i32be(ss, inst->outlineB);
		spi_write_i32be(ss, inst->outlineWidth);
		spi_write_i32be(ss, inst->enableQuant);
		spi_write_i32be(ss, inst->enableOutline);
	}
	return 0;
}

/* ----------------------------------------------------------------
 * Process — toon-shade the rendered frame
 * ---------------------------------------------------------------- */

XCALL_(static void)
Process(ToonInst *inst, const FilterAccess *fa)
{
	int x, y;
	int w = fa->width;
	int h = fa->height;
	int bands = inst->bands;
	int doQuant = inst->enableQuant;
	int doEdge  = inst->enableOutline;
	int depthThresh = inst->edgeThreshold;
	int colorThresh = inst->colorEdgeThresh;
	int thick = inst->outlineWidth;

	/* Allocate an edge map so we can do a thick pass if needed.
	 * Each byte: 0 = no edge, 1 = edge.
	 * We process one scanline at a time keeping 3 depth + 3 luma
	 * lines in registers via bufLine. */
	unsigned char *edgeMap = 0;

	XCALL_INIT;

	if (doEdge && thick >= 2) {
		edgeMap = (unsigned char *)plugin_alloc((unsigned long)w * (unsigned long)h);
		if (edgeMap)
			memset(edgeMap, 0, (unsigned long)w * (unsigned long)h);
	}

	for (y = 0; y < h; y++) {
		BufferValue *rLine = (*fa->bufLine)(LWBUF_RED, y);
		BufferValue *gLine = (*fa->bufLine)(LWBUF_GREEN, y);
		BufferValue *bLine = (*fa->bufLine)(LWBUF_BLUE, y);

		/* Depth lines for edge detection */
		BufferValue *dC = (*fa->bufLine)(LWBUF_DEPTH, y);
		BufferValue *dU = (y > 0) ? (*fa->bufLine)(LWBUF_DEPTH, y - 1) : dC;
		BufferValue *dD = (y < h - 1) ? (*fa->bufLine)(LWBUF_DEPTH, y + 1) : dC;

		/* Colour lines for vertical edge detection (hoisted from x-loop) */
		BufferValue *rUp = (y > 0) ? (*fa->bufLine)(LWBUF_RED, y-1) : rLine;
		BufferValue *gUp = (y > 0) ? (*fa->bufLine)(LWBUF_GREEN, y-1) : gLine;
		BufferValue *bUp = (y > 0) ? (*fa->bufLine)(LWBUF_BLUE, y-1) : bLine;
		BufferValue *rDn = (y < h-1) ? (*fa->bufLine)(LWBUF_RED, y+1) : rLine;
		BufferValue *gDn = (y < h-1) ? (*fa->bufLine)(LWBUF_GREEN, y+1) : gLine;
		BufferValue *bDn = (y < h-1) ? (*fa->bufLine)(LWBUF_BLUE, y+1) : bLine;

		if (!rLine || !gLine || !bLine) continue;

		for (x = 0; x < w; x++) {
			BufferValue rgb[3];
			int isEdge = 0;
			int r, g, b;

			/* --- Edge detection --- */
			if (doEdge) {
				/* Depth gradient (Sobel-like) */
				if (dC) {
					int d  = (int)dC[x];
					int dL = (x > 0)   ? (int)dC[x - 1] : d;
					int dR = (x < w-1) ? (int)dC[x + 1] : d;
					int dUp = (int)dU[x];
					int dDn = (int)dD[x];
					int gx = IABS(dR - dL);
					int gy = IABS(dDn - dUp);
					if (gx + gy > depthThresh)
						isEdge = 1;
				}

				/* Colour/luminance gradient */
				if (!isEdge && colorThresh < 100) {
					int lum = ((int)rLine[x] * 77 +
					           (int)gLine[x] * 150 +
					           (int)bLine[x] * 29) >> 8;
					int lumL = (x > 0)
						? (((int)rLine[x-1] * 77 +
						    (int)gLine[x-1] * 150 +
						    (int)bLine[x-1] * 29) >> 8)
						: lum;
					int lumR = (x < w-1)
						? (((int)rLine[x+1] * 77 +
						    (int)gLine[x+1] * 150 +
						    (int)bLine[x+1] * 29) >> 8)
						: lum;
					int lumU, lumD, cgx, cgy;

					lumU = ((int)rUp[x] * 77 + (int)gUp[x] * 150 +
					        (int)bUp[x] * 29) >> 8;
					lumD = ((int)rDn[x] * 77 + (int)gDn[x] * 150 +
					        (int)bDn[x] * 29) >> 8;

					cgx = IABS(lumR - lumL);
					cgy = IABS(lumD - lumU);
					if (cgx + cgy > colorThresh)
						isEdge = 1;
				}
			}

			/* Store in edge map for thick outline pass */
			if (edgeMap)
				edgeMap[y * w + x] = isEdge ? 1 : 0;

			/* --- Colour quantisation --- */
			r = (int)rLine[x];
			g = (int)gLine[x];
			b = (int)bLine[x];
			if (doQuant) {
				r = quantise(r, bands);
				g = quantise(g, bands);
				b = quantise(b, bands);
			}

			/* --- Draw outline (thin: 1px) --- */
			if (isEdge && !edgeMap) {
				/* Thin mode: draw directly */
				rgb[0] = (BufferValue)inst->outlineR;
				rgb[1] = (BufferValue)inst->outlineG;
				rgb[2] = (BufferValue)inst->outlineB;
			} else if (!edgeMap) {
				rgb[0] = (BufferValue)r;
				rgb[1] = (BufferValue)g;
				rgb[2] = (BufferValue)b;
			} else {
				/* Will do the thick pass below */
				rgb[0] = (BufferValue)r;
				rgb[1] = (BufferValue)g;
				rgb[2] = (BufferValue)b;
			}

			(*fa->setRGB)(x, y, rgb);
		}
	}

	/* --- Thick outline pass: dilate the edge map --- */
	if (edgeMap) {
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				int edge = 0;
				int dx, dy;
				/* Check NxN neighbourhood */
				for (dy = -(thick/2); dy <= thick/2 && !edge; dy++) {
					int ny = y + dy;
					if (ny < 0 || ny >= h) continue;
					for (dx = -(thick/2); dx <= thick/2 && !edge; dx++) {
						int nx = x + dx;
						if (nx < 0 || nx >= w) continue;
						if (edgeMap[ny * w + nx])
							edge = 1;
					}
				}
				if (edge) {
					BufferValue rgb[3];
					rgb[0] = (BufferValue)inst->outlineR;
					rgb[1] = (BufferValue)inst->outlineG;
					rgb[2] = (BufferValue)inst->outlineB;
					(*fa->setRGB)(x, y, rgb);
				}
			}
		}
		plugin_free(edgeMap);
	}
}

XCALL_(static unsigned int)
Flags(ToonInst *inst)
{
	XCALL_INIT;
	return 0;
}

/* ----------------------------------------------------------------
 * Description line
 * ---------------------------------------------------------------- */

static char descBuf[48];

XCALL_(static const char *)
DescLn(ToonInst *inst)
{
	int i = 0;
	char nb[12];
	XCALL_INIT;

	descBuf[0] = '\0';
	if (inst->enableQuant) {
		int_to_str(inst->bands, nb, 12);
		strcat(descBuf, nb);
		strcat(descBuf, " bands");
		i = 1;
	}
	if (inst->enableOutline) {
		if (i) strcat(descBuf, " + ");
		strcat(descBuf, "outline");
	}
	if (!descBuf[0])
		return "Toon (disabled)";
	return descBuf;
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static const char *widthItems[] = { "Thin (1px)", "Thick (2px)", "Heavy (3px)", 0 };

XCALL_(static int)
Interface(
	long         version,
	GlobalFunc  *global,
	ToonInst    *inst,
	void        *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl    *ctlBands, *ctlDepthThr, *ctlColorThr;
	LWControl    *ctlOutR, *ctlOutG, *ctlOutB, *ctlWidth;
	LWControl    *ctlEnQuant, *ctlEnOutline;

	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;

	msg  = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);

	if (!panl) {
		if (msg)
			(*msg->info)("Toon", "Cel-shading image filter");
		return AFUNC_OK;
	}

	{
		static LWPanControlDesc desc;
		static LWValue ival = {LWT_INTEGER};

		pan = PAN_CREATE(panl, "Toon v" PLUGIN_VERSION
		                       " (c) D. Panokostas");
		if (!pan) return AFUNC_OK;

		ctlEnQuant   = BOOL_CTL(panl, pan, "Quantise Colours");
		ctlBands     = SLIDER_CTL(panl, pan, "Bands", 150, 2, 8);
		ctlEnOutline = BOOL_CTL(panl, pan, "Draw Outlines");
		ctlDepthThr  = SLIDER_CTL(panl, pan, "Depth Edge", 150, 1, 100);
		ctlColorThr  = SLIDER_CTL(panl, pan, "Color Edge", 150, 1, 100);
		ctlWidth     = POPUP_CTL(panl, pan, "Outline Width", widthItems);
		ctlOutR      = SLIDER_CTL(panl, pan, "Outline R", 150, 0, 255);
		ctlOutG      = SLIDER_CTL(panl, pan, "Outline G", 150, 0, 255);
		ctlOutB      = SLIDER_CTL(panl, pan, "Outline B", 150, 0, 255);

		SET_INT(ctlEnQuant, inst->enableQuant);
		SET_INT(ctlBands, inst->bands);
		SET_INT(ctlEnOutline, inst->enableOutline);
		SET_INT(ctlDepthThr, inst->edgeThreshold);
		SET_INT(ctlColorThr, inst->colorEdgeThresh);
		SET_INT(ctlWidth, inst->outlineWidth - 1);
		SET_INT(ctlOutR, inst->outlineR);
		SET_INT(ctlOutG, inst->outlineG);
		SET_INT(ctlOutB, inst->outlineB);

		if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
			int idx;
			GET_INT(ctlEnQuant, inst->enableQuant);
			GET_INT(ctlBands, inst->bands);
			GET_INT(ctlEnOutline, inst->enableOutline);
			GET_INT(ctlDepthThr, inst->edgeThreshold);
			GET_INT(ctlColorThr, inst->colorEdgeThresh);
			GET_INT(ctlWidth, idx);
			inst->outlineWidth = idx + 1;
			GET_INT(ctlOutR, inst->outlineR);
			GET_INT(ctlOutG, inst->outlineG);
			GET_INT(ctlOutB, inst->outlineB);

			/* Clamp */
			if (inst->bands < 2) inst->bands = 2;
			if (inst->bands > 8) inst->bands = 8;
			if (inst->edgeThreshold < 1) inst->edgeThreshold = 1;
			if (inst->edgeThreshold > 100) inst->edgeThreshold = 100;
			if (inst->colorEdgeThresh < 1) inst->colorEdgeThresh = 1;
			if (inst->colorEdgeThresh > 100) inst->colorEdgeThresh = 100;
			if (inst->outlineWidth < 1) inst->outlineWidth = 1;
			if (inst->outlineWidth > 3) inst->outlineWidth = 3;
			if (inst->outlineR < 0) inst->outlineR = 0;
			if (inst->outlineR > 255) inst->outlineR = 255;
			if (inst->outlineG < 0) inst->outlineG = 0;
			if (inst->outlineG > 255) inst->outlineG = 255;
			if (inst->outlineB < 0) inst->outlineB = 0;
			if (inst->outlineB > 255) inst->outlineB = 255;
		}

		PAN_KILL(panl, pan);
	}

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Activation
 * ---------------------------------------------------------------- */

XCALL_(int)
Activate(
	long         version,
	GlobalFunc  *global,
	void        *local,
	void        *serverData)
{
	ImageFilterHandler *h = (ImageFilterHandler *)local;
	XCALL_INIT;

	if (version < 1)
		return AFUNC_BADVERSION;

	h->create   = (void *)Create;
	h->destroy  = (void *)Destroy;
	h->load     = (void *)Load;
	h->save     = (void *)Save;
	h->copy     = (void *)Copy;
	h->process  = (void *)Process;
	h->flags    = (void *)Flags;
	h->descln   = (void *)DescLn;
	h->useItems = 0;
	h->changeID = 0;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!msg)
		return AFUNC_BADGLOBAL;

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Server description
 * ---------------------------------------------------------------- */

ServerRecord ServerDesc[] = {
	{ "ImageFilterHandler",   "Toon",
	  (ActivateFunc *)Activate },
	{ "ImageFilterInterface", "Toon",
	  (ActivateFunc *)Interface },
	{ 0 }
};
