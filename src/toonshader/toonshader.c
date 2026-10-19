/*
 * TOONSHADER.C -- Per-surface cel shader for LightWave 3D 5.x
 *
 * Bakes shadow-aware direct lighting into colour, then replaces LightWave's
 * diffuse/specular pass.  This keeps the effect limited to surfaces carrying
 * this shader rather than changing the whole rendered image.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

#include <string.h>

#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;
extern double sqrt(double);

#define TOONSHADER_VERSION 1

typedef struct {
	int bands;
	int softness;
	int outlineEnabled;
	int outlineWidth;
	int geometricOutline;
	int outlineR, outlineG, outlineB;
	int highlightEnabled;
	int highlightSize;
	int highlightIntensity;
	int highlightR, highlightG, highlightB;
	int rimAmount;
	int rimR, rimG, rimB;
	int shadowR, shadowG, shadowB;
	LWItemInfo *itemInfo;
	int itemInfoV2;
	char ioBuf[128];
} ToonShaderInst;

typedef struct {
	double red, green, blue;
	double level;
	double highlight;
} ToonLighting;

static GlobalFunc *gfunc;
static MessageFuncs *msg;

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

static void
int_to_str(int val, char *buf, int buflen)
{
	char tmp[12];
	int i = 0, j, len;

	if (val == 0) tmp[i++] = '0';
	else {
		while (val > 0 && i < 11) {
			tmp[i++] = (char)('0' + (val % 10));
			val /= 10;
		}
	}
	len = i;
	if (len >= buflen) len = buflen - 1;
	for (j = 0; j < len; j++) buf[j] = tmp[i - 1 - j];
	buf[len] = '\0';
}

/* Kept separate from int_to_str's historical variants in other plugins. */
static void
append_int(char *buf, int *pos, int val)
{
	char tmp[12];
	int n = 0;

	if (val == 0) tmp[n++] = '0';
	else {
		while (val > 0 && n < 11) {
			tmp[n++] = (char)('0' + (val % 10));
			val /= 10;
		}
	}
	if (*pos > 0) buf[(*pos)++] = ' ';
	while (n > 0) buf[(*pos)++] = tmp[--n];
	buf[*pos] = '\0';
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

static double
clamp01(double value)
{
	if (value < 0.0) return 0.0;
	if (value > 1.0) return 1.0;
	return value;
}

static void
normalise3(double *x, double *y, double *z)
{
	double length = sqrt(*x * *x + *y * *y + *z * *z);
	if (length > 0.00001) {
		*x /= length;
		*y /= length;
		*z /= length;
	}
}

static void
init_defaults(ToonShaderInst *inst)
{
	inst->bands = 4;
	inst->softness = 0;
	inst->outlineEnabled = 1;
	inst->outlineWidth = 12;
	inst->geometricOutline = 0;
	inst->outlineR = 0;
	inst->outlineG = 0;
	inst->outlineB = 0;
	inst->highlightEnabled = 1;
	inst->highlightSize = 18;
	inst->highlightIntensity = 100;
	inst->highlightR = 255;
	inst->highlightG = 255;
	inst->highlightB = 255;
	inst->rimAmount = 0;
	inst->rimR = 180;
	inst->rimG = 210;
	inst->rimB = 255;
	inst->shadowR = 45;
	inst->shadowG = 55;
	inst->shadowB = 90;
}

static void
clamp_settings(ToonShaderInst *inst)
{
	if (inst->bands < 2) inst->bands = 2;
	if (inst->bands > 8) inst->bands = 8;
	if (inst->softness < 0) inst->softness = 0;
	if (inst->softness > 100) inst->softness = 100;
	if (inst->outlineWidth < 1) inst->outlineWidth = 1;
	if (inst->outlineWidth > 100) inst->outlineWidth = 100;
	if (inst->highlightSize < 0) inst->highlightSize = 0;
	if (inst->highlightSize > 100) inst->highlightSize = 100;
	if (inst->highlightIntensity < 0) inst->highlightIntensity = 0;
	if (inst->highlightIntensity > 100) inst->highlightIntensity = 100;
	if (inst->rimAmount < 0) inst->rimAmount = 0;
	if (inst->rimAmount > 100) inst->rimAmount = 100;
	if (inst->outlineR < 0) inst->outlineR = 0;
	if (inst->outlineR > 255) inst->outlineR = 255;
	if (inst->outlineG < 0) inst->outlineG = 0;
	if (inst->outlineG > 255) inst->outlineG = 255;
	if (inst->outlineB < 0) inst->outlineB = 0;
	if (inst->outlineB > 255) inst->outlineB = 255;
	if (inst->highlightR < 0) inst->highlightR = 0;
	if (inst->highlightR > 255) inst->highlightR = 255;
	if (inst->highlightG < 0) inst->highlightG = 0;
	if (inst->highlightG > 255) inst->highlightG = 255;
	if (inst->highlightB < 0) inst->highlightB = 0;
	if (inst->highlightB > 255) inst->highlightB = 255;
	if (inst->rimR < 0) inst->rimR = 0;
	if (inst->rimR > 255) inst->rimR = 255;
	if (inst->rimG < 0) inst->rimG = 0;
	if (inst->rimG > 255) inst->rimG = 255;
	if (inst->rimB < 0) inst->rimB = 0;
	if (inst->rimB > 255) inst->rimB = 255;
	if (inst->shadowR < 0) inst->shadowR = 0;
	if (inst->shadowR > 255) inst->shadowR = 255;
	if (inst->shadowG < 0) inst->shadowG = 0;
	if (inst->shadowG > 255) inst->shadowG = 255;
	if (inst->shadowB < 0) inst->shadowB = 0;
	if (inst->shadowB > 255) inst->shadowB = 255;
}

XCALL_(static LWInstance)
Create(LWError *err)
{
	ToonShaderInst *inst;
	XCALL_INIT;
	inst = (ToonShaderInst *)plugin_alloc(sizeof(ToonShaderInst));
	if (!inst) return 0;
	init_defaults(inst);
	return inst;
}

XCALL_(static void)
Destroy(ToonShaderInst *inst)
{
	XCALL_INIT;
	if (inst) plugin_free(inst);
}

XCALL_(static LWError)
Copy(ToonShaderInst *from, ToonShaderInst *to)
{
	XCALL_INIT;
	*to = *from;
	/* Item Info belongs to Init/Cleanup on each instance. */
	to->itemInfo = 0;
	to->itemInfoV2 = 0;
	return 0;
}

XCALL_(static LWError)
Load(ToonShaderInst *inst, const LWLoadState *ls)
{
	char *buf = inst->ioBuf;
	const char *p;
	int version;
	XCALL_INIT;

	if (ls->ioMode == LWIO_SCENE) {
		if (!spi_read_line(ls, buf, sizeof(buf)) || !buf[0]) return 0;
		p = parse_int(buf, &version);
		if (version != TOONSHADER_VERSION) return 0;
		p = parse_int(p, &inst->bands);
		p = parse_int(p, &inst->softness);
		p = parse_int(p, &inst->outlineEnabled);
		p = parse_int(p, &inst->outlineWidth);
		p = parse_int(p, &inst->geometricOutline);
		p = parse_int(p, &inst->outlineR);
		p = parse_int(p, &inst->outlineG);
		p = parse_int(p, &inst->outlineB);
		p = parse_int(p, &inst->highlightEnabled);
		p = parse_int(p, &inst->highlightSize);
		p = parse_int(p, &inst->highlightIntensity);
		p = parse_int(p, &inst->highlightR);
		p = parse_int(p, &inst->highlightG);
		p = parse_int(p, &inst->highlightB);
		p = parse_int(p, &inst->rimAmount);
		p = parse_int(p, &inst->rimR);
		p = parse_int(p, &inst->rimG);
		p = parse_int(p, &inst->rimB);
		p = parse_int(p, &inst->shadowR);
		p = parse_int(p, &inst->shadowG);
		p = parse_int(p, &inst->shadowB);
	} else {
		if (!spi_read_i32be(ls, &version) || version != TOONSHADER_VERSION) return 0;
		if (!spi_read_i32be(ls, &inst->bands)) return 0;
		if (!spi_read_i32be(ls, &inst->softness)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineEnabled)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineWidth)) return 0;
		if (!spi_read_i32be(ls, &inst->geometricOutline)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineR)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineG)) return 0;
		if (!spi_read_i32be(ls, &inst->outlineB)) return 0;
		if (!spi_read_i32be(ls, &inst->highlightEnabled)) return 0;
		if (!spi_read_i32be(ls, &inst->highlightSize)) return 0;
		if (!spi_read_i32be(ls, &inst->highlightIntensity)) return 0;
		if (!spi_read_i32be(ls, &inst->highlightR)) return 0;
		if (!spi_read_i32be(ls, &inst->highlightG)) return 0;
		if (!spi_read_i32be(ls, &inst->highlightB)) return 0;
		if (!spi_read_i32be(ls, &inst->rimAmount)) return 0;
		if (!spi_read_i32be(ls, &inst->rimR)) return 0;
		if (!spi_read_i32be(ls, &inst->rimG)) return 0;
		if (!spi_read_i32be(ls, &inst->rimB)) return 0;
		if (!spi_read_i32be(ls, &inst->shadowR)) return 0;
		if (!spi_read_i32be(ls, &inst->shadowG)) return 0;
		if (!spi_read_i32be(ls, &inst->shadowB)) return 0;
	}
	clamp_settings(inst);
	return 0;
}

