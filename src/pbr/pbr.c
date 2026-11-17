/*
 * PBR.C -- PBR-lite Shader Plugin for LightWave 3D
 *
 * Combines Fresnel reflection, roughness (normal perturbation),
 * metallic mode, blurred reflections, plus fast normal-based AO/env
 * approximations into a single shader for LightWave 5.x on AmigaOS.
 *
 * Uses AllocMem/FreeMem and custom helpers — no libnix runtime.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <lwmath.h>
#include <safe_pluginio.h>
#include <trig_approx.h>

#include <string.h>

#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;
extern double sqrt(double);

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


static void
vec_normalize(double v[3])
{
	double len = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	if (len > 0.00001) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

/*
 * Deterministic 3D hash for roughness perturbation.
 * Returns a value in -1.0 to 1.0 range.
 */
static double
hash3d(double x, double y, double z, unsigned int seed)
{
	unsigned int ix = (unsigned int)((x + 1000.0) * 731.0) & 0xFFFFu;
	unsigned int iy = (unsigned int)((y + 1000.0) * 541.0) & 0xFFFFu;
	unsigned int iz = (unsigned int)((z + 1000.0) * 379.0) & 0xFFFFu;
	unsigned int h = ix * 73856093u ^ iy * 19349669u ^ iz * 83492791u ^ seed;
	h = (h >> 13) ^ h;
	h = h * (h * 15731u + 789221u) + 1376312589u;
	return ((double)(h & 0x7FFFu) / (double)0x3FFFu) - 1.0;
}

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

typedef struct {
	/* Metallic base reflectance */
	double ior;
	double f0;

	/* Roughness */
	int    roughEnabled;
	int    roughAmount;      /* 0-100, used as percentage */

	/* Ambient Occlusion */
	int    aoEnabled;
	int    aoStrength;       /* 0-100 */

	/* Metallic */
	int    metallic;         /* 0-100 intensity */

	/* Blurred Reflections */
	int    blurReflEnabled;
	int    blurReflSamples;  /* 2, 4, 8 or 16 */
	int    blurReflAmount;   /* 0-100 cone spread */

	/* Environment Light */
	int    envEnabled;
	int    envStrength;      /* 0-100 */

	/* Render-time globals for blurred reflections (Init/Cleanup) */
	LWItemInfo  *itemInfo;
	LWLightInfo *lightInfo;
	int          lightInfoV2;	/* flags() exists only in LW Light Info 2 */
	LWTime       time;
} PBRInst;

#define PBR_SCENE_VERSION  2
#define PBR_OBJECT_VERSION 2

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static MessageFuncs *msg;
static GlobalFunc   *gfunc;

static double pow5_lut[101];
static int pow5_ready = 0;
/* sa->rayTrace can re-enter PBR on the ray hit; blur cones stay primary-only. */
static int blur_trace_depth = 0;

#define BLUR_MAX_SAMPLES 16

/* Unit-disk directions at golden-angle steps, filled in Create(). */
static double blur_disk[BLUR_MAX_SAMPLES][2];

/*
 * Blurred reflection working data. Kept off the stack because rayTrace
 * re-enters Evaluate for the surface the ray hits; only the primary
 * evaluation (blur_trace_depth == 0) uses it.
 */
static struct {
	double n[3];         /* normal facing the viewer */
	double refl[3];      /* mirror direction */
	double t[3], b[3];   /* basis around refl */
	double pos[3], dir[3], col[3];
	double sum[3];       /* averaged reflected colour */
	double light[3];     /* ambient + direct light reaching the point */
} blur;

/* ----------------------------------------------------------------
 * Precompute F0 from IOR
 * ---------------------------------------------------------------- */

static void
compute_f0(PBRInst *inst)
{
	double r;
	if (inst->ior < 1.0) inst->ior = 1.0;
	r = (inst->ior - 1.0) / (inst->ior + 1.0);
	inst->f0 = r * r;
}

