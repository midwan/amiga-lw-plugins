/*
 * NORMALMAP.C -- projected normal and material-map shader for LightWave 3D.
 * Uses only AllocMem/FreeMem; no libnix heap or stdio runtime is available.
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
extern double pow(double, double);

#define NMAP_MAX_NAME 128
#define NMAP_MAX_POPUP_IMAGES 32
#define NMAP_PROJ_XY 0
#define NMAP_PROJ_XZ 1
#define NMAP_PROJ_YZ 2
#define NMAP_PROJ_BOX 3
#define NMAP_NUM_PROJ 4
#define NMAP_OBJECT_VERSION 2
#define NMAP_SLOT_NORMAL 0
#define NMAP_SLOT_ALBEDO 1
#define NMAP_SLOT_ROUGHNESS 2
#define NMAP_SLOT_METALLIC 3
#define NMAP_SLOT_AO 4
#define NMAP_SLOT_SPECULAR 5
#define NMAP_SLOT_COUNT 6
#define NMAP_ALBEDO_MULTIPLY 0
#define NMAP_ALBEDO_REPLACE 1

typedef struct {
	LWImageID imageID;
	char imageName[NMAP_MAX_NAME];
	unsigned char *data;
	int width, height;
} MapSlot;

typedef struct {
	/* Render callbacks have a small stack.  All vector scratch is instance
	 * state so Evaluate and its helpers use only scalar locals. */
	double sample[3][3];
	double objectNormal[3];
	double objectGeomNormal[3];
	double worldNormal[3];
} NormalMapScratch;

typedef struct {
	MapSlot map[NMAP_SLOT_COUNT];
	NormalMapScratch scratch;
	int projection;
	int tileX, tileY;
	int strength;
	int flipY;
	int boxSharpness;
	int albedoAmount;
	int albedoMode;
	int roughnessAmount;
	int metallicAmount;
	int aoAmount;
	int specularAmount;
} NormalMapInst;

static MessageFuncs *msg;
static GlobalFunc *gfunc;

static void *plugin_alloc(unsigned long size)
{
	unsigned long *p = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!p) return 0;
	*p = size + 4;
	return p + 1;
}

static void plugin_free(void *ptr)
{
	unsigned long *p;
	if (!ptr) return;
	p = ((unsigned long *)ptr) - 1;
	FreeMem(p, *p);
}

static void int_to_str(int val, char *buf, int buflen)
{
	char tmp[12];
	int i = 0, neg = 0, len, j;
	if (val < 0) { neg = 1; val = -val; }
	if (val == 0) tmp[i++] = '0';
	while (val > 0 && i < 11) { tmp[i++] = (char)('0' + val % 10); val /= 10; }
	len = neg + i;
	if (len >= buflen) len = buflen - 1;
	if (neg) buf[0] = '-';
	for (j = 0; j < i && neg + j < buflen - 1; j++) buf[neg + j] = tmp[i - 1 - j];
	buf[len] = '\0';
}

static const char *parse_int(const char *s, int *val)
{
	int neg = 0;
	*val = 0;
	while (*s == ' ') s++;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') { *val = *val * 10 + (*s - '0'); s++; }
	if (neg) *val = -*val;
	return s;
}

static void append_int(char *buf, int *pos, int val)
{
	char tmp[12];
	int i;
	int_to_str(val, tmp, sizeof(tmp));
	if (*pos) buf[(*pos)++] = ' ';
	for (i = 0; tmp[i]; i++) buf[(*pos)++] = tmp[i];
	buf[*pos] = '\0';
}

static void vec_normalize(double v[3])
{
	double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len > 0.00001) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

static double my_floor(double x)
{
	int i = (int)x;
	if (x < 0.0 && x != (double)i) i--;
	return (double)i;
}

static double absd(double x) { return x < 0.0 ? -x : x; }

static void clear_slot(MapSlot *slot)
{
	if (slot->data) plugin_free(slot->data);
	slot->data = 0;
	slot->width = slot->height = 0;
}

static void init_defaults(NormalMapInst *inst)
{
	int i;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) {
		inst->map[i].imageID = 0;
		inst->map[i].imageName[0] = '\0';
		inst->map[i].data = 0;
		inst->map[i].width = inst->map[i].height = 0;
	}
	inst->projection = NMAP_PROJ_XY;
	inst->tileX = inst->tileY = 100;
	inst->strength = 100;
	inst->flipY = 0;
	inst->boxSharpness = 2;
	inst->albedoAmount = 100;
	inst->albedoMode = NMAP_ALBEDO_REPLACE;
	inst->roughnessAmount = 100;
	inst->metallicAmount = 100;
	inst->aoAmount = 100;
	inst->specularAmount = 100;
}

static const char *base_name(const char *path)
{
	const char *b = path, *p;
	if (!path) return 0;
	for (p = path; *p; p++)
		if (*p == ':' || *p == '/' || *p == '\\') b = p + 1;
	return b;
}

static int base_name_equal(const char *stored, const char *listed)
{
	const char *a, *b;
	if (!stored || !listed) return 0;
	/* Layout may list an image without its volume or path prefix. */
	a = base_name(stored); b = base_name(listed);
	while (*a && *b) {
		char ca = *a++, cb = *b++;
		if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
		if (ca != cb) return 0;
	}
	return *a == '\0' && *b == '\0';
}

static LWImageID find_image_by_name(const char *name)
{
	LWImageList *il;
	LWImageID id, baseMatch = 0;
	int baseCount = 0;
	if (!gfunc || !name || !name[0]) return 0;
	il = (LWImageList *)(*gfunc)("LW Image List", GFUSE_TRANSIENT);
	if (!il) return 0;
	/* Exact match wins; a basename match binds only when unique. */
	for (id = (*il->first)(); id; id = (*il->next)(id)) {
		const char *n = (*il->name)(id);
		if (!n) continue;
		if (strcmp(n, name) == 0) return id;
		if (!baseMatch && base_name_equal(name, n)) baseMatch = id;
		if (base_name_equal(name, n)) baseCount++;
	}
	return baseCount == 1 ? baseMatch : 0;
}