XCALL_(static LWError)
Save(ToonShaderInst *inst, const LWSaveState *ss)
{
	char *buf = inst->ioBuf;
	int pos = 0;
	XCALL_INIT;

	append_int(buf, &pos, TOONSHADER_VERSION);
	append_int(buf, &pos, inst->bands);
	append_int(buf, &pos, inst->softness);
	append_int(buf, &pos, inst->outlineEnabled);
	append_int(buf, &pos, inst->outlineWidth);
	append_int(buf, &pos, inst->geometricOutline);
	append_int(buf, &pos, inst->outlineR);
	append_int(buf, &pos, inst->outlineG);
	append_int(buf, &pos, inst->outlineB);
	append_int(buf, &pos, inst->highlightEnabled);
	append_int(buf, &pos, inst->highlightSize);
	append_int(buf, &pos, inst->highlightIntensity);
	append_int(buf, &pos, inst->highlightR);
	append_int(buf, &pos, inst->highlightG);
	append_int(buf, &pos, inst->highlightB);
	append_int(buf, &pos, inst->rimAmount);
	append_int(buf, &pos, inst->rimR);
	append_int(buf, &pos, inst->rimG);
	append_int(buf, &pos, inst->rimB);
	append_int(buf, &pos, inst->shadowR);
	append_int(buf, &pos, inst->shadowG);
	append_int(buf, &pos, inst->shadowB);

	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
	} else {
		spi_write_i32be(ss, TOONSHADER_VERSION);
		spi_write_i32be(ss, inst->bands); spi_write_i32be(ss, inst->softness);
		spi_write_i32be(ss, inst->outlineEnabled); spi_write_i32be(ss, inst->outlineWidth);
		spi_write_i32be(ss, inst->geometricOutline);
		spi_write_i32be(ss, inst->outlineR); spi_write_i32be(ss, inst->outlineG); spi_write_i32be(ss, inst->outlineB);
		spi_write_i32be(ss, inst->highlightEnabled); spi_write_i32be(ss, inst->highlightSize);
		spi_write_i32be(ss, inst->highlightIntensity);
		spi_write_i32be(ss, inst->highlightR); spi_write_i32be(ss, inst->highlightG); spi_write_i32be(ss, inst->highlightB);
		spi_write_i32be(ss, inst->rimAmount);
		spi_write_i32be(ss, inst->rimR); spi_write_i32be(ss, inst->rimG); spi_write_i32be(ss, inst->rimB);
		spi_write_i32be(ss, inst->shadowR); spi_write_i32be(ss, inst->shadowG); spi_write_i32be(ss, inst->shadowB);
	}
	return 0;
}