/* Same limits as the panel; scene or object data may be hand-edited. */
static void
clamp_settings(PBRInst *inst)
{
	if (inst->ior < 1.0) inst->ior = 1.0;
	if (inst->ior > 5.0) inst->ior = 5.0;
	if (inst->metallic < 0) inst->metallic = 0;
	if (inst->metallic > 100) inst->metallic = 100;
	if (inst->roughAmount < 0) inst->roughAmount = 0;
	if (inst->roughAmount > 100) inst->roughAmount = 100;
	if (inst->aoStrength < 0) inst->aoStrength = 0;
	if (inst->aoStrength > 100) inst->aoStrength = 100;
	if (inst->blurReflSamples < 0) inst->blurReflSamples = 0;
	if (inst->blurReflSamples > BLUR_MAX_SAMPLES)
		inst->blurReflSamples = BLUR_MAX_SAMPLES;
	if (inst->blurReflAmount < 0) inst->blurReflAmount = 0;
	if (inst->blurReflAmount > 100) inst->blurReflAmount = 100;
	if (inst->envStrength < 0) inst->envStrength = 0;
	if (inst->envStrength > 100) inst->envStrength = 100;
	inst->roughEnabled    = inst->roughEnabled != 0;
	inst->aoEnabled       = inst->aoEnabled != 0;
	inst->blurReflEnabled = inst->blurReflEnabled != 0;
	inst->envEnabled      = inst->envEnabled != 0;
}

static const char *
parse_int(const char *s, int *val);

static void
init_defaults(PBRInst *inst)
{
	inst->ior             = 1.5;
	inst->roughEnabled    = 0;
	inst->roughAmount     = 20;
	inst->aoEnabled       = 0;
	inst->aoStrength      = 50;
	inst->metallic        = 0;
	inst->blurReflEnabled = 0;
	inst->blurReflSamples = 4;
	inst->blurReflAmount  = 30;
	inst->envEnabled      = 0;
	inst->envStrength     = 50;
	compute_f0(inst);
}

static void
load_v1_from_text(PBRInst *inst, const char *p)
{
	int v, skip;

	p = parse_int(p, &v); inst->ior = v / 1000.0;
	p = parse_int(p, &skip);
	p = parse_int(p, &skip);
	p = parse_int(p, &skip);
	p = parse_int(p, &skip);
	p = parse_int(p, &skip);
	p = parse_int(p, &inst->roughEnabled);
	p = parse_int(p, &inst->roughAmount);
	p = parse_int(p, &inst->aoEnabled);
	p = parse_int(p, &skip);
	p = parse_int(p, &skip);
	p = parse_int(p, &inst->aoStrength);
	p = parse_int(p, &inst->metallic);
	if (inst->metallic == 1) inst->metallic = 100;
	p = parse_int(p, &skip);
	p = parse_int(p, &inst->blurReflEnabled);
	p = parse_int(p, &inst->blurReflSamples);
	p = parse_int(p, &inst->blurReflAmount);
	p = parse_int(p, &inst->envEnabled);
	p = parse_int(p, &skip);
	p = parse_int(p, &inst->envStrength);
}

static void
load_v2_from_text(PBRInst *inst, const char *p)
{
	int v;

	p = parse_int(p, &v); inst->ior = v / 1000.0;
	p = parse_int(p, &inst->roughEnabled);
	p = parse_int(p, &inst->roughAmount);
	p = parse_int(p, &inst->aoEnabled);
	p = parse_int(p, &inst->aoStrength);
	p = parse_int(p, &inst->metallic);
	p = parse_int(p, &inst->blurReflEnabled);
	p = parse_int(p, &inst->blurReflSamples);
	p = parse_int(p, &inst->blurReflAmount);
	p = parse_int(p, &inst->envEnabled);
	p = parse_int(p, &inst->envStrength);
}

static int
load_v1_from_object(PBRInst *inst, const LWLoadState *ls)
{
	int v, skip;

	if (!spi_read_i32be(ls, &v)) return 0;
	inst->ior = v / 1000.0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &inst->roughEnabled)) return 0;
	if (!spi_read_i32be(ls, &inst->roughAmount)) return 0;
	if (!spi_read_i32be(ls, &inst->aoEnabled)) return 0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &inst->aoStrength)) return 0;
	if (!spi_read_i32be(ls, &inst->metallic)) return 0;
	if (inst->metallic == 1) inst->metallic = 100;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &inst->blurReflEnabled)) return 0;
	if (!spi_read_i32be(ls, &inst->blurReflSamples)) return 0;
	if (!spi_read_i32be(ls, &inst->blurReflAmount)) return 0;
	if (!spi_read_i32be(ls, &inst->envEnabled)) return 0;
	if (!spi_read_i32be(ls, &skip)) return 0;
	if (!spi_read_i32be(ls, &inst->envStrength)) return 0;

	return 1;
}

