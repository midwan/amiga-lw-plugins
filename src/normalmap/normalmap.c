/*
 * NORMALMAP.C -- Normal Map Shader Plugin for LightWave 3D
 *
 * Samples an image loaded in LightWave's image list and uses it as a
 * tangent-space normal map to perturb surface normals at render time.
 * Supports planar projection (XY/XZ/YZ) with tiling, offset, and
 * adjustable strength.  Pairs well with the PBR and Fresnel plugins
 * for a full modern material stack on LightWave 5.x.
 *
 * Uses AllocMem/FreeMem and custom helpers -- no libnix runtime.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <lwmath.h>
#include <safe_pluginio.h>

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
 * Integer/string helpers (avoid libnix sprintf/sscanf)
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
 * Math helpers
 * ---------------------------------------------------------------- */

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

static double
my_floor(double x)
{
	int i = (int)x;
	if (x < 0.0 && x != (double)i) i--;
	return (double)i;
}

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

#define NMAP_MAX_NAME 128

#define NMAP_PROJ_XY   0   /* Front — U from X, V from Y */
#define NMAP_PROJ_XZ   1   /* Top   — U from X, V from Z */
#define NMAP_PROJ_YZ   2   /* Side  — U from Y, V from Z */
#define NMAP_NUM_PROJ  3

#define NMAP_OBJECT_VERSION  1

typedef struct {
	LWImageID  imageID;              /* runtime only (not saved) */
	char       imageName[NMAP_MAX_NAME]; /* persisted name for reload */
	int        projection;           /* NMAP_PROJ_* */
	int        tileX;               /* tiling factor X  (100 = 1.0) */
	int        tileY;               /* tiling factor Y  (100 = 1.0) */
	int        strength;            /* 0 - 100 */
	int        flipY;               /* flip green channel (DX normals) */

	/* Pre-loaded image data (populated in Init, freed in Cleanup).
	 * We copy the image into our own buffer so Evaluate never
	 * calls any LW globals — those are not safe during rendering. */
	unsigned char *mapData;          /* RGB triplets, row-major */
	int            mapW, mapH;
} NormalMapInst;

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static MessageFuncs *msg;
static GlobalFunc   *gfunc;

/* ----------------------------------------------------------------
 * Image list helpers
 * ---------------------------------------------------------------- */

/* Try to resolve an image by its display name (uses gfunc transient) */
static LWImageID
find_image_by_name(const char *name)
{
	LWImageList *il;
	LWImageID id;

	if (!gfunc || !name || !name[0]) return 0;

	il = (LWImageList *)(*gfunc)("LW Image List", GFUSE_TRANSIENT);
	if (!il) return 0;

	id = (*il->first)();
	while (id) {
		const char *n = (*il->name)(id);
		if (n && strcmp(n, name) == 0)
			return id;
		id = (*il->next)(id);
	}
	return 0;
}

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err)
{
	NormalMapInst *inst;
	XCALL_INIT;

	inst = (NormalMapInst *)plugin_alloc(sizeof(NormalMapInst));
	if (!inst) return 0;

	inst->imageID    = 0;
	inst->imageName[0] = '\0';
	inst->projection = NMAP_PROJ_XY;
	inst->tileX      = 100;
	inst->tileY      = 100;
	inst->strength   = 100;
	inst->flipY      = 0;
	inst->mapData    = 0;
	inst->mapW       = 0;
	inst->mapH       = 0;

	return inst;
}

XCALL_(static void)
Destroy(NormalMapInst *inst)
{
	XCALL_INIT;
	if (inst) {
		if (inst->mapData) plugin_free(inst->mapData);
		plugin_free(inst);
	}
}

XCALL_(static LWError)
Copy(NormalMapInst *from, NormalMapInst *to)
{
	XCALL_INIT;
	*to = *from;
	return 0;
}