static void refresh_name(MapSlot *slot)
{
	LWImageList *il;
	const char *n;
	if (!slot->imageID || !gfunc) return;
	il = (LWImageList *)(*gfunc)("LW Image List", GFUSE_TRANSIENT);
	if (!il) return;
	n = (*il->name)(slot->imageID);
	if (n) {
		strncpy(slot->imageName, n, NMAP_MAX_NAME - 1);
		slot->imageName[NMAP_MAX_NAME - 1] = '\0';
	}
}

static void load_slot(MapSlot *slot, LWImageList *il)
{
	int x, y, w = 0, h = 0;
	unsigned char *data;
	clear_slot(slot);
	if (!slot->imageID && slot->imageName[0]) slot->imageID = find_image_by_name(slot->imageName);
	if (!slot->imageID || !il) return;
	(*il->size)(slot->imageID, &w, &h);
	if (w <= 0 || h <= 0) return;
	data = (unsigned char *)plugin_alloc((unsigned long)w * (unsigned long)h * 3);
	if (!data) return;
	for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
		BufferValue rgb[3];
		int at = (y * w + x) * 3;
		(*il->RGB)(slot->imageID, x, y, rgb);
		data[at] = rgb[0]; data[at + 1] = rgb[1]; data[at + 2] = rgb[2];
	}
	slot->data = data;
	slot->width = w;
	slot->height = h;
}

XCALL_(static LWInstance) Create(LWError *err)
{
	NormalMapInst *inst;
	XCALL_INIT;
	inst = (NormalMapInst *)plugin_alloc(sizeof(*inst));
	if (!inst) return 0;
	init_defaults(inst);
	return inst;
}

XCALL_(static void) Destroy(NormalMapInst *inst)
{
	int i;
	XCALL_INIT;
	if (!inst) return;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) clear_slot(&inst->map[i]);
	plugin_free(inst);
}

XCALL_(static LWError) Copy(NormalMapInst *from, NormalMapInst *to)
{
	int i;
	XCALL_INIT;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) clear_slot(&to->map[i]);
	*to = *from;
	/* Pixel buffers are render-time state, not instance state. */
	for (i = 0; i < NMAP_SLOT_COUNT; i++) {
		to->map[i].data = 0;
		to->map[i].width = to->map[i].height = 0;
	}
	return 0;
}

static void load_v1_scene(NormalMapInst *inst, const char *line, const LWLoadState *ls)
{
	char name[NMAP_MAX_NAME];
	const char *p = line;
	int version;
	p = parse_int(p, &version);

	p = parse_int(p, &inst->projection);
	p = parse_int(p, &inst->tileX);
	p = parse_int(p, &inst->tileY);
	p = parse_int(p, &inst->strength);
	parse_int(p, &inst->flipY);
	if (spi_read_line(ls, name, sizeof(name)) && name[0]) {
		strncpy(inst->map[NMAP_SLOT_NORMAL].imageName, name, NMAP_MAX_NAME - 1);
		inst->map[NMAP_SLOT_NORMAL].imageName[NMAP_MAX_NAME - 1] = '\0';
	}
}
/* Object SDAT strings are length-prefixed.  An empty record is a valid
 * unassigned map slot, so its zero length must not be treated as an error. */
static int read_object_string(const LWLoadState *ls, char *buf, int buflen)
{
	unsigned char bytes[4];
	unsigned long len, keep, left;
	char discard[16];

	buf[0] = '\0';
	if ((*ls->read)(ls->readData, (char *)bytes, 4) != 4) return 0;
	len = ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
		((unsigned long)bytes[2] << 8) | (unsigned long)bytes[3];
	keep = len < (unsigned long)(buflen - 1) ? len : (unsigned long)(buflen - 1);
	if (keep && (*ls->read)(ls->readData, buf, (int)keep) != (int)keep) return 0;
	buf[keep] = '\0';
	left = len - keep;
	while (left) {
		int count = left > sizeof(discard) ? (int)sizeof(discard) : (int)left;
		if ((*ls->read)(ls->readData, discard, count) != count) return 0;
		left -= (unsigned long)count;
	}
	return 1;
}