static int
load_v2_from_object(PBRInst *inst, const LWLoadState *ls)
{
	int v;

	if (!spi_read_i32be(ls, &v)) return 0;
	inst->ior = v / 1000.0;
	if (!spi_read_i32be(ls, &inst->roughEnabled)) return 0;
	if (!spi_read_i32be(ls, &inst->roughAmount)) return 0;
	if (!spi_read_i32be(ls, &inst->aoEnabled)) return 0;
	if (!spi_read_i32be(ls, &inst->aoStrength)) return 0;
	if (!spi_read_i32be(ls, &inst->metallic)) return 0;
	if (!spi_read_i32be(ls, &inst->blurReflEnabled)) return 0;
	if (!spi_read_i32be(ls, &inst->blurReflSamples)) return 0;
	if (!spi_read_i32be(ls, &inst->blurReflAmount)) return 0;
	if (!spi_read_i32be(ls, &inst->envEnabled)) return 0;
	if (!spi_read_i32be(ls, &inst->envStrength)) return 0;

	return 1;
}

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err)
{
	PBRInst *inst;
	XCALL_INIT;

	inst = (PBRInst *)plugin_alloc(sizeof(PBRInst));
	if (!inst) return 0;

	init_defaults(inst);

	if (!pow5_ready) {
		int j;
		for (j = 0; j <= 100; j++) {
			double t = j / 100.0;
			pow5_lut[j] = t * t * t * t * t;
		}
		for (j = 0; j < BLUR_MAX_SAMPLES; j++) {
			double a = j * 2.399963229728653;	/* golden angle */
			blur_disk[j][0] = cos_approx(a);
			blur_disk[j][1] = sin_approx(a);
		}
		pow5_ready = 1;
	}

	return inst;
}

XCALL_(static void)
Destroy(PBRInst *inst)
{
	XCALL_INIT;
	if (inst) plugin_free(inst);
}

XCALL_(static LWError)
Copy(PBRInst *from, PBRInst *to)
{
	XCALL_INIT;
	*to = *from;
	/* Animation globals belong to Init/Cleanup on each instance. */
	to->itemInfo = 0;
	to->lightInfo = 0;
	to->lightInfoV2 = 0;
	return 0;
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

XCALL_(static LWError)
Load(PBRInst *inst, const LWLoadState *ls)
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
		if (v >= 100) {
			load_v1_from_text(inst, buf);
		} else if (v == PBR_SCENE_VERSION) {
			load_v2_from_text(inst, p);
		} else {
			return 0;
		}
	} else {
		if (!spi_read_i32be(ls, &v))
			return 0;
		if (v == 1) {
			if (!load_v1_from_object(inst, ls))
				return 0;
		} else if (v == PBR_OBJECT_VERSION) {
			if (!load_v2_from_object(inst, ls))
				return 0;
		} else {
			return 0;
		}
	}

	clamp_settings(inst);
	compute_f0(inst);
	return 0;
}

XCALL_(static LWError)
Save(PBRInst *inst, const LWSaveState *ss)
{
	char buf[128];
	int pos = 0;
	XCALL_INIT;

	append_int(buf, &pos, PBR_SCENE_VERSION);
	append_int(buf, &pos, (int)(inst->ior * 1000.0));
	append_int(buf, &pos, inst->roughEnabled);
	append_int(buf, &pos, inst->roughAmount);
	append_int(buf, &pos, inst->aoEnabled);
	append_int(buf, &pos, inst->aoStrength);
	append_int(buf, &pos, inst->metallic);
	append_int(buf, &pos, inst->blurReflEnabled);
	append_int(buf, &pos, inst->blurReflSamples);
	append_int(buf, &pos, inst->blurReflAmount);
	append_int(buf, &pos, inst->envEnabled);
	append_int(buf, &pos, inst->envStrength);

	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
	} else {
		spi_write_i32be(ss, PBR_OBJECT_VERSION);
		spi_write_i32be(ss, (int)(inst->ior * 1000.0));
		spi_write_i32be(ss, inst->roughEnabled);
		spi_write_i32be(ss, inst->roughAmount);
		spi_write_i32be(ss, inst->aoEnabled);
		spi_write_i32be(ss, inst->aoStrength);
		spi_write_i32be(ss, inst->metallic);
		spi_write_i32be(ss, inst->blurReflEnabled);
		spi_write_i32be(ss, inst->blurReflSamples);
		spi_write_i32be(ss, inst->blurReflAmount);
		spi_write_i32be(ss, inst->envEnabled);
		spi_write_i32be(ss, inst->envStrength);
	}

	return 0;
}