XCALL_(static LWError)
Init(ToonShaderInst *inst)
{
	XCALL_INIT;
	inst->itemInfo = 0;
	inst->itemInfoV2 = 0;
	if (gfunc) {
		inst->itemInfo = (LWItemInfo *)(*gfunc)("LW Item Info 2", GFUSE_ACQUIRE);
		if (inst->itemInfo) inst->itemInfoV2 = 1;
		else inst->itemInfo = (LWItemInfo *)(*gfunc)("LW Item Info", GFUSE_ACQUIRE);
	}
	return 0;
}

XCALL_(static void)
Cleanup(ToonShaderInst *inst)
{
	XCALL_INIT;
	if (inst->itemInfo && gfunc) {
		(*gfunc)(inst->itemInfoV2 ? "LW Item Info 2" : "LW Item Info", GFUSE_RELEASE);
		inst->itemInfo = 0;
		inst->itemInfoV2 = 0;
	}
}

XCALL_(static LWError)
NewTime(ToonShaderInst *inst, LWFrame frame, LWTime time)
{
	XCALL_INIT;
	return 0;
}

XCALL_(static unsigned int)
Flags(ToonShaderInst *inst)
{
	XCALL_INIT;
	return LWSHF_COLOR | LWSHF_LUMINOUS | LWSHF_DIFFUSE |
	       LWSHF_SPECULAR | LWSHF_MIRROR | LWSHF_RAYTRACE;
}