XCALL_(static LWError) Load(NormalMapInst *inst, const LWLoadState *ls)
{
	char buf[256];
	const char *p;
	int v, i;
	XCALL_INIT;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) clear_slot(&inst->map[i]);
	init_defaults(inst);
	if (ls->ioMode == LWIO_SCENE) {
		if (!spi_read_line(ls, buf, sizeof(buf)) || !buf[0]) return 0;
		p = parse_int(buf, &v);
		if (v == 1) {
			load_v1_scene(inst, buf, ls);
		} else if (v == NMAP_OBJECT_VERSION) {
			p = parse_int(p, &inst->projection);
			p = parse_int(p, &inst->tileX); p = parse_int(p, &inst->tileY);
			p = parse_int(p, &inst->strength); p = parse_int(p, &inst->flipY);
			p = parse_int(p, &inst->boxSharpness); p = parse_int(p, &inst->albedoMode);
			p = parse_int(p, &inst->albedoAmount); p = parse_int(p, &inst->roughnessAmount);
			p = parse_int(p, &inst->metallicAmount); p = parse_int(p, &inst->aoAmount);
			parse_int(p, &inst->specularAmount);
			for (i = 0; i < NMAP_SLOT_COUNT; i++) {
				if (spi_read_line(ls, buf, sizeof(buf)) && buf[0]) {
					strncpy(inst->map[i].imageName, buf, NMAP_MAX_NAME - 1);
					inst->map[i].imageName[NMAP_MAX_NAME - 1] = '\0';
				}
			}
		} else return 0;
	} else {
		if (!spi_read_i32be(ls, &v)) return 0;
		if (v == 1) {
			if (!spi_read_i32be(ls, &inst->projection) || !spi_read_i32be(ls, &inst->tileX) ||
				!spi_read_i32be(ls, &inst->tileY) || !spi_read_i32be(ls, &inst->strength) ||
				!spi_read_i32be(ls, &inst->flipY) ||
				!read_object_string(ls, inst->map[NMAP_SLOT_NORMAL].imageName, NMAP_MAX_NAME)) return 0;
		} else if (v == NMAP_OBJECT_VERSION) {
			if (!spi_read_i32be(ls, &inst->projection) || !spi_read_i32be(ls, &inst->tileX) ||
				!spi_read_i32be(ls, &inst->tileY) || !spi_read_i32be(ls, &inst->strength) ||
				!spi_read_i32be(ls, &inst->flipY) || !spi_read_i32be(ls, &inst->boxSharpness) ||
				!spi_read_i32be(ls, &inst->albedoMode) || !spi_read_i32be(ls, &inst->albedoAmount) ||
				!spi_read_i32be(ls, &inst->roughnessAmount) || !spi_read_i32be(ls, &inst->metallicAmount) ||
				!spi_read_i32be(ls, &inst->aoAmount) || !spi_read_i32be(ls, &inst->specularAmount)) return 0;
			for (i = 0; i < NMAP_SLOT_COUNT; i++)
				if (!read_object_string(ls, inst->map[i].imageName, NMAP_MAX_NAME)) return 0;
		} else return 0;
	}
	/* Damaged or hand-edited data must not reach the render loop with
	 * out-of-range values (boxSharpness drives a per-pixel multiply loop). */
	if (inst->projection < 0 || inst->projection >= NMAP_NUM_PROJ) inst->projection = NMAP_PROJ_XY;
	if (inst->tileX < 1) inst->tileX = 1;
	if (inst->tileX > 10000) inst->tileX = 10000;
	if (inst->tileY < 1) inst->tileY = 1;
	if (inst->tileY > 10000) inst->tileY = 10000;
	if (inst->strength < 0) inst->strength = 0;
	if (inst->strength > 100) inst->strength = 100;
	if (inst->boxSharpness < 1) inst->boxSharpness = 1;
	if (inst->boxSharpness > 8) inst->boxSharpness = 8;
	if (inst->albedoMode < 0 || inst->albedoMode > 1) inst->albedoMode = NMAP_ALBEDO_REPLACE;
	if (inst->albedoAmount < 0) inst->albedoAmount = 0;
	if (inst->albedoAmount > 100) inst->albedoAmount = 100;
	if (inst->roughnessAmount < 0) inst->roughnessAmount = 0;
	if (inst->roughnessAmount > 100) inst->roughnessAmount = 100;
	if (inst->metallicAmount < 0) inst->metallicAmount = 0;
	if (inst->metallicAmount > 100) inst->metallicAmount = 100;
	if (inst->aoAmount < 0) inst->aoAmount = 0;
	if (inst->aoAmount > 100) inst->aoAmount = 100;
	if (inst->specularAmount < 0) inst->specularAmount = 0;
	if (inst->specularAmount > 100) inst->specularAmount = 100;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) inst->map[i].imageID = find_image_by_name(inst->map[i].imageName);
	return 0;
}

XCALL_(static LWError) Save(NormalMapInst *inst, const LWSaveState *ss)
{
	char buf[256];
	int p = 0, i, objectBytes = 13 * 4;
	XCALL_INIT;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) refresh_name(&inst->map[i]);
	append_int(buf, &p, NMAP_OBJECT_VERSION); append_int(buf, &p, inst->projection);
	append_int(buf, &p, inst->tileX); append_int(buf, &p, inst->tileY);
	append_int(buf, &p, inst->strength); append_int(buf, &p, inst->flipY);
	append_int(buf, &p, inst->boxSharpness); append_int(buf, &p, inst->albedoMode);
	append_int(buf, &p, inst->albedoAmount); append_int(buf, &p, inst->roughnessAmount);
	append_int(buf, &p, inst->metallicAmount); append_int(buf, &p, inst->aoAmount);
	append_int(buf, &p, inst->specularAmount);
	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
		for (i = 0; i < NMAP_SLOT_COUNT; i++) spi_write_line(ss, inst->map[i].imageName);
	} else {
		spi_write_i32be(ss, NMAP_OBJECT_VERSION); spi_write_i32be(ss, inst->projection);
		spi_write_i32be(ss, inst->tileX); spi_write_i32be(ss, inst->tileY);
		spi_write_i32be(ss, inst->strength); spi_write_i32be(ss, inst->flipY);
		spi_write_i32be(ss, inst->boxSharpness); spi_write_i32be(ss, inst->albedoMode);
		spi_write_i32be(ss, inst->albedoAmount); spi_write_i32be(ss, inst->roughnessAmount);
		spi_write_i32be(ss, inst->metallicAmount); spi_write_i32be(ss, inst->aoAmount);
		spi_write_i32be(ss, inst->specularAmount);
		for (i = 0; i < NMAP_SLOT_COUNT; i++) {
			spi_write_string_record(ss, inst->map[i].imageName);
			objectBytes += 4 + (int)strlen(inst->map[i].imageName);
		}
		/* LightWave 5.0 corrupts odd-length SDAT chunks. */
		if (objectBytes & 1) {
			char pad = 0;
			(*ss->write)(ss->writeData, &pad, 1);
		}
	}
	return 0;
}

