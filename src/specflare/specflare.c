/*
 * SPECFLARE.C -- Specular Lens Flare Image Filter for LightWave 3D
 *
 * Post-render filter that detects bright specular highlights and
 * composites glow and star streaks over the rendered image.
 * Uses ImageFilterHandler for full frame buffer access.
 *
 * Uses AllocMem/FreeMem and custom helpers -- no libnix runtime.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

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
	unsigned long magnitude;
	int i = 0, neg = 0, len;

	if (val < 0) {
		neg = 1;
		magnitude = (unsigned long)(-(val + 1)) + 1;
	} else {
		magnitude = (unsigned long)val;
	}
	if (magnitude == 0) {
		tmp[i++] = '0';
	} else {
		while (magnitude > 0 && i < 11) {
			tmp[i++] = (char)('0' + (magnitude % 10));
			magnitude /= 10;
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
lf_parse_int(const char *s, int *val)
{
	unsigned long magnitude = 0;
	unsigned long limit;
	int neg = 0;

	while (*s == ' ') s++;
	if (*s == '-') {
		neg = 1;
		s++;
	}
	limit = neg ? 2147483648UL : 2147483647UL;
	while (*s >= '0' && *s <= '9') {
		unsigned long digit = (unsigned long)(*s - '0');
		if (magnitude > (limit - digit) / 10)
			magnitude = limit;
		else
			magnitude = magnitude * 10 + digit;
		s++;
	}
	if (neg) {
		*val = magnitude == 2147483648UL
		     ? (-2147483647 - 1) : -(int)magnitude;
	} else {
		*val = (int)magnitude;
	}
	return s;
}

static void
lf_append_int(char *buf, int *pos, int val)
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
 * Star filter directions
 * ---------------------------------------------------------------- */

#define PI 3.14159265358979323846

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

#define MAX_DETECT 250
#define MAX_FLARES 250
#define MAX_STAR_DIRECTIONS 20
#define STAR_DIRECTION_SCALE 1024
#define SPECFLARE_OBJECT_VERSION 2

typedef struct {
	int x, y;
	int brightness;
	int accumBright;
	int pixelCount;
	double depth;
	int glowRadius;
	int streakLength;
} FlareSource;

typedef struct {
	int dx, dy;
	int lengthScale;
	int intensityScale;
} StarDirection;

typedef struct {
	int    threshold;
	int    glowRadius;
	int    streakLength;
	int    intensity;
	int    starFilter;
	int    rotationAngle;
	int    spin;
	int    showRing;
	int    anamorphic;
	int    maxFlares;
	char   ioBuffer[144];
} SpecFlareInst;

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
	SpecFlareInst *inst;
	XCALL_INIT;

	inst = (SpecFlareInst *)plugin_alloc(sizeof(SpecFlareInst));
	if (!inst) return 0;

	inst->threshold = 220;
	inst->glowRadius = 100;
	inst->streakLength = 200;
	inst->intensity = 80;
	inst->starFilter = 5;
	inst->rotationAngle = 0;
	inst->spin = 0;
	inst->showRing = 1;
	inst->anamorphic = 1;
	inst->maxFlares = 8;

	return inst;
}

XCALL_(static void)
Destroy(SpecFlareInst *inst)
{
	XCALL_INIT;
	if (inst) plugin_free(inst);
}

XCALL_(static LWError)
Copy(SpecFlareInst *from, SpecFlareInst *to)
{
	XCALL_INIT;
	*to = *from;
	return 0;
}

