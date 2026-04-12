/*
 * MOTION.C -- Procedural Motion Plugin for LightWave 3D
 *
 * Adds procedural animation to any item (object, light, camera)
 * without keyframing.  Supports three modes:
 *
 *   Wiggle — smooth random noise on position and/or rotation
 *   Bounce — sinusoidal oscillation
 *   Shake  — decaying noise burst (camera shake)
 *
 * The procedural offset is added on top of existing keyframed motion,
 * so users can still animate normally and layer this effect over it.
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
extern double sin(double);
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
 * Smooth value noise (1D)
 *
 * Hash-based with smoothstep interpolation.  Returns -1.0 to 1.0.
 * ---------------------------------------------------------------- */

static double
value_noise(double t, unsigned int seed)
{
	int ti;
	unsigned int h1, h2;
	double v1, v2, frac, s;

	ti = (int)t;
	if (t < 0.0 && t != (double)ti) ti--;

	h1 = ((unsigned int)ti * 2654435761u) ^ seed;
	h2 = ((unsigned int)(ti + 1) * 2654435761u) ^ seed;

	h1 = (h1 ^ (h1 >> 13)) * 1274126177u;
	h2 = (h2 ^ (h2 >> 13)) * 1274126177u;

	v1 = ((double)(h1 & 0x7FFFu) / (double)0x3FFFu) - 1.0;
	v2 = ((double)(h2 & 0x7FFFu) / (double)0x3FFFu) - 1.0;

	frac = t - (double)ti;
	if (frac < 0.0) frac = 0.0;
	if (frac > 1.0) frac = 1.0;
	s = frac * frac * (3.0 - 2.0 * frac);   /* smoothstep */

	return v1 + (v2 - v1) * s;
}

/* Multi-octave noise for richer wiggle */
static double
noise_fbm(double t, unsigned int seed, int octaves)
{
	double val = 0.0, amp = 1.0, freq = 1.0, total = 0.0;
	int i;
	for (i = 0; i < octaves; i++) {
		val += value_noise(t * freq, seed + (unsigned int)i * 7919u) * amp;
		total += amp;
		amp *= 0.5;
		freq *= 2.0;
	}
	return val / total;
}

/* ----------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------- */

#define PI  3.14159265358979

#define MODE_WIGGLE  0
#define MODE_BOUNCE  1
#define MODE_SHAKE   2
#define NUM_MODES    3

#define MOTION_OBJECT_VERSION  1

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

typedef struct {
	LWItemID  itemID;

	int       mode;            /* MODE_WIGGLE / BOUNCE / SHAKE */

	/* Amplitude (stored as mm * 100, so 100 = 1.0 unit) */
	int       ampX, ampY, ampZ;

	/* Frequency (cycles per second * 10, so 10 = 1.0 Hz) */
	int       frequency;

	/* Affect flags */
	int       affectPos;       /* 1 = modify position */
	int       affectRot;       /* 1 = modify rotation */

	/* Rotation amplitude in degrees * 10 (so 50 = 5.0 degrees) */
	int       rotAmpX, rotAmpY, rotAmpZ;

	/* Shake: start frame and decay rate (0-100) */
	int       shakeStart;
	int       shakeDecay;      /* 0-100: higher = faster decay */

	/* Noise octaves for wiggle (1-4) */
	int       octaves;
} MotionInst;

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static MessageFuncs *msg;

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err, LWItemID item)
{
	MotionInst *inst;
	XCALL_INIT;

	inst = (MotionInst *)plugin_alloc(sizeof(MotionInst));
	if (!inst) return 0;

	inst->itemID    = item;
	inst->mode      = MODE_WIGGLE;
	inst->ampX      = 10;   /* 0.1 units */
	inst->ampY      = 10;
	inst->ampZ      = 10;
	inst->frequency = 10;   /* 1.0 Hz */
	inst->affectPos = 1;
	inst->affectRot = 0;
	inst->rotAmpX   = 50;   /* 5.0 degrees */
	inst->rotAmpY   = 50;
	inst->rotAmpZ   = 50;
	inst->shakeStart = 0;
	inst->shakeDecay = 50;
	inst->octaves    = 2;

	return inst;
}

XCALL_(static void)
Destroy(MotionInst *inst)
{
	XCALL_INIT;
	if (inst) plugin_free(inst);
}

XCALL_(static LWError)
Copy(MotionInst *from, MotionInst *to, LWItemID item)
{
	XCALL_INIT;
	*to = *from;
	to->itemID = item;
	return 0;
}