XCALL_(static LWError)
Init(PBRInst *inst)
{
	XCALL_INIT;
	compute_f0(inst);
	inst->itemInfo = 0;
	inst->lightInfo = 0;
	inst->lightInfoV2 = 0;
	if (gfunc) {
		inst->itemInfo = (LWItemInfo *)(*gfunc)("LW Item Info", GFUSE_ACQUIRE);
		/* LW 5.5+ adds flags and range in LW Light Info 2; 5.00r only has
		 * the older global, whose struct ends before flags. */
		inst->lightInfo = (LWLightInfo *)(*gfunc)("LW Light Info 2", GFUSE_ACQUIRE);
		if (inst->lightInfo)
			inst->lightInfoV2 = 1;
		else
			inst->lightInfo = (LWLightInfo *)(*gfunc)("LW Light Info", GFUSE_ACQUIRE);
	}
	return 0;
}

XCALL_(static void)
Cleanup(PBRInst *inst)
{
	XCALL_INIT;
	if (inst->itemInfo && gfunc)
		(*gfunc)("LW Item Info", GFUSE_RELEASE);
	if (inst->lightInfo && gfunc)
		(*gfunc)(inst->lightInfoV2 ? "LW Light Info 2" : "LW Light Info",
		         GFUSE_RELEASE);
	inst->itemInfo = 0;
	inst->lightInfo = 0;
	inst->lightInfoV2 = 0;
}

XCALL_(static LWError)
NewTime(PBRInst *inst, LWFrame f, LWTime t)
{
	XCALL_INIT;
	inst->time = t;
	return 0;
}

XCALL_(static unsigned int)
Flags(PBRInst *inst)
{
	unsigned int f = 0;
	XCALL_INIT;

	if (inst->roughEnabled && inst->roughAmount > 0)
		f |= LWSHF_NORMAL;

	if (inst->metallic > 0)
		f |= LWSHF_MIRROR | LWSHF_DIFFUSE | LWSHF_SPECULAR;

	if (inst->aoEnabled && inst->aoStrength > 0)
		f |= LWSHF_DIFFUSE | LWSHF_LUMINOUS;

	if (inst->blurReflEnabled && inst->blurReflSamples > 0)
		f |= LWSHF_COLOR | LWSHF_LUMINOUS | LWSHF_DIFFUSE
		   | LWSHF_MIRROR | LWSHF_RAYTRACE;

	if (inst->envEnabled && inst->envStrength > 0)
		f |= LWSHF_LUMINOUS | LWSHF_COLOR;

	return f;
}