XCALL_(static LWError) Init(NormalMapInst *inst)
{
	LWImageList *il;
	int i;
	XCALL_INIT;
	il = gfunc ? (LWImageList *)(*gfunc)("LW Image List", GFUSE_TRANSIENT) : 0;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) load_slot(&inst->map[i], il);
	return 0;
}

XCALL_(static void) Cleanup(NormalMapInst *inst)
{
	int i;
	XCALL_INIT;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) clear_slot(&inst->map[i]);
}

XCALL_(static LWError) NewTime(NormalMapInst *inst, LWFrame f, LWTime t)
{
	LWImageList *il;
	int i;
	XCALL_INIT;
	il = gfunc ? (LWImageList *)(*gfunc)("LW Image List", GFUSE_TRANSIENT) : 0;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) {
		clear_slot(&inst->map[i]);
		load_slot(&inst->map[i], il);
	}
	return 0;
}

XCALL_(static unsigned int) Flags(NormalMapInst *inst)
{
	unsigned int f = 0;
	XCALL_INIT;
	if (inst->map[NMAP_SLOT_NORMAL].data && inst->strength) f |= LWSHF_NORMAL;
	if (inst->map[NMAP_SLOT_ALBEDO].data && inst->albedoAmount) f |= LWSHF_COLOR;
	if (inst->map[NMAP_SLOT_ROUGHNESS].data && inst->roughnessAmount) f |= LWSHF_ROUGH;
	if (inst->map[NMAP_SLOT_METALLIC].data && inst->metallicAmount) f |= LWSHF_MIRROR | LWSHF_DIFFUSE | LWSHF_SPECULAR;
	if (inst->map[NMAP_SLOT_AO].data && inst->aoAmount) f |= LWSHF_COLOR | LWSHF_DIFFUSE;
	if (inst->map[NMAP_SLOT_SPECULAR].data && inst->specularAmount) f |= LWSHF_SPECULAR;
	return f;
}

static void map_uv(const NormalMapInst *inst, int axis, const double pos[3], double *u, double *v)
{
	switch (axis) {
	case NMAP_PROJ_XZ: *u = pos[0]; *v = pos[2]; break;
	case NMAP_PROJ_YZ: *u = pos[1]; *v = pos[2]; break;
	default: *u = pos[0]; *v = pos[1]; break;
	}
	*u = *u * inst->tileX / 100.0;
	*v = *v * inst->tileY / 100.0;
	*u -= my_floor(*u);
	*v -= my_floor(*v);
}

static void sample_2d(const MapSlot *slot, double u, double v, double rgb[3])
{
	int x0, y0, x1, y1, component;
	double fx, fy, xmix, ymix, top, bottom;

	if (!slot->data || slot->width < 1 || slot->height < 1) {
		rgb[0] = rgb[1] = rgb[2] = 0.0;
		return;
	}
	fx = u * slot->width;
	fy = (1.0 - v) * slot->height;
	if (fy >= slot->height) fy = 0.0;
	x0 = (int)fx;
	y0 = (int)fy;
	x1 = x0 + 1;
	y1 = y0 + 1;
	if (x1 == slot->width) x1 = 0;
	if (y1 == slot->height) y1 = 0;
	xmix = fx - x0;
	ymix = fy - y0;
	for (component = 0; component < 3; component++) {
		top = slot->data[(y0 * slot->width + x0) * 3 + component] / 255.0;
		top += (slot->data[(y0 * slot->width + x1) * 3 + component] / 255.0 - top) * xmix;
		bottom = slot->data[(y1 * slot->width + x0) * 3 + component] / 255.0;
		bottom += (slot->data[(y1 * slot->width + x1) * 3 + component] / 255.0 - bottom) * xmix;
		rgb[component] = top + (bottom - top) * ymix;
	}
}

static double tri_weight(double normal, int sharpness)
{
	double weight = absd(normal);
	int i;
	for (i = 1; i < sharpness; i++) weight *= absd(normal);
	return weight;
}

static void sample_axis_mirrored(const NormalMapInst *inst, const MapSlot *slot, int axis,
	const double pos[3], int mirrorV, double rgb[3])
{
	double u, v;
	map_uv(inst, axis, pos, &u, &v);
	if (mirrorV) {
		v = 1.0 - v;
		if (v >= 1.0) v = 0.0;
	}
	sample_2d(slot, u, v, rgb);
}

static void sample_axis(const NormalMapInst *inst, const MapSlot *slot, int axis,
	const double pos[3], double rgb[3])
{
	sample_axis_mirrored(inst, slot, axis, pos, 0, rgb);
}

static void sample_map(NormalMapInst *inst, const MapSlot *slot,
	const double pos[3], const double normal[3], double rgb[3])
{
	double wx, wy, wz, total;

	if (inst->projection != NMAP_PROJ_BOX) {
		sample_axis(inst, slot, inst->projection, pos, rgb);
		return;
	}
	sample_axis(inst, slot, NMAP_PROJ_XY, pos, inst->scratch.sample[0]);
	sample_axis(inst, slot, NMAP_PROJ_XZ, pos, inst->scratch.sample[1]);
	sample_axis(inst, slot, NMAP_PROJ_YZ, pos, inst->scratch.sample[2]);
	wx = tri_weight(normal[0], inst->boxSharpness);
	wy = tri_weight(normal[1], inst->boxSharpness);
	wz = tri_weight(normal[2], inst->boxSharpness);
	total = wx + wy + wz;
	if (total < 0.00001) total = 1.0;
	rgb[0] = (wz * inst->scratch.sample[0][0] + wy * inst->scratch.sample[1][0] + wx * inst->scratch.sample[2][0]) / total;
	rgb[1] = (wz * inst->scratch.sample[0][1] + wy * inst->scratch.sample[1][1] + wx * inst->scratch.sample[2][1]) / total;
	rgb[2] = (wz * inst->scratch.sample[0][2] + wy * inst->scratch.sample[1][2] + wx * inst->scratch.sample[2][2]) / total;
}