XCALL_(static LWError)
Load(NormalMapInst *inst, const LWLoadState *ls)
{
	char buf[256];
	const char *p;
	int v;
	XCALL_INIT;

	if (ls->ioMode == LWIO_SCENE) {
		/* Line 1: version + numeric params */
		if (!spi_read_line(ls, buf, sizeof(buf)) || !buf[0])
			return 0;

		p = buf;
		p = parse_int(p, &v);
		if (v != NMAP_OBJECT_VERSION)
			return 0;
		p = parse_int(p, &inst->projection);
		p = parse_int(p, &inst->tileX);
		p = parse_int(p, &inst->tileY);
		p = parse_int(p, &inst->strength);
		p = parse_int(p, &inst->flipY);

		/* Line 2: image name */
		if (spi_read_line(ls, buf, sizeof(buf)) && buf[0])
			strncpy(inst->imageName, buf, NMAP_MAX_NAME - 1);
		else
			inst->imageName[0] = '\0';
		inst->imageName[NMAP_MAX_NAME - 1] = '\0';
	} else {
		if (!spi_read_i32be(ls, &v) || v != NMAP_OBJECT_VERSION)
			return 0;
		if (!spi_read_i32be(ls, &inst->projection)) return 0;
		if (!spi_read_i32be(ls, &inst->tileX)) return 0;
		if (!spi_read_i32be(ls, &inst->tileY)) return 0;
		if (!spi_read_i32be(ls, &inst->strength)) return 0;
		if (!spi_read_i32be(ls, &inst->flipY)) return 0;
		if (!spi_read_string_record(ls, inst->imageName, NMAP_MAX_NAME))
			inst->imageName[0] = '\0';
	}

	/* Try to reconnect the image */
	inst->imageID = find_image_by_name(inst->imageName);
	return 0;
}

XCALL_(static LWError)
Save(NormalMapInst *inst, const LWSaveState *ss)
{
	char buf[256];
	int pos = 0;
	XCALL_INIT;

	/* Refresh the stored name from the current image */
	if (inst->imageID && gfunc) {
		LWImageList *il = (LWImageList *)(*gfunc)("LW Image List",
		                                           GFUSE_TRANSIENT);
		if (il) {
			const char *n = (*il->name)(inst->imageID);
			if (n) {
				strncpy(inst->imageName, n, NMAP_MAX_NAME - 1);
				inst->imageName[NMAP_MAX_NAME - 1] = '\0';
			}
		}
	}

	append_int(buf, &pos, NMAP_OBJECT_VERSION);
	append_int(buf, &pos, inst->projection);
	append_int(buf, &pos, inst->tileX);
	append_int(buf, &pos, inst->tileY);
	append_int(buf, &pos, inst->strength);
	append_int(buf, &pos, inst->flipY);

	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
		spi_write_line(ss, inst->imageName[0] ? inst->imageName : "");
	} else {
		spi_write_i32be(ss, NMAP_OBJECT_VERSION);
		spi_write_i32be(ss, inst->projection);
		spi_write_i32be(ss, inst->tileX);
		spi_write_i32be(ss, inst->tileY);
		spi_write_i32be(ss, inst->strength);
		spi_write_i32be(ss, inst->flipY);
		spi_write_string_record(ss, inst->imageName[0] ? inst->imageName : "");
	}

	return 0;
}

XCALL_(static LWError)
Init(NormalMapInst *inst)
{
	LWImageList *il;
	XCALL_INIT;

	/* Free any previous buffer */
	if (inst->mapData) {
		plugin_free(inst->mapData);
		inst->mapData = 0;
		inst->mapW = inst->mapH = 0;
	}

	/* Get image list (transient — used only during Init) */
	il = 0;
	if (gfunc)
		il = (LWImageList *)(*gfunc)("LW Image List", GFUSE_TRANSIENT);

	/* Re-resolve the image by name if needed */
	if (inst->imageName[0] && !inst->imageID && il) {
		LWImageID id = (*il->first)();
		while (id) {
			const char *n = (*il->name)(id);
			if (n && strcmp(n, inst->imageName) == 0) {
				inst->imageID = id;
				break;
			}
			id = (*il->next)(id);
		}
	}

	/* Copy the image into our own buffer so Evaluate never
	 * needs to call any LW globals during rendering. */
	if (inst->imageID && il) {
		int w = 0, h = 0;
		(*il->size)(inst->imageID, &w, &h);
		if (w > 0 && h > 0) {
			unsigned char *buf;
			buf = (unsigned char *)plugin_alloc(
				(unsigned long)w * (unsigned long)h * 3);
			if (buf) {
				int x, y;
				for (y = 0; y < h; y++) {
					for (x = 0; x < w; x++) {
						BufferValue rgb[3];
						int idx = (y * w + x) * 3;
						rgb[0] = rgb[1] = rgb[2] = 128;
						(*il->RGB)(inst->imageID, x, y, rgb);
						buf[idx]     = rgb[0];
						buf[idx + 1] = rgb[1];
						buf[idx + 2] = rgb[2];
					}
				}
				inst->mapData = buf;
				inst->mapW = w;
				inst->mapH = h;
			}
		}
	}

	return 0;
}

XCALL_(static void)
Cleanup(NormalMapInst *inst)
{
	XCALL_INIT;
	if (inst->mapData) {
		plugin_free(inst->mapData);
		inst->mapData = 0;
		inst->mapW = inst->mapH = 0;
	}
}