static double
dot3(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/*
 * Trace the blur rays and leave their average in blur.sum.
 *
 * The samples cover a disk of radius spread (tangent of the cone angle)
 * around the mirror direction: sample i sits in the equal-area ring
 * sqrt(i/n)..sqrt((i+1)/n) at a golden-angle step. Each point rotates the
 * pattern and moves the samples within their rings, so neighbouring pixels
 * use different rays and antialiasing passes average them.
 */
static void
trace_blur(PBRInst *inst, ShaderAccess *sa)
{
	double d, spread, cr, sr, jit, rad, u, v;
	int n, i, k;

	n = inst->blurReflSamples;
	if (n > BLUR_MAX_SAMPLES) n = BLUR_MAX_SAMPLES;
	spread = inst->blurReflAmount / 200.0;
	if (spread <= 0.0) n = 1;

	for (k = 0; k < 3; k++)
		blur.dir[k] = sa->wPos[k] - sa->raySource[k];
	vec_normalize(blur.dir);

	/* Reflect off the side that faces the viewer. */
	d = dot3(blur.dir, sa->wNorm);
	for (k = 0; k < 3; k++)
		blur.n[k] = d > 0.0 ? -sa->wNorm[k] : sa->wNorm[k];
	d = dot3(blur.dir, blur.n);
	for (k = 0; k < 3; k++)
		blur.refl[k] = blur.dir[k] - 2.0 * d * blur.n[k];
	vec_normalize(blur.refl);

	if (blur.refl[0] < 0.9 && blur.refl[0] > -0.9) {
		blur.t[0] = 0.0;
		blur.t[1] = -blur.refl[2];
		blur.t[2] = blur.refl[1];
	} else {
		blur.t[0] = blur.refl[2];
		blur.t[1] = 0.0;
		blur.t[2] = -blur.refl[0];
	}
	vec_normalize(blur.t);
	blur.b[0] = blur.refl[1] * blur.t[2] - blur.refl[2] * blur.t[1];
	blur.b[1] = blur.refl[2] * blur.t[0] - blur.refl[0] * blur.t[2];
	blur.b[2] = blur.refl[0] * blur.t[1] - blur.refl[1] * blur.t[0];

	cr = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 50000u);
	sr = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 60000u);
	d = sqrt(cr * cr + sr * sr);
	if (d < 0.001) {
		cr = 1.0;
		sr = 0.0;
	} else {
		cr /= d;
		sr /= d;
	}
	jit = (hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 70000u) + 1.0) * 0.5;
	if (jit < 0.0) jit = 0.0;
	if (jit > 0.999) jit = 0.999;

	for (k = 0; k < 3; k++) {
		blur.pos[k] = sa->wPos[k] + blur.n[k] * 0.001;
		blur.sum[k] = 0.0;
	}

	blur_trace_depth++;
	for (i = 0; i < n; i++) {
		rad = spread * sqrt((i + jit) / n);
		u = (blur_disk[i][0] * cr - blur_disk[i][1] * sr) * rad;
		v = (blur_disk[i][1] * cr + blur_disk[i][0] * sr) * rad;
		for (k = 0; k < 3; k++)
			blur.dir[k] = blur.refl[k] + u * blur.t[k] + v * blur.b[k];
		vec_normalize(blur.dir);

		/* A ray below the surface would hit the object itself. */
		d = dot3(blur.dir, blur.n);
		if (d < 0.0) {
			for (k = 0; k < 3; k++)
				blur.dir[k] -= 2.0 * d * blur.n[k];
		}

		blur.col[0] = blur.col[1] = blur.col[2] = 0.0;
		(*sa->rayTrace)(blur.pos, blur.dir, blur.col);
		for (k = 0; k < 3; k++)
			blur.sum[k] += blur.col[k];
	}
	blur_trace_depth--;

	for (k = 0; k < 3; k++)
		blur.sum[k] /= n;
}

/*
 * LightWave 5 adds the mirror reflection on top of the lit surface:
 * colour * (luminous + diffuse * light) + specular + mirror * reflection
 * (measured in Layout 5.00r). A shader cannot add colour after lighting,
 * so light the surface here and hand LightWave the result as a fully
 * luminous colour. LightWave still adds the specular highlight.
 */