static double luma(const double rgb[3])
{
	return rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722;
}

static void object_to_world(const ShaderAccess *sa, const double in[3], double out[3])
{
	out[0] = sa->oXfrm[0] * in[0] + sa->oXfrm[3] * in[1] + sa->oXfrm[6] * in[2];
	out[1] = sa->oXfrm[1] * in[0] + sa->oXfrm[4] * in[1] + sa->oXfrm[7] * in[2];
	out[2] = sa->oXfrm[2] * in[0] + sa->oXfrm[5] * in[1] + sa->oXfrm[8] * in[2];
}

static void world_to_object(const ShaderAccess *sa, const double in[3], double out[3])
{
	out[0] = sa->wXfrm[0] * in[0] + sa->wXfrm[3] * in[1] + sa->wXfrm[6] * in[2];
	out[1] = sa->wXfrm[1] * in[0] + sa->wXfrm[4] * in[1] + sa->wXfrm[7] * in[2];
	out[2] = sa->wXfrm[2] * in[0] + sa->wXfrm[5] * in[1] + sa->wXfrm[8] * in[2];
}

static void axis_normal(int axis, const double rgb[3], int flipY, double sign, double out[3])
{
	double x = rgb[0] * 2.0 - 1.0;
	double y = rgb[1] * 2.0 - 1.0;
	double z = rgb[2];
	if (flipY) y = -y;
	if (z < 0.01) z = 0.01;
	if (axis == NMAP_PROJ_XZ) { out[0] = x; out[1] = sign * z; out[2] = y; }
	else if (axis == NMAP_PROJ_YZ) { out[0] = sign * z; out[1] = x; out[2] = y; }
	else { out[0] = x; out[1] = y; out[2] = sign * z; }
	vec_normalize(out);
}

static void apply_normal(NormalMapInst *inst, ShaderAccess *sa)
{
	double wx, wy, wz, total, strength;
	const double *normal = inst->scratch.objectGeomNormal;

	if (!inst->map[NMAP_SLOT_NORMAL].data || !inst->strength) return;
	if (inst->projection == NMAP_PROJ_BOX) {
		/* Mirror the sampled V on faces whose U x V opposes the outward
		 * normal, so sampled features and decoded slopes stay aligned. */
		sample_axis_mirrored(inst, &inst->map[NMAP_SLOT_NORMAL], NMAP_PROJ_XY, sa->oPos, normal[2] < 0.0, inst->scratch.sample[0]);
		sample_axis_mirrored(inst, &inst->map[NMAP_SLOT_NORMAL], NMAP_PROJ_XZ, sa->oPos, normal[1] >= 0.0, inst->scratch.sample[1]);
		sample_axis_mirrored(inst, &inst->map[NMAP_SLOT_NORMAL], NMAP_PROJ_YZ, sa->oPos, normal[0] < 0.0, inst->scratch.sample[2]);
		axis_normal(NMAP_PROJ_XY, inst->scratch.sample[0], inst->flipY, normal[2] < 0.0 ? -1.0 : 1.0, inst->scratch.sample[0]);
		axis_normal(NMAP_PROJ_XZ, inst->scratch.sample[1], inst->flipY, normal[1] < 0.0 ? -1.0 : 1.0, inst->scratch.sample[1]);
		axis_normal(NMAP_PROJ_YZ, inst->scratch.sample[2], inst->flipY, normal[0] < 0.0 ? -1.0 : 1.0, inst->scratch.sample[2]);
		/* Reorient each plane's perturbation around the pre-perturbation
		 * surface normal. A neutral normal map must leave it unchanged. */
		inst->scratch.sample[0][0] += normal[0];
		inst->scratch.sample[0][1] += normal[1];
		inst->scratch.sample[0][2] += normal[2] - (normal[2] < 0.0 ? -1.0 : 1.0);
		inst->scratch.sample[1][0] += normal[0];
		inst->scratch.sample[1][1] += normal[1] - (normal[1] < 0.0 ? -1.0 : 1.0);
		inst->scratch.sample[1][2] += normal[2];
		inst->scratch.sample[2][0] += normal[0] - (normal[0] < 0.0 ? -1.0 : 1.0);
		inst->scratch.sample[2][1] += normal[1];
		inst->scratch.sample[2][2] += normal[2];
		wx = tri_weight(normal[0], inst->boxSharpness);
		wy = tri_weight(normal[1], inst->boxSharpness);
		wz = tri_weight(normal[2], inst->boxSharpness);
		total = wx + wy + wz;
		if (total < 0.00001) total = 1.0;
		inst->scratch.objectNormal[0] = (wz * inst->scratch.sample[0][0] + wy * inst->scratch.sample[1][0] + wx * inst->scratch.sample[2][0]) / total;
		inst->scratch.objectNormal[1] = (wz * inst->scratch.sample[0][1] + wy * inst->scratch.sample[1][1] + wx * inst->scratch.sample[2][1]) / total;
		inst->scratch.objectNormal[2] = (wz * inst->scratch.sample[0][2] + wy * inst->scratch.sample[1][2] + wx * inst->scratch.sample[2][2]) / total;
	} else {
		sample_axis(inst, &inst->map[NMAP_SLOT_NORMAL], inst->projection, sa->oPos, inst->scratch.sample[0]);
		inst->scratch.objectNormal[0] = inst->scratch.sample[0][0] * 2.0 - 1.0;
		inst->scratch.objectNormal[1] = inst->scratch.sample[0][1] * 2.0 - 1.0;
		inst->scratch.objectNormal[2] = inst->scratch.sample[0][2];
		if (inst->flipY) inst->scratch.objectNormal[1] = -inst->scratch.objectNormal[1];
		if (inst->scratch.objectNormal[2] < 0.01) inst->scratch.objectNormal[2] = 0.01;
		/* Planar projection retains the original normal-relative tangent
		 * frame so it can perturb curved or off-axis surface geometry. */
		if (sa->wNorm[0] > 0.9 || sa->wNorm[0] < -0.9) {
			inst->scratch.sample[1][0] = -sa->wNorm[2];
			inst->scratch.sample[1][1] = 0.0;
			inst->scratch.sample[1][2] = sa->wNorm[0];
		} else {
			inst->scratch.sample[1][0] = 0.0;
			inst->scratch.sample[1][1] = sa->wNorm[2];
			inst->scratch.sample[1][2] = -sa->wNorm[1];
		}
		vec_normalize(inst->scratch.sample[1]);
		inst->scratch.sample[2][0] = sa->wNorm[1] * inst->scratch.sample[1][2] - sa->wNorm[2] * inst->scratch.sample[1][1];
		inst->scratch.sample[2][1] = sa->wNorm[2] * inst->scratch.sample[1][0] - sa->wNorm[0] * inst->scratch.sample[1][2];
		inst->scratch.sample[2][2] = sa->wNorm[0] * inst->scratch.sample[1][1] - sa->wNorm[1] * inst->scratch.sample[1][0];
		vec_normalize(inst->scratch.sample[2]);
		inst->scratch.worldNormal[0] = inst->scratch.objectNormal[0] * inst->scratch.sample[1][0] + inst->scratch.objectNormal[1] * inst->scratch.sample[2][0] + inst->scratch.objectNormal[2] * sa->wNorm[0];
		inst->scratch.worldNormal[1] = inst->scratch.objectNormal[0] * inst->scratch.sample[1][1] + inst->scratch.objectNormal[1] * inst->scratch.sample[2][1] + inst->scratch.objectNormal[2] * sa->wNorm[1];
		inst->scratch.worldNormal[2] = inst->scratch.objectNormal[0] * inst->scratch.sample[1][2] + inst->scratch.objectNormal[1] * inst->scratch.sample[2][2] + inst->scratch.objectNormal[2] * sa->wNorm[2];
	}
	if (inst->projection == NMAP_PROJ_BOX) {
		object_to_world(sa, inst->scratch.objectNormal, inst->scratch.worldNormal);
		vec_normalize(inst->scratch.worldNormal);
	} else {
		vec_normalize(inst->scratch.worldNormal);
	}
	strength = inst->strength / 100.0;
	sa->wNorm[0] = sa->wNorm[0] * (1.0 - strength) + inst->scratch.worldNormal[0] * strength;
	sa->wNorm[1] = sa->wNorm[1] * (1.0 - strength) + inst->scratch.worldNormal[1] * strength;
	sa->wNorm[2] = sa->wNorm[2] * (1.0 - strength) + inst->scratch.worldNormal[2] * strength;
	vec_normalize(sa->wNorm);
}