XCALL_(static LWError)
NewTime(NormalMapInst *inst, LWFrame f, LWTime t)
{
	XCALL_INIT;
	return 0;
}

XCALL_(static unsigned int)
Flags(NormalMapInst *inst)
{
	XCALL_INIT;
	if (inst->mapData && inst->strength > 0)
		return LWSHF_NORMAL;
	return 0;
}

/* ----------------------------------------------------------------
 * Evaluate — sample our pre-loaded buffer and perturb wNorm
 *
 * No LW globals are called here — everything is pure math on
 * the pre-loaded mapData buffer.
 * ---------------------------------------------------------------- */

XCALL_(static void)
Evaluate(NormalMapInst *inst, ShaderAccess *sa)
{
	double u, v, tU, tV;
	double mapR, mapG, mapB;
	double nmx, nmy, nmz, str;
	double N[3], T[3], B[3];
	int px, py, idx;

	XCALL_INIT;

	if (!inst->mapData || inst->strength == 0)
		return;

	/* ---- Derive UV from object-space position ---- */
	switch (inst->projection) {
	case NMAP_PROJ_XZ:
		u = sa->oPos[0]; v = sa->oPos[2]; break;
	case NMAP_PROJ_YZ:
		u = sa->oPos[1]; v = sa->oPos[2]; break;
	default: /* NMAP_PROJ_XY */
		u = sa->oPos[0]; v = sa->oPos[1]; break;
	}

	/* Apply tiling */
	tU = inst->tileX / 100.0;
	tV = inst->tileY / 100.0;
	if (tU < 0.01) tU = 0.01;
	if (tV < 0.01) tV = 0.01;
	u *= tU;
	v *= tV;

	/* Wrap to 0..1 */
	u = u - my_floor(u);
	v = v - my_floor(v);

	/* Convert to pixel coordinates and clamp */
	px = (int)(u * (double)(inst->mapW - 1));
	py = (int)((1.0 - v) * (double)(inst->mapH - 1));  /* flip V */
	if (px < 0) px = 0;
	if (px >= inst->mapW) px = inst->mapW - 1;
	if (py < 0) py = 0;
	if (py >= inst->mapH) py = inst->mapH - 1;

	/* Sample from our pre-loaded buffer (no LW calls) */
	idx = (py * inst->mapW + px) * 3;
	mapR = (inst->mapData[idx]     / 255.0) * 2.0 - 1.0;
	mapG = (inst->mapData[idx + 1] / 255.0) * 2.0 - 1.0;
	mapB =  inst->mapData[idx + 2] / 255.0;

	if (inst->flipY) mapG = -mapG;
	if (mapB < 0.01) mapB = 0.01;

	str = inst->strength / 100.0;
	nmx = mapR * str;
	nmy = mapG * str;
	nmz = mapB;

	/* ---- Build tangent frame from surface normal ---- */
	N[0] = sa->wNorm[0];
	N[1] = sa->wNorm[1];
	N[2] = sa->wNorm[2];

	{
		double ref[3];
		if (N[0] > 0.9 || N[0] < -0.9) {
			ref[0] = 0.0; ref[1] = 1.0; ref[2] = 0.0;
		} else {
			ref[0] = 1.0; ref[1] = 0.0; ref[2] = 0.0;
		}
		T[0] = N[1]*ref[2] - N[2]*ref[1];
		T[1] = N[2]*ref[0] - N[0]*ref[2];
		T[2] = N[0]*ref[1] - N[1]*ref[0];
	}
	vec_normalize(T);

	B[0] = N[1]*T[2] - N[2]*T[1];
	B[1] = N[2]*T[0] - N[0]*T[2];
	B[2] = N[0]*T[1] - N[1]*T[0];
	vec_normalize(B);

	/* ---- Transform tangent-space normal to world space ---- */
	sa->wNorm[0] = nmx * T[0] + nmy * B[0] + nmz * N[0];
	sa->wNorm[1] = nmx * T[1] + nmy * B[1] + nmz * N[1];
	sa->wNorm[2] = nmx * T[2] + nmy * B[2] + nmz * N[2];
	vec_normalize(sa->wNorm);
}

/* ----------------------------------------------------------------
 * Description line
 * ---------------------------------------------------------------- */

static char descBuf[64];