static void
ClampSettings(SpecFlareInst *inst)
{
	if (inst->threshold < 0) inst->threshold = 0;
	if (inst->threshold > 255) inst->threshold = 255;
	if (inst->glowRadius < 1) inst->glowRadius = 1;
	if (inst->glowRadius > 200) inst->glowRadius = 200;
	if (inst->streakLength < 1) inst->streakLength = 1;
	if (inst->streakLength > 400) inst->streakLength = 400;
	if (inst->intensity < 0) inst->intensity = 0;
	if (inst->intensity > 100) inst->intensity = 100;
	if (inst->starFilter < 0) inst->starFilter = 0;
	if (inst->starFilter > 10) inst->starFilter = 10;
	if (inst->showRing) inst->showRing = 1;
	if (inst->anamorphic) inst->anamorphic = 1;
	if (inst->maxFlares < 1) inst->maxFlares = 1;
	if (inst->maxFlares > MAX_FLARES) inst->maxFlares = MAX_FLARES;
}

static int
SinHalfDegrees(long angle)
{
	long product, denom, sign = 1;

	angle %= 720;
	if (angle < 0) angle += 720;
	if (angle > 360) {
		angle -= 360;
		sign = -1;
	}
	product = angle * (360 - angle);
	denom = 162000L - product;
	if (denom <= 0) denom = 1;
	return (int)(sign * ((4096L * product) / denom));
}
static int
BuildStarDirections(const SpecFlareInst *inst, LWFrame frame,
	StarDirection *stars)
{
	int mainCount, hasSecondary, s, count = 0;
	long angle, spacing, base, rotation, spin, frameAngle;

	if (inst->starFilter == 0) return 0;
	mainCount = ((inst->starFilter + 1) / 2) * 2;
	hasSecondary = (inst->starFilter & 1) == 0;
	rotation = (long)inst->rotationAngle % 360;
	spin = (long)inst->spin % 360;
	frameAngle = (long)frame % 360;
	base = (rotation + spin * frameAngle) % 360 * 2;
	spacing = 720 / mainCount;

	for (s = 0; s < mainCount; s++) {
		angle = base + spacing * (long)s;
		stars[count].dx = SinHalfDegrees(angle + 180);
		stars[count].dy = SinHalfDegrees(angle);
		stars[count].lengthScale = STAR_DIRECTION_SCALE;
		stars[count].intensityScale = STAR_DIRECTION_SCALE;
		count++;
	}
	if (hasSecondary) {
		for (s = 0; s < mainCount; s++) {
			angle = base + spacing * (long)s + spacing / 2;
			stars[count].dx = SinHalfDegrees(angle + 180);
			stars[count].dy = SinHalfDegrees(angle);
			stars[count].lengthScale = STAR_DIRECTION_SCALE / 2;
			stars[count].intensityScale = STAR_DIRECTION_SCALE / 2;
			count++;
		}
	}

	return count;
}

XCALL_(static LWError)
Load(SpecFlareInst *inst, const LWLoadState *ls)
{
	const char *p;
	int v, threshold, glowRadius, streakLength, intensity, starFilter;
	int rotationAngle, spin, showRing, anamorphic, maxFlares;
	XCALL_INIT;

	if (ls->ioMode == LWIO_SCENE) {
		if (!spi_read_line(ls, inst->ioBuffer, sizeof(inst->ioBuffer))
			|| !inst->ioBuffer[0])
			return 0;

		p = inst->ioBuffer;
		p = lf_parse_int(p, &v);
		if (v != SPECFLARE_OBJECT_VERSION) return 0;
		p = lf_parse_int(p, &inst->threshold);
		p = lf_parse_int(p, &inst->glowRadius);
		p = lf_parse_int(p, &inst->streakLength);
		p = lf_parse_int(p, &inst->intensity);
		p = lf_parse_int(p, &inst->starFilter);
		p = lf_parse_int(p, &inst->rotationAngle);
		p = lf_parse_int(p, &inst->spin);
		p = lf_parse_int(p, &inst->showRing);
		p = lf_parse_int(p, &inst->anamorphic);
		p = lf_parse_int(p, &inst->maxFlares);
	} else {
		if (!spi_read_i32be(ls, &v) || v != SPECFLARE_OBJECT_VERSION)
			return 0;
		if (!spi_read_i32be(ls, &threshold)) return 0;
		if (!spi_read_i32be(ls, &glowRadius)) return 0;
		if (!spi_read_i32be(ls, &streakLength)) return 0;
		if (!spi_read_i32be(ls, &intensity)) return 0;
		if (!spi_read_i32be(ls, &starFilter)) return 0;
		if (!spi_read_i32be(ls, &rotationAngle)) return 0;
		if (!spi_read_i32be(ls, &spin)) return 0;
		if (!spi_read_i32be(ls, &showRing)) return 0;
		if (!spi_read_i32be(ls, &anamorphic)) return 0;
		if (!spi_read_i32be(ls, &maxFlares)) return 0;
		inst->threshold = threshold;
		inst->glowRadius = glowRadius;
		inst->streakLength = streakLength;
		inst->intensity = intensity;
		inst->starFilter = starFilter;
		inst->rotationAngle = rotationAngle;
		inst->spin = spin;
		inst->showRing = showRing;
		inst->anamorphic = anamorphic;
		inst->maxFlares = maxFlares;
	}
	ClampSettings(inst);
	return 0;
}