XCALL_(static void) Evaluate(NormalMapInst *inst, ShaderAccess *sa)
{
	double *rgb = inst->scratch.sample[0];
	double value, amount, oneMinusCos, fresnel;

	XCALL_INIT;
	/* Use the unperturbed smoothed surface normal for box weights. */
	world_to_object(sa, sa->wNorm, inst->scratch.objectGeomNormal);
	vec_normalize(inst->scratch.objectGeomNormal);
	apply_normal(inst, sa);
	if (inst->map[NMAP_SLOT_ALBEDO].data && inst->albedoAmount) {
		amount = inst->albedoAmount / 100.0;
		sample_map(inst, &inst->map[NMAP_SLOT_ALBEDO], sa->oPos, inst->scratch.objectGeomNormal, rgb);
		if (inst->albedoMode == NMAP_ALBEDO_REPLACE) {
			sa->color[0] += (rgb[0] - sa->color[0]) * amount;
			sa->color[1] += (rgb[1] - sa->color[1]) * amount;
			sa->color[2] += (rgb[2] - sa->color[2]) * amount;
		} else {
			sa->color[0] *= 1.0 + (rgb[0] - 1.0) * amount;
			sa->color[1] *= 1.0 + (rgb[1] - 1.0) * amount;
			sa->color[2] *= 1.0 + (rgb[2] - 1.0) * amount;
		}
	}
	if (inst->map[NMAP_SLOT_ROUGHNESS].data && inst->roughnessAmount) {
		double gloss;
		amount = inst->roughnessAmount / 100.0;
		sample_map(inst, &inst->map[NMAP_SLOT_ROUGHNESS], sa->oPos, inst->scratch.objectGeomNormal, rgb);
		/* sa->roughness is the reciprocal of the glossiness exponent
		 * (measured in-host: GLOS 16 -> 0.0625, GLOS 256 -> 0.0039).
		 * Map roughness 0..1 to exponent 4096..4 and write 1/E. */
		gloss = pow(2.0, 10.0 * (1.0 - luma(rgb)) + 2.0);
		sa->roughness += (1.0 / gloss - sa->roughness) * amount;
	}
	if (inst->map[NMAP_SLOT_METALLIC].data && inst->metallicAmount) {
		amount = inst->metallicAmount / 100.0;
		sample_map(inst, &inst->map[NMAP_SLOT_METALLIC], sa->oPos, inst->scratch.objectGeomNormal, rgb);
	value = luma(rgb) * amount;
	/* Fresnel follows the perturbed normal: rebuild the view dot from
	 * the ray source instead of the read-only cosine field. */
	{
		double view[3], vlen;
		view[0] = sa->raySource[0] - sa->wPos[0];
		view[1] = sa->raySource[1] - sa->wPos[1];
		view[2] = sa->raySource[2] - sa->wPos[2];
		vlen = sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
		if (vlen > 0.000001) { view[0] /= vlen; view[1] /= vlen; view[2] /= vlen; }
		oneMinusCos = 1.0 - absd(view[0] * sa->wNorm[0] + view[1] * sa->wNorm[1] + view[2] * sa->wNorm[2]);
	}
		if (oneMinusCos < 0.0) oneMinusCos = 0.0;
		if (oneMinusCos > 1.0) oneMinusCos = 1.0;
		fresnel = oneMinusCos * oneMinusCos;
		fresnel = 0.04 + 0.96 * fresnel * fresnel * oneMinusCos;
		sa->mirror += (1.0 - sa->mirror) * fresnel * value;
		sa->specular += (1.0 - sa->specular) * fresnel * value;
		sa->diffuse *= 1.0 - 0.95 * value;
	}
	if (inst->map[NMAP_SLOT_AO].data && inst->aoAmount) {
		amount = inst->aoAmount / 100.0;
		sample_map(inst, &inst->map[NMAP_SLOT_AO], sa->oPos, inst->scratch.objectGeomNormal, rgb);
		value = 1.0 + (luma(rgb) - 1.0) * amount;
		sa->color[0] *= value;
		sa->color[1] *= value;
		sa->color[2] *= value;
		sa->diffuse *= value;
	}
	if (inst->map[NMAP_SLOT_SPECULAR].data && inst->specularAmount) {
		amount = inst->specularAmount / 100.0;
		sample_map(inst, &inst->map[NMAP_SLOT_SPECULAR], sa->oPos, inst->scratch.objectGeomNormal, rgb);
		sa->specular += (luma(rgb) - sa->specular) * amount;
	}
}