static void
compose_blur(PBRInst *inst, ShaderAccess *sa)
{
	LWItemID light;
	double len, ndotl, v;
	int k;

	blur.light[0] = blur.light[1] = blur.light[2] = 0.0;
	if (inst->lightInfo)
		(*inst->lightInfo->ambient)(inst->time, blur.light);

	/* Shadow rays can evaluate shaders too; keep those off blur. */
	blur_trace_depth++;
	for (light = (*inst->itemInfo->first)(LWI_LIGHT, LWITEM_NULL); light;
	     light = (*inst->itemInfo->next)(light)) {
		if (inst->lightInfoV2
		    && ((*inst->lightInfo->flags)(light) & LWLFL_NO_DIFFUSE))
			continue;
		if (!(*sa->illuminate)(light, sa->wPos, blur.dir, blur.col))
			continue;
		len = sqrt(dot3(blur.dir, blur.dir));
		if (len < 0.00001)
			continue;
		/* blur.dir runs from the light to the point. */
		ndotl = -dot3(blur.dir, blur.n) / len;
		if (ndotl <= 0.0)
			continue;
		for (k = 0; k < 3; k++)
			blur.light[k] += blur.col[k] * ndotl;
	}
	blur_trace_depth--;

	for (k = 0; k < 3; k++) {
		v = sa->color[k] * (sa->luminous + sa->diffuse * blur.light[k])
		  + sa->mirror * blur.sum[k];
		if (v < 0.0) v = 0.0;
		if (v > 1.0) v = 1.0;
		sa->color[k] = v;
	}
	sa->luminous = 1.0;
	sa->diffuse = 0.0;
	sa->mirror = 0.0;
}

XCALL_(static void)
Evaluate(PBRInst *inst, ShaderAccess *sa)
{
	double cosAngle, oneMinusCos, fresnel;
	int blurred = 0;

	XCALL_INIT;

	cosAngle = sa->cosine;
	if (cosAngle < 0.0) cosAngle = -cosAngle;
	if (cosAngle > 1.0) cosAngle = 1.0;
	oneMinusCos = 1.0 - cosAngle;

	/* --- Roughness: perturb surface normal --- */
	if (inst->roughEnabled && inst->roughAmount > 0) {
		double scale = inst->roughAmount / 1000.0;
		double nx = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 0u) * scale;
		double ny = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 7919u) * scale;
		double nz = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 15737u) * scale;

		sa->wNorm[0] += nx;
		sa->wNorm[1] += ny;
		sa->wNorm[2] += nz;
		vec_normalize(sa->wNorm);
	}

	/* --- Metallic: blend between dielectric and metallic behavior --- */
	if (inst->metallic > 0) {
		double met = inst->metallic / 100.0;
		int lutIdx = (int)(oneMinusCos * 100.0);
		if (lutIdx < 0) lutIdx = 0;
		if (lutIdx > 100) lutIdx = 100;
		fresnel = inst->f0 + (1.0 - inst->f0) * pow5_lut[lutIdx];
		if (fresnel > 1.0) fresnel = 1.0;

		sa->mirror = sa->mirror + (1.0 - sa->mirror) * fresnel * met;
		sa->diffuse *= 1.0 - 0.95 * met;
		sa->specular = sa->specular + (1.0 - sa->specular) * fresnel * met;
	}

	/* --- Ambient Occlusion: normal-based approximation --- */
	if (inst->aoEnabled && inst->aoStrength > 0) {
		double aoStr = inst->aoStrength / 100.0;
		double skyDot = sa->wNorm[1];
		double edgeDot = sa->cosine;
		double aoFactor;

		if (skyDot < -1.0) skyDot = -1.0;
		if (skyDot > 1.0) skyDot = 1.0;
		if (edgeDot < 0.0) edgeDot = -edgeDot;

		aoFactor = 1.0 - aoStr * (1.0 - (skyDot + 1.0) * 0.4)
		                        * (0.5 + 0.5 * edgeDot);
		if (aoFactor < 0.0) aoFactor = 0.0;
		if (aoFactor > 1.0) aoFactor = 1.0;

		sa->diffuse *= aoFactor;
		sa->luminous *= aoFactor;
	}

	/* --- Blurred Reflections: replace LightWave's single mirror ray --- */
	if (inst->blurReflEnabled && inst->blurReflSamples > 0
	    && blur_trace_depth == 0 && sa->mirror > 0.001
	    && sa->rayTrace && sa->illuminate && inst->itemInfo) {
		trace_blur(inst, sa);
		blurred = 1;
	}

	/* --- Environment Light: normal-based ambient boost --- */
	if (inst->envEnabled && inst->envStrength > 0) {
		double envStr = inst->envStrength / 100.0;
		double skyDot = sa->wNorm[1];
		double envFactor;
		if (skyDot < 0.0) skyDot = 0.0;
		envFactor = (0.3 + 0.7 * skyDot) * envStr;
		sa->luminous += envFactor * 0.5;
		if (sa->luminous > 1.0) sa->luminous = 1.0;
		sa->color[0] += envFactor * sa->color[0] * 0.3;
		sa->color[1] += envFactor * sa->color[1] * 0.3;
		sa->color[2] += envFactor * sa->color[2] * 0.3;
		if (sa->color[0] > 1.0) sa->color[0] = 1.0;
		if (sa->color[1] > 1.0) sa->color[1] = 1.0;
		if (sa->color[2] > 1.0) sa->color[2] = 1.0;
	}

	/* Last, so the lighting sees the final colour, diffuse and luminous. */
	if (blurred)
		compose_blur(inst, sa);
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static const char *blurSampleItems[] = { "Off", "2", "4", "8", "16", 0 };
static int blurSampleValues[] = { 0, 2, 4, 8, 16 };