XCALL_(static LWError)
Save(SpecFlareInst *inst, const LWSaveState *ss)
{
	int pos = 0;
	XCALL_INIT;

	inst->ioBuffer[0] = '\0';
	lf_append_int(inst->ioBuffer, &pos, SPECFLARE_OBJECT_VERSION);
	lf_append_int(inst->ioBuffer, &pos, inst->threshold);
	lf_append_int(inst->ioBuffer, &pos, inst->glowRadius);
	lf_append_int(inst->ioBuffer, &pos, inst->streakLength);
	lf_append_int(inst->ioBuffer, &pos, inst->intensity);
	lf_append_int(inst->ioBuffer, &pos, inst->starFilter);
	lf_append_int(inst->ioBuffer, &pos, inst->rotationAngle);
	lf_append_int(inst->ioBuffer, &pos, inst->spin);
	lf_append_int(inst->ioBuffer, &pos, inst->showRing);
	lf_append_int(inst->ioBuffer, &pos, inst->anamorphic);
	lf_append_int(inst->ioBuffer, &pos, inst->maxFlares);

	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, inst->ioBuffer);
	} else {
		spi_write_i32be(ss, SPECFLARE_OBJECT_VERSION);
		spi_write_i32be(ss, inst->threshold);
		spi_write_i32be(ss, inst->glowRadius);
		spi_write_i32be(ss, inst->streakLength);
		spi_write_i32be(ss, inst->intensity);
		spi_write_i32be(ss, inst->starFilter);
		spi_write_i32be(ss, inst->rotationAngle);
		spi_write_i32be(ss, inst->spin);
		spi_write_i32be(ss, inst->showRing);
		spi_write_i32be(ss, inst->anamorphic);
		spi_write_i32be(ss, inst->maxFlares);
	}

	return 0;
}