static char descBuf[64];
XCALL_(static const char *) DescLn(NormalMapInst *inst)
{
	int i;
	XCALL_INIT;
	if (!inst->map[NMAP_SLOT_NORMAL].imageName[0]) return "(no image)";
	strncpy(descBuf, inst->map[NMAP_SLOT_NORMAL].imageName, 40); descBuf[40] = '\0';
	i = (int)strlen(descBuf); descBuf[i++] = ' '; int_to_str(inst->strength, descBuf + i, sizeof(descBuf) - i); i = (int)strlen(descBuf); descBuf[i++] = '%'; descBuf[i] = '\0';
	return descBuf;
}

static const char *projNames[] = { "Planar XY", "Planar XZ", "Planar YZ", "Triplanar (box)", 0 };
static const char *albedoModeNames[] = { "Multiply base colour", "Replace base colour", 0 };
static const char *slotLabels[NMAP_SLOT_COUNT] = { "Normal image", "Albedo / Colour", "Roughness", "Metallic", "Ambient occlusion", "Specular" };

static void select_popup_slot(NormalMapInst *inst, int slot, int index, int count, LWImageID *ids, const char **names)
{
	if (index > 0 && index <= count) {
		inst->map[slot].imageID = ids[index - 1];
		strncpy(inst->map[slot].imageName, names[index], NMAP_MAX_NAME - 1);
		inst->map[slot].imageName[NMAP_MAX_NAME - 1] = '\0';
	} else { inst->map[slot].imageID = 0; inst->map[slot].imageName[0] = '\0'; }
}