enum {
	PC_IOR, PC_METALLIC, PC_ROUGH_EN, PC_ROUGH_AMT, PC_AO_EN, PC_AO_STR,
	PC_BLUR_SAMP, PC_BLUR_AMT, PC_ENV_EN, PC_ENV_STR, PC_COUNT
};

/* Panel state lives in static data: the panel is modal, and the LWPanels
 * macros need a desc and values named desc/ival/fval in scope. */
static LWControl *panelCon[PC_COUNT];
static LWPanControlDesc desc;
static LWValue ival = { LWT_INTEGER };
static LWValue fval = { LWT_FLOAT };

/* The CON_* macros all return the shared ival, so two of them in one
 * expression read the same value; this returns a copy instead. */
static int
con_get(LWControl *ctl, int tag)
{
	(*ctl->get)(ctl, tag, &ival);
	return ival.intv.value;
}

/* LWPanels stacks the controls in one column with left-aligned labels.
 * Right-align the labels so every field, slider, popup and check box
 * starts at the same x, then fit the panel width to the column. */
static void
align_panel(LWPanelFuncs *panl, LWPanelID pan)
{
	int i, x, y, label, hot = 0, right = 0, edge, margin;

	margin = con_get(panelCon[0], CTL_X);
	for (i = 0; i < PC_COUNT; i++) {
		label = con_get(panelCon[i], CTL_HOTX) - con_get(panelCon[i], CTL_X);
		if (label > hot) hot = label;
	}
	for (i = 0; i < PC_COUNT; i++) {
		label = con_get(panelCon[i], CTL_HOTX) - con_get(panelCon[i], CTL_X);
		x = margin + hot - label;
		y = con_get(panelCon[i], CTL_Y);
		MOVE_CON(panelCon[i], x, y);
		edge = con_get(panelCon[i], CTL_X) + con_get(panelCon[i], CTL_W);
		if (edge > right) right = edge;
	}
	PAN_SETW(panl, pan, right + margin);
}