XCALL_(static LWError)
Load(MotionInst *inst, const LWLoadState *ls)
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
		if (v != MOTION_OBJECT_VERSION) return 0;
		p = parse_int(p, &inst->mode);
		p = parse_int(p, &inst->ampX);
		p = parse_int(p, &inst->ampY);
		p = parse_int(p, &inst->ampZ);
		p = parse_int(p, &inst->frequency);
		p = parse_int(p, &inst->affectPos);
		p = parse_int(p, &inst->affectRot);
		p = parse_int(p, &inst->rotAmpX);
		p = parse_int(p, &inst->rotAmpY);
		p = parse_int(p, &inst->rotAmpZ);
		p = parse_int(p, &inst->shakeStart);
		p = parse_int(p, &inst->shakeDecay);
		p = parse_int(p, &inst->octaves);
	} else {
		if (!spi_read_i32be(ls, &v) || v != MOTION_OBJECT_VERSION)
			return 0;
		if (!spi_read_i32be(ls, &inst->mode)) return 0;
		if (!spi_read_i32be(ls, &inst->ampX)) return 0;
		if (!spi_read_i32be(ls, &inst->ampY)) return 0;
		if (!spi_read_i32be(ls, &inst->ampZ)) return 0;
		if (!spi_read_i32be(ls, &inst->frequency)) return 0;
		if (!spi_read_i32be(ls, &inst->affectPos)) return 0;
		if (!spi_read_i32be(ls, &inst->affectRot)) return 0;
		if (!spi_read_i32be(ls, &inst->rotAmpX)) return 0;
		if (!spi_read_i32be(ls, &inst->rotAmpY)) return 0;
		if (!spi_read_i32be(ls, &inst->rotAmpZ)) return 0;
		if (!spi_read_i32be(ls, &inst->shakeStart)) return 0;
		if (!spi_read_i32be(ls, &inst->shakeDecay)) return 0;
		if (!spi_read_i32be(ls, &inst->octaves)) return 0;
	}

	if (inst->octaves < 1) inst->octaves = 1;
	if (inst->octaves > 4) inst->octaves = 4;
	if (inst->frequency < 1) inst->frequency = 1;
	if (inst->frequency > 1000) inst->frequency = 1000;

	return 0;
}

XCALL_(static LWError)
Save(MotionInst *inst, const LWSaveState *ss)
{
	char buf[128];
	int pos = 0;
	XCALL_INIT;

	append_int(buf, &pos, MOTION_OBJECT_VERSION);
	append_int(buf, &pos, inst->mode);
	append_int(buf, &pos, inst->ampX);
	append_int(buf, &pos, inst->ampY);
	append_int(buf, &pos, inst->ampZ);
	append_int(buf, &pos, inst->frequency);
	append_int(buf, &pos, inst->affectPos);
	append_int(buf, &pos, inst->affectRot);
	append_int(buf, &pos, inst->rotAmpX);
	append_int(buf, &pos, inst->rotAmpY);
	append_int(buf, &pos, inst->rotAmpZ);
	append_int(buf, &pos, inst->shakeStart);
	append_int(buf, &pos, inst->shakeDecay);
	append_int(buf, &pos, inst->octaves);

	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
	} else {
		spi_write_i32be(ss, MOTION_OBJECT_VERSION);
		spi_write_i32be(ss, inst->mode);
		spi_write_i32be(ss, inst->ampX);
		spi_write_i32be(ss, inst->ampY);
		spi_write_i32be(ss, inst->ampZ);
		spi_write_i32be(ss, inst->frequency);
		spi_write_i32be(ss, inst->affectPos);
		spi_write_i32be(ss, inst->affectRot);
		spi_write_i32be(ss, inst->rotAmpX);
		spi_write_i32be(ss, inst->rotAmpY);
		spi_write_i32be(ss, inst->rotAmpZ);
		spi_write_i32be(ss, inst->shakeStart);
		spi_write_i32be(ss, inst->shakeDecay);
		spi_write_i32be(ss, inst->octaves);
	}
	return 0;
}

/* ----------------------------------------------------------------
 * Evaluate — compute procedural motion offset
 * ---------------------------------------------------------------- */