XCALL_(static int) Interface(long version, GlobalFunc *global, NormalMapInst *inst, void *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID pan;
	LWImageList *il;
	LWControl *image[NMAP_SLOT_COUNT], *projection, *tileX, *tileY, *strength, *flipY, *sharpness, *albedoMode;
	LWControl *amount[NMAP_SLOT_COUNT];
	/* Popup tables are static: the modal panel runs on a limited
	 * callback stack and these arrays alone exceed its budget. */
	static const char *items[NMAP_MAX_POPUP_IMAGES + NMAP_SLOT_COUNT + 2];
	static LWImageID ids[NMAP_MAX_POPUP_IMAGES + NMAP_SLOT_COUNT];
	static LWPanControlDesc desc;
	static LWValue ival = { LWT_INTEGER };
	static const char *amountLabels[] = { "Albedo amount", "Roughness amount", "Metallic amount", "AO amount", "Specular amount" };
	int count = 0, current[NMAP_SLOT_COUNT], i, j, value, *a;

	XCALL_INIT;
	if (version != 1) return AFUNC_BADVERSION;
	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	il = (LWImageList *)(*global)("LW Image List", GFUSE_TRANSIENT);
	if (!panl) {
		if (msg) (*msg->info)("Normal Map", inst->map[0].imageName[0] ? inst->map[0].imageName : "(no image)");
		return AFUNC_OK;
	}
	items[0] = "(None)";
	if (il) {
		LWImageID id = (*il->first)();
		while (id && count < NMAP_MAX_POPUP_IMAGES) {
			const char *n = (*il->name)(id);
			/* Skip unnamed images: a null entry would truncate the list. */
			if (n) {
				ids[count] = id;
				items[count + 1] = n;
				count++;
			}
			id = (*il->next)(id);
		}
	}
	/* Append active images dropped by the popup cap so every selection
	 * stays visible and can be cleared explicitly. */
	for (i = 0; i < NMAP_SLOT_COUNT; i++) {
		if (!inst->map[i].imageID) continue;
		for (j = 0; j < count; j++)
			if (ids[j] == inst->map[i].imageID) break;
		if (j < count) continue;
		ids[count] = inst->map[i].imageID;
		items[count + 1] = il ? (*il->name)(inst->map[i].imageID) : inst->map[i].imageName;
		if (!items[count + 1]) items[count + 1] = inst->map[i].imageName;
		count++;
	}
	items[count + 1] = 0;
	for (i = 0; i < NMAP_SLOT_COUNT; i++) {
		current[i] = 0;
		for (value = 0; value < count; value++)
			if (ids[value] == inst->map[i].imageID) current[i] = value + 1;
	}
	pan = PAN_CREATE(panl, "Normal Map v" PLUGIN_VERSION);
	if (!pan) return AFUNC_OK;
	for (i = 0; i < NMAP_SLOT_COUNT; i++)
		image[i] = POPUP_CTL(panl, pan, (char *)slotLabels[i], items);
	projection = POPUP_CTL(panl, pan, "Projection", projNames);
	tileX = INT_CTL(panl, pan, "Tile X %");
	tileY = INT_CTL(panl, pan, "Tile Y %");
	strength = SLIDER_CTL(panl, pan, "Normal strength", 150, 0, 100);
	flipY = BOOL_CTL(panl, pan, "Flip normal Y (DirectX)");
	sharpness = SLIDER_CTL(panl, pan, "Box blend sharpness", 150, 1, 8);
	albedoMode = POPUP_CTL(panl, pan, "Albedo mode", albedoModeNames);
	for (i = NMAP_SLOT_ALBEDO; i < NMAP_SLOT_COUNT; i++)
		amount[i] = SLIDER_CTL(panl, pan, (char *)amountLabels[i - NMAP_SLOT_ALBEDO], 150, 0, 100);
	for (i = 0; i < NMAP_SLOT_COUNT; i++) SET_INT(image[i], current[i]);
	SET_INT(projection, inst->projection);
	SET_INT(tileX, inst->tileX);
	SET_INT(tileY, inst->tileY);
	SET_INT(strength, inst->strength);
	SET_INT(flipY, inst->flipY);
	SET_INT(sharpness, inst->boxSharpness);
	SET_INT(albedoMode, inst->albedoMode);
	SET_INT(amount[NMAP_SLOT_ALBEDO], inst->albedoAmount);
	SET_INT(amount[NMAP_SLOT_ROUGHNESS], inst->roughnessAmount);
	SET_INT(amount[NMAP_SLOT_METALLIC], inst->metallicAmount);
	SET_INT(amount[NMAP_SLOT_AO], inst->aoAmount);
	SET_INT(amount[NMAP_SLOT_SPECULAR], inst->specularAmount);
	if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
		for (i = 0; i < NMAP_SLOT_COUNT; i++) {
			GET_INT(image[i], value);
			select_popup_slot(inst, i, value, count, ids, items);
		}
		GET_INT(projection, inst->projection);
		GET_INT(tileX, inst->tileX);
		GET_INT(tileY, inst->tileY);
		GET_INT(strength, inst->strength);
		GET_INT(flipY, inst->flipY);
		GET_INT(sharpness, inst->boxSharpness);
		GET_INT(albedoMode, inst->albedoMode);
		GET_INT(amount[NMAP_SLOT_ALBEDO], inst->albedoAmount);
		GET_INT(amount[NMAP_SLOT_ROUGHNESS], inst->roughnessAmount);
		GET_INT(amount[NMAP_SLOT_METALLIC], inst->metallicAmount);
		GET_INT(amount[NMAP_SLOT_AO], inst->aoAmount);
		GET_INT(amount[NMAP_SLOT_SPECULAR], inst->specularAmount);
		if (inst->projection < 0) inst->projection = 0;
		if (inst->projection >= NMAP_NUM_PROJ) inst->projection = NMAP_NUM_PROJ - 1;
		if (inst->tileX < 1) inst->tileX = 1;
		if (inst->tileX > 10000) inst->tileX = 10000;
		if (inst->tileY < 1) inst->tileY = 1;
		if (inst->tileY > 10000) inst->tileY = 10000;
		if (inst->strength < 0) inst->strength = 0;
		if (inst->strength > 100) inst->strength = 100;
		if (inst->boxSharpness < 1) inst->boxSharpness = 1;
		if (inst->boxSharpness > 8) inst->boxSharpness = 8;
		if (inst->albedoMode < 0 || inst->albedoMode > 1) inst->albedoMode = NMAP_ALBEDO_REPLACE;
		for (i = NMAP_SLOT_ALBEDO; i < NMAP_SLOT_COUNT; i++) {
			a = i == NMAP_SLOT_ALBEDO ? &inst->albedoAmount :
				i == NMAP_SLOT_ROUGHNESS ? &inst->roughnessAmount :
				i == NMAP_SLOT_METALLIC ? &inst->metallicAmount :
				i == NMAP_SLOT_AO ? &inst->aoAmount : &inst->specularAmount;
			if (*a < 0) *a = 0;
			if (*a > 100) *a = 100;
		}
	}
	PAN_KILL(panl, pan);
	return AFUNC_OK;
}

XCALL_(int) Activate(long version, GlobalFunc *global, void *local, void *serverData)
{
	ShaderHandler *h = (ShaderHandler *)local;
	XCALL_INIT;
	if (version < 1) return AFUNC_BADVERSION;
	h->create = (void *)Create; h->destroy = (void *)Destroy; h->load = (void *)Load; h->save = (void *)Save; h->copy = (void *)Copy; h->init = (void *)Init; h->cleanup = (void *)Cleanup; h->newTime = (void *)NewTime; h->evaluate = (void *)Evaluate; h->flags = (void *)Flags; h->descln = (void *)DescLn;
	gfunc = global; msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	return msg ? AFUNC_OK : AFUNC_BADGLOBAL;
}

ServerRecord ServerDesc[] = {
	{ "ShaderHandler", "NormalMap", (ActivateFunc *)Activate },
	{ "ShaderInterface", "NormalMap", (ActivateFunc *)Interface },
	{ 0 }
};