XCALL_(static void)
Process(SpecFlareInst *inst, const FilterAccess *fa)
{
	FlareSource *detect;
	FlareSource *flares;
	StarDirection *stars = 0;
	int numDetect = 0, numFlares = 0, numStars = 0;
	int x, y, i, s;
	double inten;
	int thresh, gradR, strkLen;

	XCALL_INIT;

	detect = (FlareSource *)plugin_alloc(sizeof(FlareSource) * MAX_DETECT);
	flares = (FlareSource *)plugin_alloc(sizeof(FlareSource) * MAX_FLARES);
	if (inst->starFilter) {
		stars = (StarDirection *)plugin_alloc(
			sizeof(StarDirection) * MAX_STAR_DIRECTIONS);
	}
	if (!detect || !flares || (inst->starFilter && !stars)) {
		if (stars) plugin_free(stars);
		if (detect) plugin_free(detect);
		if (flares) plugin_free(flares);
		return;
	}

	if (stars) numStars = BuildStarDirections(inst, fa->frame, stars);
	thresh = inst->threshold;
	gradR = inst->glowRadius;
	strkLen = inst->streakLength;
	inten = inst->intensity / 100.0;

	for (y = 0; y < fa->height; y++) {
		BufferValue *rLine = (*fa->bufLine)(LWBUF_RED, y);
		BufferValue *gLine = (*fa->bufLine)(LWBUF_GREEN, y);
		BufferValue *bLine = (*fa->bufLine)(LWBUF_BLUE, y);
		BufferValue *dLine = (*fa->bufLine)(LWBUF_DEPTH, y);
		if (!rLine || !gLine || !bLine) continue;

		for (x = 0; x < fa->width; x++) {
			int bright = ((int)rLine[x] + (int)gLine[x] + (int)bLine[x]) / 3;
			if (bright >= thresh) {
				double d = (dLine && dLine[x] > 0)
				         ? (double)dLine[x] : 1.0;
				int merged = 0, fi;
				for (fi = 0; fi < numDetect; fi++) {
					int ddx = x - detect[fi].x;
					int ddy = y - detect[fi].y;
					if (ddx * ddx + ddy * ddy < 900) {
						detect[fi].accumBright += bright;
						detect[fi].pixelCount++;
						if (bright > detect[fi].brightness) {
							detect[fi].x = x;
							detect[fi].y = y;
							detect[fi].brightness = bright;
							detect[fi].depth = d;
						}
						merged = 1;
						break;
					}
				}
				if (!merged) {
					if (numDetect < MAX_DETECT) {
						detect[numDetect].x = x;
						detect[numDetect].y = y;
						detect[numDetect].brightness = bright;
						detect[numDetect].accumBright = bright;
						detect[numDetect].pixelCount = 1;
						detect[numDetect].depth = d;
						numDetect++;
					} else {
						int weakest = 0, wi;
						for (wi = 1; wi < MAX_DETECT; wi++) {
							if (detect[wi].accumBright < detect[weakest].accumBright)
								weakest = wi;
						}
						if (bright > detect[weakest].brightness) {
							detect[weakest].x = x;
							detect[weakest].y = y;
							detect[weakest].brightness = bright;
							detect[weakest].accumBright = bright;
							detect[weakest].pixelCount = 1;
							detect[weakest].depth = d;
						}
					}
				}
			}
		}
	}

	for (i = 0; i < inst->maxFlares && i < numDetect; i++) {
		int best = -1, bi;
		for (bi = 0; bi < numDetect; bi++) {
			if (detect[bi].accumBright > 0) {
				if (best < 0 || detect[bi].accumBright > detect[best].accumBright)
					best = bi;
			}
		}
		if (best < 0) break;
		flares[numFlares++] = detect[best];
		detect[best].accumBright = 0;
	}

	for (i = 0; i < numFlares; i++) {
		double dscale = 1.0 / (1.0 + flares[i].depth * 0.1);
		double areaScale = flares[i].pixelCount / 10.0;
		if (areaScale > 2.0) areaScale = 2.0;
		if (areaScale < 0.15) areaScale = 0.15;
		dscale *= areaScale;
		flares[i].glowRadius = (int)(gradR * dscale);
		flares[i].streakLength = (int)(strkLen * dscale);
		if (flares[i].glowRadius < 4) flares[i].glowRadius = 4;
		if (flares[i].streakLength < 4) flares[i].streakLength = 4;
		flares[i].depth = (double)(flares[i].glowRadius
		                         * flares[i].glowRadius);
	}

	for (y = 0; y < fa->height; y++) {
		BufferValue *rLine = (*fa->bufLine)(LWBUF_RED, y);
		BufferValue *gLine = (*fa->bufLine)(LWBUF_GREEN, y);
		BufferValue *bLine = (*fa->bufLine)(LWBUF_BLUE, y);
		if (!rLine || !gLine || !bLine) continue;

		for (x = 0; x < fa->width; x++) {
			BufferValue rgb[3];
			double fR = 0.0, fG = 0.0, fB = 0.0;

			for (i = 0; i < numFlares; i++) {
				int fx = flares[i].x;
				int fy = flares[i].y;
				int sGlow = flares[i].glowRadius;
				int sStrk = flares[i].streakLength;
				int idx = x - fx;
				int idy = y - fy;
				int ir2, imaxR;
				double sGlow2, bright, dx, dy, r2;
				double tR = 0.0, tG = 0.0, tB = 0.0;

				imaxR = sGlow * 4;
				if (sStrk > imaxR) imaxR = sStrk;
				if (inst->anamorphic && sStrk * 4 > imaxR)
					imaxR = sStrk * 4;
				ir2 = idx * idx + idy * idy;
				if (ir2 > imaxR * imaxR) continue;

				sGlow2 = flares[i].depth;
				bright = (double)flares[i].brightness / 255.0;
				dx = (double)idx;
				dy = (double)idy;
				r2 = (double)ir2;

				if (r2 < 9.0) {
					double core = inten * bright;
					tR += core; tG += core; tB += core;
				}

				{
					double gd = 1.0 + r2 / sGlow2;
					double glow = inten * bright * 0.8 / (gd * gd);
					tR += glow;
					tG += glow * 0.8;
					tB += glow * 0.6;
				}

				if (inst->showRing) {
					double ringR2 = sGlow2 * 0.5;
					double rdiff = r2 - ringR2;
					if (rdiff < 0.0) rdiff = -rdiff;
					if (rdiff < ringR2 * 0.3) {
						double ringI = (1.0 - rdiff / (ringR2 * 0.3))
						             * inten * bright * 0.5;
						tR += ringI;
						tG += ringI * 0.3;
						tB += ringI * 0.8;
					}
				}

				if (inst->anamorphic) {
					double ax = dx > 0 ? dx : -dx;
					double aLen = (double)sStrk * 4.0;
					if (ax < aLen) {
						double hfade = 1.0 - ax / aLen;
						double hw = 1.0 / (1.0 + dy * dy * 0.02);
						double h = hfade * hfade * hw * inten * bright * 0.7;
						tR += h * 0.3;
						tG += h * 0.5;
						tB += h;
					}
				}

				for (s = 0; s < numStars; s++) {
					long along = (long)idx * stars[s].dx
					           + (long)idy * stars[s].dy;
					long perp = (long)idx * stars[s].dy
					          - (long)idy * stars[s].dx;
					long length = (long)sStrk * stars[s].lengthScale;

					if (along > 0 && along < length
					 && perp > -4 * STAR_DIRECTION_SCALE
					 && perp < 4 * STAR_DIRECTION_SCALE) {
						double fade = 1.0 - (double)along / (double)length;
						double p = (double)perp * (1.0 / (double)STAR_DIRECTION_SCALE);
						double width = 1.0 / (1.0 + p * p * 0.5);
						double sk = fade * width * inten * bright * 0.6
						          * (double)stars[s].intensityScale
						          * (1.0 / (double)STAR_DIRECTION_SCALE);
						tR += sk; tG += sk * 0.85; tB += sk * 0.7;
					}
				}

				if (tR > fR) fR = tR;
				if (tG > fG) fG = tG;
				if (tB > fB) fB = tB;
			}

			if (fR > 0.001 || fG > 0.001 || fB > 0.001) {
				int rv = rLine[x] + (int)(fR * 255.0);
				int gv = gLine[x] + (int)(fG * 255.0);
				int bv = bLine[x] + (int)(fB * 255.0);
				if (rv > 255) rv = 255;
				if (gv > 255) gv = 255;
				if (bv > 255) bv = 255;
				rgb[0] = (BufferValue)rv;
				rgb[1] = (BufferValue)gv;
				rgb[2] = (BufferValue)bv;
			} else {
				rgb[0] = rLine[x];
				rgb[1] = gLine[x];
				rgb[2] = bLine[x];
			}
			(*fa->setRGB)(x, y, rgb);
		}
	}

	if (stars) plugin_free(stars);
	plugin_free(flares);
	plugin_free(detect);
}