XCALL_(static void)
Evaluate(MotionInst *inst, const ItemMotionAccess *ma)
{
	double t, freq, env;
	XCALL_INIT;

	freq = inst->frequency / 10.0;   /* cycles per second */
	t = ma->time * freq;             /* scaled time */

	/* Envelope: mode-dependent amplitude scaling */
	env = 1.0;

	if (inst->mode == MODE_SHAKE) {
		/* Decay envelope: starts at shakeStart frame,
		 * decays exponentially. Before shakeStart → no effect */
		double decay = inst->shakeDecay / 100.0;
		double startTime = (double)inst->shakeStart / 30.0; /* assume 30fps */
		double elapsed = ma->time - startTime;
		if (elapsed < 0.0) {
			env = 0.0;
		} else {
			/* Exponential decay: faster decay value = faster falloff */
			double rate = 1.0 + decay * 9.0;   /* 1.0 to 10.0 */
			env = 1.0;
			{
				/* Manual exp(-rate * elapsed) using repeated multiply */
				double step = 0.01;
				double tt = elapsed;
				while (tt > step) {
					env *= (1.0 - rate * step);
					if (env < 0.001) { env = 0.0; break; }
					tt -= step;
				}
				if (env > 0.001)
					env *= (1.0 - rate * tt);
				if (env < 0.0) env = 0.0;
			}
		}
	}

	/* ---- Position ---- */
	if (inst->affectPos) {
		double pos[3];
		(*ma->getParam)(LWIP_POSITION, ma->time, pos);

		if (inst->mode == MODE_BOUNCE) {
			double phase;
			phase = t * 2.0 * PI;
			pos[0] += sin(phase)                * (inst->ampX / 100.0) * env;
			pos[1] += sin(phase + PI * 0.333)   * (inst->ampY / 100.0) * env;
			pos[2] += sin(phase + PI * 0.667)   * (inst->ampZ / 100.0) * env;
		} else {
			/* Wiggle or Shake — noise-based */
			int oct = inst->octaves;
			pos[0] += noise_fbm(t, 10000u, oct) * (inst->ampX / 100.0) * env;
			pos[1] += noise_fbm(t, 20000u, oct) * (inst->ampY / 100.0) * env;
			pos[2] += noise_fbm(t, 30000u, oct) * (inst->ampZ / 100.0) * env;
		}

		(*ma->setParam)(LWIP_POSITION, pos);
	}

	/* ---- Rotation ---- */
	if (inst->affectRot) {
		double rot[3];
		double degToRad = PI / 180.0;

		(*ma->getParam)(LWIP_ROTATION, ma->time, rot);

		if (inst->mode == MODE_BOUNCE) {
			double phase = t * 2.0 * PI;
			rot[0] += sin(phase)                * (inst->rotAmpX / 10.0) * degToRad * env;
			rot[1] += sin(phase + PI * 0.333)   * (inst->rotAmpY / 10.0) * degToRad * env;
			rot[2] += sin(phase + PI * 0.667)   * (inst->rotAmpZ / 10.0) * degToRad * env;
		} else {
			int oct = inst->octaves;
			rot[0] += noise_fbm(t, 40000u, oct) * (inst->rotAmpX / 10.0) * degToRad * env;
			rot[1] += noise_fbm(t, 50000u, oct) * (inst->rotAmpY / 10.0) * degToRad * env;
			rot[2] += noise_fbm(t, 60000u, oct) * (inst->rotAmpZ / 10.0) * degToRad * env;
		}

		(*ma->setParam)(LWIP_ROTATION, rot);
	}
}

/* ----------------------------------------------------------------
 * Description line
 * ---------------------------------------------------------------- */

static const char *modeNames[] = { "Wiggle", "Bounce", "Shake" };