/* illuminate() returns a shadowed, attenuated colour, so this preserves
 * LightWave's cast shadows while replacing only the final light response. */
static void
sample_lights(ToonShaderInst *inst, ShaderAccess *sa, ToonLighting *result)
{
	LWItemID light;
	double direction[3], lightColor[3];
	double vx, vy, vz, nx, ny, nz;
	double ndotl, intensity, halfx, halfy, halfz, ndoth;

	result->red = result->green = result->blue = 0.0;
	result->level = result->highlight = 0.0;
	if (!inst->itemInfo || !sa->illuminate) return;

	vx = sa->raySource[0] - sa->wPos[0];
	vy = sa->raySource[1] - sa->wPos[1];
	vz = sa->raySource[2] - sa->wPos[2];
	normalise3(&vx, &vy, &vz);
	nx = sa->wNorm[0]; ny = sa->wNorm[1]; nz = sa->wNorm[2];

	light = (*inst->itemInfo->first)(LWI_LIGHT, LWITEM_NULL);
	while (light) {
		if ((*sa->illuminate)(light, sa->wPos, direction, lightColor)) {
			normalise3(&direction[0], &direction[1], &direction[2]);
			/* illuminate returns the incoming light ray, so turn it
			 * into the point-to-light direction for N.L and Blinn. */
			direction[0] = -direction[0];
			direction[1] = -direction[1];
			direction[2] = -direction[2];
			ndotl = nx * direction[0] + ny * direction[1] + nz * direction[2];
			if (ndotl > 0.0) {
				result->red += lightColor[0] * ndotl;
				result->green += lightColor[1] * ndotl;
				result->blue += lightColor[2] * ndotl;
				intensity = lightColor[0];
				if (lightColor[1] > intensity) intensity = lightColor[1];
				if (lightColor[2] > intensity) intensity = lightColor[2];
				halfx = direction[0] + vx;
				halfy = direction[1] + vy;
				halfz = direction[2] + vz;
				normalise3(&halfx, &halfy, &halfz);
				ndoth = nx * halfx + ny * halfy + nz * halfz;
				ndoth *= intensity;
				if (ndoth > result->highlight)
					result->highlight = ndoth;
			}
		}
		light = (*inst->itemInfo->next)(light);
	}
	result->level = result->red;
	if (result->green > result->level) result->level = result->green;
	if (result->blue > result->level) result->level = result->blue;
}

static double
band_value(double level, int bands, int softness)
{
	double scaled, fraction, base, transition;
	int band;

	if (bands < 2) return clamp01(level);
	scaled = clamp01(level) * (double)bands;
	band = (int)scaled;
	if (band >= bands - 1) return 1.0;
	base = (double)band / (double)(bands - 1);
	if (softness <= 0) return base;
	fraction = scaled - (double)band;
	transition = 1.0 - (double)softness / 100.0;
	if (fraction <= transition) return base;
	return base + (fraction - transition) / (1.0 - transition) /
		(double)(bands - 1);
}