XCALL_(static int)
Interface(
	long       version,
	GlobalFunc *global,
	PBRInst    *inst,
	void       *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl   **c = panelCon;
	int           blurIdx;
	char          infoBuf[80];

	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);

	if (panl) {
		pan = PAN_CREATE(panl, "PBR v" PLUGIN_VERSION " (c) D. Panokostas");
		if (!pan) goto fallback;

		c[PC_IOR]       = FLOAT_CTL(panl, pan, "IOR");
		c[PC_METALLIC]  = SLIDER_CTL(panl, pan, "Metallic", 150, 0, 100);
		c[PC_ROUGH_EN]  = BOOL_CTL(panl, pan, "Roughness");
		c[PC_ROUGH_AMT] = SLIDER_CTL(panl, pan, "Roughness Amount", 150, 0, 100);
		c[PC_AO_EN]     = BOOL_CTL(panl, pan, "AO");
		c[PC_AO_STR]    = SLIDER_CTL(panl, pan, "AO Strength", 150, 0, 100);
		c[PC_BLUR_SAMP] = POPUP_CTL(panl, pan, "Blur Refl", blurSampleItems);
		c[PC_BLUR_AMT]  = SLIDER_CTL(panl, pan, "Blur Spread", 150, 0, 100);
		c[PC_ENV_EN]    = BOOL_CTL(panl, pan, "Env Light");
		c[PC_ENV_STR]   = SLIDER_CTL(panl, pan, "Env Strength", 150, 0, 100);
		align_panel(panl, pan);

		/* Set values */
		SET_FLOAT(c[PC_IOR], inst->ior);
		SET_INT(c[PC_METALLIC], inst->metallic);
		SET_INT(c[PC_ROUGH_EN], inst->roughEnabled);
		SET_INT(c[PC_ROUGH_AMT], inst->roughAmount);
		SET_INT(c[PC_AO_EN], inst->aoEnabled);
		SET_INT(c[PC_AO_STR], inst->aoStrength);
		blurIdx = inst->blurReflEnabled
		        ? ((inst->blurReflSamples <= 2) ? 1 : (inst->blurReflSamples <= 4) ? 2
		          : (inst->blurReflSamples <= 8) ? 3 : 4)
		        : 0;
		SET_INT(c[PC_BLUR_SAMP], blurIdx);
		SET_INT(c[PC_BLUR_AMT], inst->blurReflAmount);
		SET_INT(c[PC_ENV_EN], inst->envEnabled);
		SET_INT(c[PC_ENV_STR], inst->envStrength);

		if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
			GET_FLOAT(c[PC_IOR], inst->ior);
			GET_INT(c[PC_METALLIC], inst->metallic);
			GET_INT(c[PC_ROUGH_EN], inst->roughEnabled);
			GET_INT(c[PC_ROUGH_AMT], inst->roughAmount);
			GET_INT(c[PC_AO_EN], inst->aoEnabled);
			GET_INT(c[PC_AO_STR], inst->aoStrength);
			GET_INT(c[PC_BLUR_SAMP], blurIdx);
			inst->blurReflEnabled = (blurIdx > 0) ? 1 : 0;
			inst->blurReflSamples = (blurIdx > 0 && blurIdx < 5) ? blurSampleValues[blurIdx] : 8;
			GET_INT(c[PC_BLUR_AMT], inst->blurReflAmount);
			GET_INT(c[PC_ENV_EN], inst->envEnabled);
			GET_INT(c[PC_ENV_STR], inst->envStrength);

			clamp_settings(inst);

			compute_f0(inst);
		}

		PAN_KILL(panl, pan);
		return AFUNC_OK;
	}

fallback:
	if (!msg)
		return AFUNC_BADGLOBAL;

	{
		char nb[12];
		int iorFixed = (int)(inst->ior * 100.0);
		strcpy(infoBuf, "IOR:");
		int_to_str(iorFixed / 100, nb, 12); strcat(infoBuf, nb);
		strcat(infoBuf, ".");
		int_to_str(iorFixed % 100, nb, 12);
		if (iorFixed % 100 < 10) strcat(infoBuf, "0");
		strcat(infoBuf, nb);
		if (inst->metallic) strcat(infoBuf, " Metal");
		if (inst->roughEnabled) {
			strcat(infoBuf, " Rough:");
			int_to_str(inst->roughAmount, nb, 12);
			strcat(infoBuf, nb);
		}
		if (inst->aoEnabled) strcat(infoBuf, " +AO");
		if (inst->blurReflEnabled) strcat(infoBuf, " +Blur");
		if (inst->envEnabled) strcat(infoBuf, " +Env");
	}
	(*msg->info)("PBR Shader", infoBuf);
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
	ShaderHandler *h = (ShaderHandler *)local;
	XCALL_INIT;

	if (version < 1)
		return AFUNC_BADVERSION;

	h->create   = (void *)Create;
	h->destroy  = (void *)Destroy;
	h->load     = (void *)Load;
	h->save     = (void *)Save;
	h->copy     = (void *)Copy;
	h->init     = (void *)Init;
	h->cleanup  = (void *)Cleanup;
	h->newTime  = (void *)NewTime;
	h->evaluate = (void *)Evaluate;
	h->flags    = (void *)Flags;

	gfunc = global;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!msg)
		return AFUNC_BADGLOBAL;

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Server description
 * ---------------------------------------------------------------- */

ServerRecord ServerDesc[] = {
	{ "ShaderHandler",   "PBR",
	  (ActivateFunc *)Activate },
	{ "ShaderInterface", "PBR",
	  (ActivateFunc *)Interface },
	{ 0 }
};