XCALL_(static unsigned int)
Flags(SpecFlareInst *inst)
{
	XCALL_INIT;
	return 0;
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static const char *starItems[] = {
	"Off",
	"2 Point",
	"2+2 Point",
	"4 Point",
	"4+4 Point",
	"6 Point",
	"6+6 Point",
	"8 Point",
	"8+8 Point",
	"10 Point",
	"10+10 Point",
	0
};

XCALL_(static int)
Interface(
	long             version,
	GlobalFunc      *global,
	SpecFlareInst   *inst,
	void            *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl    *ctlThresh, *ctlGlow, *ctlStreak, *ctlInten, *ctlStar;
	LWControl    *ctlRotation, *ctlSpin, *ctlRing, *ctlAnamorphic, *ctlMaxFlr;

	XCALL_INIT;
	if (version < 1)
		return AFUNC_BADVERSION;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	static LWPanControlDesc desc;
	static LWValue ival = {LWT_INTEGER};

	if (panl) {
		pan = PAN_CREATE(panl, "SpecFlare v" PLUGIN_VERSION
		                       " (c) D. Panokostas");
		if (!pan) return AFUNC_OK;

		ctlThresh = SLIDER_CTL(panl, pan, "Threshold", 150, 0, 255);
		ctlGlow = INT_CTL(panl, pan, "Glow Radius");
		ctlStreak = INT_CTL(panl, pan, "Streak Length");
		ctlInten = SLIDER_CTL(panl, pan, "Intensity", 150, 0, 100);
		ctlStar = POPUP_CTL(panl, pan, "Star Filter", starItems);
		ctlRotation = INT_CTL(panl, pan, "Rotation Angle");
		ctlSpin = INT_CTL(panl, pan, "Spin");
		ctlRing = BOOL_CTL(panl, pan, "Show Ring");
		ctlAnamorphic = BOOL_CTL(panl, pan, "Anamorphic");
		ctlMaxFlr = SLIDER_CTL(panl, pan, "Max Flares", 150, 1, MAX_FLARES);

		SET_INT(ctlThresh, inst->threshold);
		SET_INT(ctlGlow, inst->glowRadius);
		SET_INT(ctlStreak, inst->streakLength);
		SET_INT(ctlInten, inst->intensity);
		SET_INT(ctlStar, inst->starFilter);
		SET_INT(ctlRotation, inst->rotationAngle);
		SET_INT(ctlSpin, inst->spin);
		SET_INT(ctlRing, inst->showRing);
		SET_INT(ctlAnamorphic, inst->anamorphic);
		SET_INT(ctlMaxFlr, inst->maxFlares);

		if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
			GET_INT(ctlThresh, inst->threshold);
			GET_INT(ctlGlow, inst->glowRadius);
			GET_INT(ctlStreak, inst->streakLength);
			GET_INT(ctlInten, inst->intensity);
			GET_INT(ctlStar, inst->starFilter);
			GET_INT(ctlRotation, inst->rotationAngle);
			GET_INT(ctlSpin, inst->spin);
			GET_INT(ctlRing, inst->showRing);
			GET_INT(ctlAnamorphic, inst->anamorphic);
			GET_INT(ctlMaxFlr, inst->maxFlares);
			ClampSettings(inst);
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
	h->descln   = 0;
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
	{ "ImageFilterHandler",   "SpecFlare",
	  (ActivateFunc *)Activate },
	{ "ImageFilterInterface", "SpecFlare",
	  (ActivateFunc *)Interface },
	{ 0 }
};