XCALL_(static void)
Evaluate(ToonShaderInst *inst, ShaderAccess *sa)
{
	ToonLighting lights;
	double band, hueR, hueG, hueB, outR, outG, outB;
	double vx, vy, vz, facing, rim, edge;
	double nx, ny, nz;
	XCALL_INIT;

	sample_lights(inst, sa, &lights);
	band = band_value(lights.level, inst->bands, inst->softness);
	if (lights.level > 0.00001) {
		hueR = clamp01(lights.red / lights.level);
		hueG = clamp01(lights.green / lights.level);
		hueB = clamp01(lights.blue / lights.level);
	} else hueR = hueG = hueB = 0.0;

	outR = (inst->shadowR / 255.0) * (1.0 - band) + sa->color[0] * hueR * band;
	outG = (inst->shadowG / 255.0) * (1.0 - band) + sa->color[1] * hueG * band;
	outB = (inst->shadowB / 255.0) * (1.0 - band) + sa->color[2] * hueB * band;

	vx = sa->wPos[0] - sa->raySource[0];
	vy = sa->wPos[1] - sa->raySource[1];
	vz = sa->wPos[2] - sa->raySource[2];
	normalise3(&vx, &vy, &vz);
	nx = sa->wNorm[0]; ny = sa->wNorm[1]; nz = sa->wNorm[2];
	if (inst->geometricOutline) {
		nx = sa->gNorm[0]; ny = sa->gNorm[1]; nz = sa->gNorm[2];
	}
	normalise3(&nx, &ny, &nz);
	facing = nx * vx + ny * vy + nz * vz;
	if (facing < 0.0) facing = -facing;
	facing = clamp01(facing);

	if (inst->rimAmount > 0) {
		rim = (1.0 - facing) * (inst->rimAmount / 100.0);
		outR += rim * (inst->rimR / 255.0);
		outG += rim * (inst->rimG / 255.0);
		outB += rim * (inst->rimB / 255.0);
	}
	if (inst->highlightEnabled && inst->highlightIntensity > 0 &&
		lights.highlight >= 0.98 - inst->highlightSize * 0.005) {
		double highlight = inst->highlightIntensity / 100.0;
		outR += highlight * (inst->highlightR / 255.0);
		outG += highlight * (inst->highlightG / 255.0);
		outB += highlight * (inst->highlightB / 255.0);
	}

	edge = inst->outlineWidth / 100.0;
	if (inst->outlineEnabled && facing < edge) {
		outR = inst->outlineR / 255.0;
		outG = inst->outlineG / 255.0;
		outB = inst->outlineB / 255.0;
	}

	sa->color[0] = clamp01(outR);
	sa->color[1] = clamp01(outG);
	sa->color[2] = clamp01(outB);
	sa->luminous = 1.0;
	sa->diffuse = 0.0;
	sa->specular = 0.0;
	sa->mirror = 0.0;
}

static char descBuf[64];

XCALL_(static const char *)
DescLn(ToonShaderInst *inst)
{
	int pos = 0;
	XCALL_INIT;
	int_to_str(inst->bands, descBuf, sizeof(descBuf));
	pos = (int)strlen(descBuf);
	strcpy(descBuf + pos, " bands");
	if (inst->outlineEnabled) strcat(descBuf, ", outline");
	if (inst->highlightEnabled) strcat(descBuf, ", highlight");
	return descBuf;
}