XCALL_(static const char *)
DescLn(NormalMapInst *inst)
{
	XCALL_INIT;
	if (inst->imageName[0]) {
		int i;
		strncpy(descBuf, inst->imageName, 40);
		descBuf[40] = '\0';
		/* Append strength */
		i = (int)strlen(descBuf);
		descBuf[i++] = ' ';
		int_to_str(inst->strength, descBuf + i, (int)sizeof(descBuf) - i);
		i = (int)strlen(descBuf);
		descBuf[i++] = '%';
		descBuf[i] = '\0';
		return descBuf;
	}
	return "(no image)";
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

#define MAX_POPUP_IMAGES 32

static const char *projNames[] = { "Planar XY", "Planar XZ", "Planar YZ", 0 };

XCALL_(static int)
Interface(
	long       version,
	GlobalFunc *global,
	NormalMapInst *inst,
	void       *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWImageList  *il;
	LWControl    *ctlImage, *ctlProj, *ctlTileX, *ctlTileY;
	LWControl    *ctlStrength, *ctlFlipY;
	const char   *imgItems[MAX_POPUP_IMAGES + 2];
	LWImageID     imgIDs[MAX_POPUP_IMAGES];
	int           imgCount = 0;
	int           curImgIdx = 0;

	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;

	msg  = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	il   = (LWImageList *)(*global)("LW Image List", GFUSE_TRANSIENT);

	if (!panl) {
		if (msg)
			(*msg->info)("Normal Map", inst->imageName[0]
				? inst->imageName : "(no image)");
		return AFUNC_OK;
	}

	/* Build the image popup items */
	imgItems[0] = "(None)";
	if (il) {
		LWImageID id = (*il->first)();
		while (id && imgCount < MAX_POPUP_IMAGES) {
			const char *name = (*il->name)(id);
			imgIDs[imgCount] = id;
			imgItems[imgCount + 1] = name ? name : "?";
			if (id == inst->imageID)
				curImgIdx = imgCount + 1;
			imgCount++;
			id = (*il->next)(id);
		}
	}
	imgItems[imgCount + 1] = 0;

	{
		static LWPanControlDesc desc;
		static LWValue ival = {LWT_INTEGER};

		pan = PAN_CREATE(panl, "Normal Map v" PLUGIN_VERSION
		                       " (c) D. Panokostas");
		if (!pan) return AFUNC_OK;

		ctlImage    = POPUP_CTL(panl, pan, "Image", imgItems);
		ctlProj     = POPUP_CTL(panl, pan, "Projection", projNames);
		ctlTileX    = INT_CTL(panl, pan, "Tile X %");
		ctlTileY    = INT_CTL(panl, pan, "Tile Y %");
		ctlStrength = SLIDER_CTL(panl, pan, "Strength", 150, 0, 100);
		ctlFlipY    = BOOL_CTL(panl, pan, "Flip Y (DirectX)");

		SET_INT(ctlImage, curImgIdx);
		SET_INT(ctlProj, inst->projection);
		SET_INT(ctlTileX, inst->tileX);
		SET_INT(ctlTileY, inst->tileY);
		SET_INT(ctlStrength, inst->strength);
		SET_INT(ctlFlipY, inst->flipY);

		if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
			int idx;
			GET_INT(ctlImage, idx);
			if (idx > 0 && idx <= imgCount) {
				inst->imageID = imgIDs[idx - 1];
				/* Save the name from our popup array (known valid) */
				if (imgItems[idx]) {
					strncpy(inst->imageName, imgItems[idx],
					        NMAP_MAX_NAME - 1);
					inst->imageName[NMAP_MAX_NAME - 1] = '\0';
				}
			} else {
				inst->imageID = 0;
				inst->imageName[0] = '\0';
			}

			GET_INT(ctlProj, inst->projection);
			GET_INT(ctlTileX, inst->tileX);
			GET_INT(ctlTileY, inst->tileY);
			GET_INT(ctlStrength, inst->strength);
			GET_INT(ctlFlipY, inst->flipY);

			/* Clamp */
			if (inst->projection < 0) inst->projection = 0;
			if (inst->projection >= NMAP_NUM_PROJ)
				inst->projection = NMAP_NUM_PROJ - 1;
			if (inst->tileX < 1) inst->tileX = 1;
			if (inst->tileX > 10000) inst->tileX = 10000;
			if (inst->tileY < 1) inst->tileY = 1;
			if (inst->tileY > 10000) inst->tileY = 10000;
			if (inst->strength < 0) inst->strength = 0;
			if (inst->strength > 100) inst->strength = 100;
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
	h->descln   = (void *)DescLn;

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
	{ "ShaderHandler",   "NormalMap",
	  (ActivateFunc *)Activate },
	{ "ShaderInterface", "NormalMap",
	  (ActivateFunc *)Interface },
	{ 0 }
};