XCALL_(static const char *)
DescLn(MotionInst *inst)
{
	XCALL_INIT;
	if (inst->mode >= 0 && inst->mode < NUM_MODES)
		return modeNames[inst->mode];
	return "Motion";
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static const char *modeItems[] = { "Wiggle", "Bounce", "Shake", 0 };
static const char *octaveItems[] = { "1", "2", "3", "4", 0 };

XCALL_(static int)
Interface(
	long       version,
	GlobalFunc *global,
	MotionInst *inst,
	void       *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl    *ctlMode, *ctlAmpX, *ctlAmpY, *ctlAmpZ, *ctlFreq;
	LWControl    *ctlPos, *ctlRot;
	LWControl    *ctlRotX, *ctlRotY, *ctlRotZ;
	LWControl    *ctlShakeStart, *ctlDecay, *ctlOctaves;

	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;

	msg  = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);

	if (!panl) {
		if (msg) {
			const char *mn = (inst->mode >= 0 && inst->mode < NUM_MODES)
			               ? modeNames[inst->mode] : "?";
			(*msg->info)("Motion", mn);
		}
		return AFUNC_OK;
	}

	{
		static LWPanControlDesc desc;
		static LWValue ival = {LWT_INTEGER};

		pan = PAN_CREATE(panl, "Motion v" PLUGIN_VERSION
		                       " (c) D. Panokostas");
		if (!pan) return AFUNC_OK;

		ctlMode  = POPUP_CTL(panl, pan, "Mode", modeItems);
		ctlFreq  = INT_CTL(panl, pan, "Speed (x10 Hz)");
		ctlOctaves = POPUP_CTL(panl, pan, "Octaves", octaveItems);
		ctlPos   = BOOL_CTL(panl, pan, "Affect Position");
		ctlAmpX  = INT_CTL(panl, pan, "Pos Amp X");
		ctlAmpY  = INT_CTL(panl, pan, "Pos Amp Y");
		ctlAmpZ  = INT_CTL(panl, pan, "Pos Amp Z");
		ctlRot   = BOOL_CTL(panl, pan, "Affect Rotation");
		ctlRotX  = INT_CTL(panl, pan, "Rot Amp H");
		ctlRotY  = INT_CTL(panl, pan, "Rot Amp P");
		ctlRotZ  = INT_CTL(panl, pan, "Rot Amp B");
		ctlShakeStart = INT_CTL(panl, pan, "Shake Start Fr");
		ctlDecay = SLIDER_CTL(panl, pan, "Shake Decay", 150, 0, 100);

		SET_INT(ctlMode, inst->mode);
		SET_INT(ctlFreq, inst->frequency);
		SET_INT(ctlOctaves, inst->octaves - 1);
		SET_INT(ctlPos, inst->affectPos);
		SET_INT(ctlAmpX, inst->ampX);
		SET_INT(ctlAmpY, inst->ampY);
		SET_INT(ctlAmpZ, inst->ampZ);
		SET_INT(ctlRot, inst->affectRot);
		SET_INT(ctlRotX, inst->rotAmpX);
		SET_INT(ctlRotY, inst->rotAmpY);
		SET_INT(ctlRotZ, inst->rotAmpZ);
		SET_INT(ctlShakeStart, inst->shakeStart);
		SET_INT(ctlDecay, inst->shakeDecay);

		if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
			int idx;
			GET_INT(ctlMode, inst->mode);
			GET_INT(ctlFreq, inst->frequency);
			GET_INT(ctlOctaves, idx);
			inst->octaves = idx + 1;
			GET_INT(ctlPos, inst->affectPos);
			GET_INT(ctlAmpX, inst->ampX);
			GET_INT(ctlAmpY, inst->ampY);
			GET_INT(ctlAmpZ, inst->ampZ);
			GET_INT(ctlRot, inst->affectRot);
			GET_INT(ctlRotX, inst->rotAmpX);
			GET_INT(ctlRotY, inst->rotAmpY);
			GET_INT(ctlRotZ, inst->rotAmpZ);
			GET_INT(ctlShakeStart, inst->shakeStart);
			GET_INT(ctlDecay, inst->shakeDecay);

			/* Clamp */
			if (inst->mode < 0) inst->mode = 0;
			if (inst->mode >= NUM_MODES) inst->mode = NUM_MODES - 1;
			if (inst->frequency < 1) inst->frequency = 1;
			if (inst->frequency > 1000) inst->frequency = 1000;
			if (inst->octaves < 1) inst->octaves = 1;
			if (inst->octaves > 4) inst->octaves = 4;
			if (inst->shakeDecay < 0) inst->shakeDecay = 0;
			if (inst->shakeDecay > 100) inst->shakeDecay = 100;
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
	ItemMotionHandler *h = (ItemMotionHandler *)local;
	XCALL_INIT;

	if (version < 1)
		return AFUNC_BADVERSION;

	h->create   = (void *)Create;
	h->destroy  = (void *)Destroy;
	h->load     = (void *)Load;
	h->save     = (void *)Save;
	h->copy     = (void *)Copy;
	h->evaluate = (void *)Evaluate;
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
	{ "ItemMotionHandler",   "Motion",
	  (ActivateFunc *)Activate },
	{ "ItemMotionInterface", "Motion",
	  (ActivateFunc *)Interface },
	{ 0 }
};