XCALL_(static int)
Interface(long version, GlobalFunc *global, ToonShaderInst *inst, void *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID pan;
	LWControl *bands, *softness, *outline, *width, *geometric, *outlineColor;
	LWControl *highlight, *highlightSize, *highlightIntensity, *highlightColor;
	LWControl *rimAmount, *rimColor, *shadowColor;
	XCALL_INIT;
	if (version != 1) return AFUNC_BADVERSION;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	if (!panl) {
		if (msg) (*msg->info)("ToonShader", "LWPanelServices is unavailable");
		return AFUNC_OK;
	}
	{
		static LWPanControlDesc desc;
		static LWValue ival = { LWT_INTEGER };
		static LWValue ivecval = { LWT_VINT };

		pan = PAN_CREATE(panl, "ToonShader v" PLUGIN_VERSION);
		if (!pan) return AFUNC_OK;
		bands = SLIDER_CTL(panl, pan, "Bands", 150, 2, 8);
		softness = SLIDER_CTL(panl, pan, "Band Softness", 150, 0, 100);
		outline = BOOL_CTL(panl, pan, "Outline");
		width = SLIDER_CTL(panl, pan, "Edge Width", 150, 1, 100);
		geometric = BOOL_CTL(panl, pan, "Use Geometric Normal");
		outlineColor = RGB_CTL(panl, pan, "Edge Colour");
		highlight = BOOL_CTL(panl, pan, "Highlight");
		highlightSize = SLIDER_CTL(panl, pan, "Highlight Size", 150, 0, 100);
		highlightIntensity = SLIDER_CTL(panl, pan, "Highlight Intensity", 150, 0, 100);
		highlightColor = RGB_CTL(panl, pan, "Highlight Colour");
		rimAmount = SLIDER_CTL(panl, pan, "Rim Amount", 150, 0, 100);
		rimColor = RGB_CTL(panl, pan, "Rim Colour");
		shadowColor = RGB_CTL(panl, pan, "Shadow Colour");

		SET_INT(bands, inst->bands); SET_INT(softness, inst->softness);
		SET_INT(outline, inst->outlineEnabled); SET_INT(width, inst->outlineWidth);
		SET_INT(geometric, inst->geometricOutline);
		SET_IVEC(outlineColor, inst->outlineR, inst->outlineG, inst->outlineB);
		SET_INT(highlight, inst->highlightEnabled); SET_INT(highlightSize, inst->highlightSize);
		SET_INT(highlightIntensity, inst->highlightIntensity);
		SET_IVEC(highlightColor, inst->highlightR, inst->highlightG, inst->highlightB);
		SET_INT(rimAmount, inst->rimAmount);
		SET_IVEC(rimColor, inst->rimR, inst->rimG, inst->rimB);
		SET_IVEC(shadowColor, inst->shadowR, inst->shadowG, inst->shadowB);

		if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
			GET_INT(bands, inst->bands); GET_INT(softness, inst->softness);
			GET_INT(outline, inst->outlineEnabled); GET_INT(width, inst->outlineWidth);
			GET_INT(geometric, inst->geometricOutline);
			GET_IVEC(outlineColor, inst->outlineR, inst->outlineG, inst->outlineB);
			GET_INT(highlight, inst->highlightEnabled); GET_INT(highlightSize, inst->highlightSize);
			GET_INT(highlightIntensity, inst->highlightIntensity);
			GET_IVEC(highlightColor, inst->highlightR, inst->highlightG, inst->highlightB);
			GET_INT(rimAmount, inst->rimAmount);
			GET_IVEC(rimColor, inst->rimR, inst->rimG, inst->rimB);
			GET_IVEC(shadowColor, inst->shadowR, inst->shadowG, inst->shadowB);
			clamp_settings(inst);
		}
		PAN_KILL(panl, pan);
	}
	return AFUNC_OK;
}

XCALL_(int)
Activate(long version, GlobalFunc *global, void *local, void *serverData)
{
	ShaderHandler *handler = (ShaderHandler *)local;
	XCALL_INIT;
	if (version < 1) return AFUNC_BADVERSION;

	handler->create = (void *)Create;
	handler->destroy = (void *)Destroy;
	handler->copy = (void *)Copy;
	handler->load = (void *)Load;
	handler->save = (void *)Save;
	handler->init = (void *)Init;
	handler->cleanup = (void *)Cleanup;
	handler->newTime = (void *)NewTime;
	handler->evaluate = (void *)Evaluate;
	handler->flags = (void *)Flags;
	if (version >= 2)
		handler->descln = (void *)DescLn;

	gfunc = global;
	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!msg) return AFUNC_BADGLOBAL;
	return AFUNC_OK;
}

ServerRecord ServerDesc[] = {
	{ "ShaderHandler", "ToonShader", (ActivateFunc *)Activate },
	{ "ShaderInterface", "ToonShader", (ActivateFunc *)Interface },
	{ 0 }
};
