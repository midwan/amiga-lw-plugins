/*
 * POSTFILTER.C -- configurable post-render image filter for LightWave 3D.
 *
 * The filter performs its enabled operations in a documented fixed order:
 * colour correction, grain, blur, edge-preserving smoothing,
 * then unsharp masking.  It uses only LightWave's image-filter API and is an
 * original implementation.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

#include <stddef.h>
#include <string.h>
#include <math.h>

#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;

#define POSTFILTER_VERSION 1
#define REGION_FULL 0
#define REGION_OBJECTS 1
#define REGION_BACKGROUND 2
#define BLUR_BOX 0
#define BLUR_GAUSSIAN 1

#define CLAMP_BYTE(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))
#define ABS_INT(v) ((v) < 0 ? -(v) : (v))

typedef struct {
	int region;
	int enableColor;
	int red, green, blue;
	int brightness, contrast, gamma, saturation;
	int greyscale, negative, posterize;
	int enableNoise, noise;
	int enableBlur, blurRadius, blurStyle;
	int enableSharpen, sharpen;
	int enableSmooth, variance;
	int passes;
} PostFilterInst;

/* Shared scene-serialization buffer: LightWave calls load/save with a
 * small stack, so the line buffer lives in static data, not on it. */
static char iobuf[256];

static MessageFuncs *msg;
static char descBuf[64];

static void *
plugin_alloc(unsigned long size)
{
	unsigned long *p;
	p = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!p) return 0;
	*p = size + 4;
	return p + 1;
}

static void
plugin_free(void *ptr)
{
	unsigned long *p;
	if (!ptr) return;
	p = ((unsigned long *)ptr) - 1;
	FreeMem(p, *p);
}

static int
clamp_int(int value, int low, int high)
{
	if (value < low) return low;
	if (value > high) return high;
	return value;
}

static void
int_to_str(int value, char *buf, int buflen)
{
	char tmp[12];
	int i = 0, neg = 0, len, j;
	if (value < 0) { neg = 1; value = -value; }
	if (!value) tmp[i++] = '0';
	while (value && i < 11) {
		tmp[i++] = (char)('0' + value % 10);
		value /= 10;
	}
	len = i + neg;
	if (len >= buflen) len = buflen - 1;
	if (neg) buf[0] = '-';
	for (j = 0; j < i && j + neg < buflen - 1; j++)
		buf[j + neg] = tmp[i - j - 1];
	buf[len] = '\0';
}

static const char *
parse_int(const char *s, int *value)
{
	int neg = 0;
	*value = 0;
	while (*s == ' ') s++;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') {
		*value = *value * 10 + (*s - '0');
		s++;
	}
	if (neg) *value = -*value;
	return s;
}

static void
append_int(char *buf, int *pos, int value)
{
	char number[12];
	int i;
	int_to_str(value, number, sizeof(number));
	if (*pos) buf[(*pos)++] = ' ';
	for (i = 0; number[i]; i++) buf[(*pos)++] = number[i];
	buf[*pos] = '\0';
}

static void
normalise(PostFilterInst *inst)
{
	inst->region = clamp_int(inst->region, REGION_FULL, REGION_BACKGROUND);
	inst->red = clamp_int(inst->red, 0, 200);
	inst->green = clamp_int(inst->green, 0, 200);
	inst->blue = clamp_int(inst->blue, 0, 200);
	inst->brightness = clamp_int(inst->brightness, 0, 200);
	inst->contrast = clamp_int(inst->contrast, 0, 200);
	inst->gamma = clamp_int(inst->gamma, 10, 300);
	inst->saturation = clamp_int(inst->saturation, 0, 200);
	inst->posterize = clamp_int(inst->posterize, 0, 32);
	inst->noise = clamp_int(inst->noise, 0, 100);
	inst->blurRadius = clamp_int(inst->blurRadius, 1, 12);
	inst->blurStyle = clamp_int(inst->blurStyle, BLUR_BOX, BLUR_GAUSSIAN);
	inst->sharpen = clamp_int(inst->sharpen, 0, 200);
	inst->variance = clamp_int(inst->variance, 0, 255);
	inst->passes = clamp_int(inst->passes, 1, 4);
}

XCALL_(static LWInstance)
Create(LWError *err)
{
	PostFilterInst *inst;
	XCALL_INIT;
	inst = (PostFilterInst *)plugin_alloc(sizeof(*inst));
	if (!inst) return 0;
	inst->region = REGION_FULL;
	inst->enableColor = 1;
	inst->red = inst->green = inst->blue = 100;
	inst->brightness = inst->contrast = inst->gamma = inst->saturation = 100;
	inst->greyscale = inst->negative = 0;
	inst->posterize = 0;
	inst->enableNoise = inst->noise = 0;
	inst->enableBlur = 0;
	inst->blurRadius = 1;
	inst->blurStyle = BLUR_GAUSSIAN;
	inst->enableSharpen = inst->sharpen = 0;
	inst->enableSmooth = 0;
	inst->variance = 20;
	inst->passes = 1;
	return inst;
}

XCALL_(static void)
Destroy(PostFilterInst *inst)
{
	XCALL_INIT;
	plugin_free(inst);
}

XCALL_(static LWError)
Copy(PostFilterInst *from, PostFilterInst *to)
{
	XCALL_INIT;
	*to = *from;
	return 0;
}

XCALL_(static LWError)
Load(PostFilterInst *inst, const LWLoadState *ls)
{
	char *buf = iobuf;
	const char *p;
	int version;
	int *values = &inst->region;
	int i;
	XCALL_INIT;
	if (ls->ioMode == LWIO_SCENE) {
		if (!spi_read_line(ls, buf, sizeof(iobuf)) || !buf[0]) return 0;
		p = parse_int(buf, &version);
		if (version != POSTFILTER_VERSION) return 0;
		for (i = 0; i < 22; i++) p = parse_int(p, values + i);
	} else {
		if (!spi_read_i32be(ls, &version) || version != POSTFILTER_VERSION) return 0;
		for (i = 0; i < 22; i++)
			if (!spi_read_i32be(ls, values + i)) return 0;
	}
	normalise(inst);
	return 0;
}

XCALL_(static LWError)
Save(PostFilterInst *inst, const LWSaveState *ss)
{
	char *buf = iobuf;
	int pos = 0;
	int *values = &inst->region;
	int i;
	XCALL_INIT;
	append_int(buf, &pos, POSTFILTER_VERSION);
	for (i = 0; i < 22; i++) append_int(buf, &pos, values[i]);
	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
	} else {
		spi_write_i32be(ss, POSTFILTER_VERSION);
		for (i = 0; i < 22; i++) spi_write_i32be(ss, values[i]);
	}
	return 0;
}

static int
in_region(const unsigned char *mask, unsigned long pixel)
{
	return mask[pixel] != 0;
}

static void
blur_horizontal(unsigned char *source, unsigned char *target,
	const unsigned char *mask, int width, int height, int radius, int style)
{
	int x, y, dx, channel;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned long pixel = (unsigned long)y * width + x;
			unsigned long base = pixel * 3;
			if (!in_region(mask, pixel)) {
				target[base] = source[base]; target[base + 1] = source[base + 1]; target[base + 2] = source[base + 2];
				continue;
			}
			for (channel = 0; channel < 3; channel++) {
				int sum = 0, weight = 0;
				for (dx = -radius; dx <= radius; dx++) {
					int nx = x + dx;
					int w;
					unsigned long neighbour;
					if (nx < 0 || nx >= width) continue;
					neighbour = (unsigned long)y * width + nx;
					if (!in_region(mask, neighbour)) continue;
					w = style == BLUR_GAUSSIAN ? radius + 1 - ABS_INT(dx) : 1;
					sum += source[neighbour * 3 + channel] * w;
					weight += w;
				}
				target[base + channel] = (unsigned char)(weight > 0 ? (sum / weight) : source[base + channel]);
			}
		}
	}
}

static void
blur_vertical(unsigned char *source, unsigned char *target,
	const unsigned char *mask, int width, int height, int radius, int style)
{
	int x, y, dy, channel;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned long pixel = (unsigned long)y * width + x;
			unsigned long base = pixel * 3;
			if (!in_region(mask, pixel)) {
				target[base] = source[base]; target[base + 1] = source[base + 1]; target[base + 2] = source[base + 2];
				continue;
			}
			for (channel = 0; channel < 3; channel++) {
				int sum = 0, weight = 0;
				for (dy = -radius; dy <= radius; dy++) {
					int ny = y + dy;
					int w;
					unsigned long neighbour;
					if (ny < 0 || ny >= height) continue;
					neighbour = (unsigned long)ny * width + x;
					if (!in_region(mask, neighbour)) continue;
					w = style == BLUR_GAUSSIAN ? radius + 1 - ABS_INT(dy) : 1;
					sum += source[neighbour * 3 + channel] * w;
					weight += w;
				}
				target[base + channel] = (unsigned char)(weight > 0 ? (sum / weight) : source[base + channel]);
			}
		}
	}
}

static int
luminance(const unsigned char *rgb)
{
	return (rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8;
}

/* Grain seed for one frame: the pattern changes from frame to frame but is
 * the same each time a given frame is rendered (re-renders, ScreamerNet). */
static unsigned long
grain_seed(LWFrame frame)
{
	unsigned long h = (unsigned long)frame * 0x9E3779B9UL + 0x13579BDFUL;
	h ^= h >> 16;
	h *= 0x7FEB352DUL;
	h ^= h >> 15;
	return h;
}

static void
apply_smooth(unsigned char *source, unsigned char *target, const unsigned char *mask,
	int width, int height, int variance)
{
	int x, y, dx, dy;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned long pixel = (unsigned long)y * width + x;
			unsigned long base = pixel * 3;
			int centre = luminance(source + base);
			int rsum = 0, gsum = 0, bsum = 0, count = 0;
			if (!in_region(mask, pixel)) {
				target[base] = source[base]; target[base + 1] = source[base + 1]; target[base + 2] = source[base + 2];
				continue;
			}
			for (dy = -1; dy <= 1; dy++) {
				int ny = y + dy;
				unsigned long rowOffset;
				if (ny < 0 || ny >= height) continue;
				rowOffset = (unsigned long)ny * width;
				for (dx = -1; dx <= 1; dx++) {
					int nx = x + dx;
					unsigned long neighbour;
					unsigned long nbase;
					if (nx < 0 || nx >= width) continue;
					neighbour = rowOffset + nx;
					nbase = neighbour * 3;
					if (!in_region(mask, neighbour) || ABS_INT(luminance(source + nbase) - centre) > variance) continue;
					rsum += source[nbase];
					gsum += source[nbase + 1];
					bsum += source[nbase + 2];
					count++;
				}
			}
			if (count > 0) {
				target[base] = (unsigned char)(rsum / count);
				target[base + 1] = (unsigned char)(gsum / count);
				target[base + 2] = (unsigned char)(bsum / count);
			} else {
				target[base] = source[base]; target[base + 1] = source[base + 1]; target[base + 2] = source[base + 2];
			}
		}
	}
}

static void
apply_sharpen(unsigned char *source, unsigned char *target, const unsigned char *mask,
	int width, int height, int amount)
{
	int x, y, dx, dy;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned long pixel = (unsigned long)y * width + x;
			unsigned long base = pixel * 3;
			int rsum = 0, gsum = 0, bsum = 0, count = 0;
			if (!in_region(mask, pixel)) {
				target[base] = source[base]; target[base + 1] = source[base + 1]; target[base + 2] = source[base + 2];
				continue;
			}
			for (dy = -1; dy <= 1; dy++) {
				int ny = y + dy;
				unsigned long rowOffset;
				if (ny < 0 || ny >= height) continue;
				rowOffset = (unsigned long)ny * width;
				for (dx = -1; dx <= 1; dx++) {
					int nx = x + dx;
					unsigned long neighbour;
					unsigned long nbase;
					if (nx < 0 || nx >= width) continue;
					neighbour = rowOffset + nx;
					if (!in_region(mask, neighbour)) continue;
					nbase = neighbour * 3;
					rsum += source[nbase];
					gsum += source[nbase + 1];
					bsum += source[nbase + 2];
					count++;
				}
			}
			if (count > 0) {
				target[base] = (unsigned char)CLAMP_BYTE(source[base] + amount * (source[base] - rsum / count) / 100);
				target[base + 1] = (unsigned char)CLAMP_BYTE(source[base + 1] + amount * (source[base + 1] - gsum / count) / 100);
				target[base + 2] = (unsigned char)CLAMP_BYTE(source[base + 2] + amount * (source[base + 2] - bsum / count) / 100);
			} else {
				target[base] = source[base]; target[base + 1] = source[base + 1]; target[base + 2] = source[base + 2];
			}
		}
	}
}

XCALL_(static void)
Process(PostFilterInst *inst, const FilterAccess *fa)
{
	unsigned long pixels;
	unsigned long pixel;
	unsigned char *front = 0, *back = 0, *mask = 0, *gammaTable = 0;
	int x, y, pass;
	int needBack;
	unsigned long random;
	XCALL_INIT;
	if (fa->width <= 0 || fa->height <= 0) return;
	pixels = (unsigned long)fa->width * (unsigned long)fa->height;
	needBack = inst->enableBlur || inst->enableSmooth ||
		(inst->enableSharpen && inst->sharpen);
	front = (unsigned char *)plugin_alloc(pixels * 3);
	if (needBack) back = (unsigned char *)plugin_alloc(pixels * 3);
	mask = (unsigned char *)plugin_alloc(pixels);
	if (!front || !mask || (needBack && !back)) goto done;
	if (inst->enableColor) {
		gammaTable = (unsigned char *)plugin_alloc(256);
		if (!gammaTable) goto done;
		for (x = 0; x < 256; x++) {
			if (inst->gamma == 100) gammaTable[x] = (unsigned char)x;
			else gammaTable[x] = (unsigned char)CLAMP_BYTE((int)(pow((double)x / 255.0, 100.0 / inst->gamma) * 255.0 + 0.5));
		}
	}
	for (y = 0; y < fa->height; y++) {
		BufferValue *red = (*fa->bufLine)(LWBUF_RED, y);
		BufferValue *green = (*fa->bufLine)(LWBUF_GREEN, y);
		BufferValue *blue = (*fa->bufLine)(LWBUF_BLUE, y);
		BufferValue *alpha = (*fa->bufLine)(LWBUF_ALPHA, y);
		if (!red || !green || !blue || !alpha) goto done;
		for (x = 0; x < fa->width; x++) {
			unsigned long base = ((unsigned long)y * fa->width + x) * 3;
			pixel = (unsigned long)y * fa->width + x;
			front[base] = red[x]; front[base + 1] = green[x]; front[base + 2] = blue[x];
			mask[pixel] = inst->region == REGION_FULL ||
				(inst->region == REGION_OBJECTS && alpha[x] != 0) ||
				(inst->region == REGION_BACKGROUND && alpha[x] == 0);
		}
	}
	if (inst->enableColor) {
		for (pixel = 0; pixel < pixels; pixel++) {
			unsigned long base = pixel * 3;
			int r, g, b, lum;
			if (!in_region(mask, pixel)) continue;
			r = front[base] * inst->red / 100;
			g = front[base + 1] * inst->green / 100;
			b = front[base + 2] * inst->blue / 100;
			r = r * inst->brightness / 100;
			g = g * inst->brightness / 100;
			b = b * inst->brightness / 100;
			r = ((r - 128) * inst->contrast / 100) + 128;
			g = ((g - 128) * inst->contrast / 100) + 128;
			b = ((b - 128) * inst->contrast / 100) + 128;
			r = gammaTable[CLAMP_BYTE(r)]; g = gammaTable[CLAMP_BYTE(g)]; b = gammaTable[CLAMP_BYTE(b)];
			lum = (r * 77 + g * 150 + b * 29) >> 8;
			r = lum + (r - lum) * inst->saturation / 100;
			g = lum + (g - lum) * inst->saturation / 100;
			b = lum + (b - lum) * inst->saturation / 100;
			if (inst->greyscale) r = g = b = (r * 77 + g * 150 + b * 29) >> 8;
			if (inst->negative) { r = 255 - r; g = 255 - g; b = 255 - b; }
			if (inst->posterize >= 2) {
				/* Nearest-level quantisation: equal-width input bands, so
				 * the brightest output level is reachable below 255. */
				r = ((r * (inst->posterize - 1) + 127) / 255) * 255 / (inst->posterize - 1);
				g = ((g * (inst->posterize - 1) + 127) / 255) * 255 / (inst->posterize - 1);
				b = ((b * (inst->posterize - 1) + 127) / 255) * 255 / (inst->posterize - 1);
			}
			front[base] = (unsigned char)CLAMP_BYTE(r);
			front[base + 1] = (unsigned char)CLAMP_BYTE(g);
			front[base + 2] = (unsigned char)CLAMP_BYTE(b);
		}
	}
	if (inst->enableNoise && inst->noise) {
		random = grain_seed(fa->frame);
		for (pixel = 0; pixel < pixels; pixel++) {
			unsigned long base = pixel * 3;
			int grain;
			if (!in_region(mask, pixel)) continue;
			random = random * 1103515245UL + 12345UL;
			grain = (int)((random >> 16) & 255) - 128;
			grain = grain * inst->noise / 100;
			front[base] = (unsigned char)CLAMP_BYTE(front[base] + grain);
			front[base + 1] = (unsigned char)CLAMP_BYTE(front[base + 1] + grain);
			front[base + 2] = (unsigned char)CLAMP_BYTE(front[base + 2] + grain);
		}
	}
	if (inst->enableBlur) {
		for (pass = 0; pass < inst->passes; pass++) {
			blur_horizontal(front, back, mask, fa->width, fa->height, inst->blurRadius, inst->blurStyle);
			blur_vertical(back, front, mask, fa->width, fa->height, inst->blurRadius, inst->blurStyle);
		}
	}
	if (inst->enableSmooth) {
		for (pass = 0; pass < inst->passes; pass++) {
			apply_smooth(front, back, mask, fa->width, fa->height, inst->variance);
			memcpy(front, back, pixels * 3);
		}
	}
	if (inst->enableSharpen && inst->sharpen) {
		apply_sharpen(front, back, mask, fa->width, fa->height, inst->sharpen);
		memcpy(front, back, pixels * 3);
	}
	for (y = 0; y < fa->height; y++) {
		BufferValue *alpha = (*fa->bufLine)(LWBUF_ALPHA, y);
		for (x = 0; x < fa->width; x++) {
			BufferValue rgb[3];
			unsigned long base = ((unsigned long)y * fa->width + x) * 3;
			rgb[0] = front[base]; rgb[1] = front[base + 1]; rgb[2] = front[base + 2];
			spi_set_rgb(fa, x, y, rgb);
			(*fa->setAlpha)(x, y, alpha ? alpha[x] : 255);
		}
	}
done:
	plugin_free(gammaTable);
	plugin_free(mask);
	plugin_free(back);
	plugin_free(front);
}

XCALL_(static unsigned int)
Flags(PostFilterInst *inst)
{
	XCALL_INIT;
	return 0;
}

XCALL_(static const char *)
DescLn(PostFilterInst *inst)
{
	int pos = 0;
	XCALL_INIT;
	descBuf[0] = '\0';
	if (inst->enableColor) { strcat(descBuf, "Colour"); pos = 1; }
	if (inst->enableBlur || inst->enableSmooth || inst->enableSharpen) {
		if (pos) strcat(descBuf, " + ");
		strcat(descBuf, "Filter"); pos = 1;
	}
	if (inst->enableNoise) { if (pos) strcat(descBuf, " + "); strcat(descBuf, "Grain"); }
	return descBuf[0] ? descBuf : "PostFilter (disabled)";
}

static const char *regionItems[] = { "Full frame", "Objects only", "Background only", 0 };
static const char *blurItems[] = { "Box", "Gaussian", 0 };

enum { CK_BOOL, CK_SLIDER, CK_POPUP };

typedef struct {
	char *label;		/* LWPanels takes a non-const label */
	unsigned char kind;
	unsigned char field;	/* int index into PostFilterInst */
	unsigned char sameRow;	/* sits to the right of the previous control */
	short min, max;
	const char **items;
} PanelCtl;

#define PF_FIELD(name) (offsetof(PostFilterInst, name) / sizeof(int))
#define SLIDER_W 100
#define ROW_GAP 8
#define COLUMN_GAP 16

/* Two columns keep the panel short on small Layout screens: region and
 * colour on the left, the filters on the right in the order Process runs
 * them. */
static const PanelCtl panelCtls[] = {
	{ .label = "Region", .kind = CK_POPUP, .field = PF_FIELD(region), .items = regionItems },
	{ .label = "Colour", .kind = CK_BOOL, .field = PF_FIELD(enableColor) },
	{ .label = "Red %", .kind = CK_SLIDER, .field = PF_FIELD(red), .min = 0, .max = 200 },
	{ .label = "Green %", .kind = CK_SLIDER, .field = PF_FIELD(green), .min = 0, .max = 200 },
	{ .label = "Blue %", .kind = CK_SLIDER, .field = PF_FIELD(blue), .min = 0, .max = 200 },
	{ .label = "Brightness %", .kind = CK_SLIDER, .field = PF_FIELD(brightness), .min = 0, .max = 200 },
	{ .label = "Contrast %", .kind = CK_SLIDER, .field = PF_FIELD(contrast), .min = 0, .max = 200 },
	{ .label = "Gamma %", .kind = CK_SLIDER, .field = PF_FIELD(gamma), .min = 10, .max = 300 },
	{ .label = "Saturation %", .kind = CK_SLIDER, .field = PF_FIELD(saturation), .min = 0, .max = 200 },
	{ .label = "Greyscale", .kind = CK_BOOL, .field = PF_FIELD(greyscale) },
	{ .label = "Negative", .kind = CK_BOOL, .field = PF_FIELD(negative), .sameRow = 1 },
	{ .label = "Posterize", .kind = CK_SLIDER, .field = PF_FIELD(posterize), .min = 0, .max = 32 },
	{ .label = "Grain", .kind = CK_BOOL, .field = PF_FIELD(enableNoise) },
	{ .label = "Grain Amount", .kind = CK_SLIDER, .field = PF_FIELD(noise), .min = 0, .max = 100 },
	{ .label = "Blur", .kind = CK_BOOL, .field = PF_FIELD(enableBlur) },
	{ .label = "Blur Radius", .kind = CK_SLIDER, .field = PF_FIELD(blurRadius), .min = 1, .max = 12 },
	{ .label = "Blur Style", .kind = CK_POPUP, .field = PF_FIELD(blurStyle), .items = blurItems },
	{ .label = "Smooth", .kind = CK_BOOL, .field = PF_FIELD(enableSmooth) },
	{ .label = "Smooth Variance", .kind = CK_SLIDER, .field = PF_FIELD(variance), .min = 0, .max = 255 },
	{ .label = "Passes", .kind = CK_SLIDER, .field = PF_FIELD(passes), .min = 1, .max = 4 },
	{ .label = "Sharpen", .kind = CK_BOOL, .field = PF_FIELD(enableSharpen) },
	{ .label = "Sharpen Amount", .kind = CK_SLIDER, .field = PF_FIELD(sharpen), .min = 0, .max = 200 },
};

#define LEFT_COUNT 12
#define CTL_COUNT ((int)(sizeof(panelCtls) / sizeof(panelCtls[0])))

/* Panel state lives in static data: the panel is modal, and the LWPanels
 * macros need a desc and value named desc/ival in scope. */
static LWControl *panelCon[CTL_COUNT];
static LWPanControlDesc desc;
static LWValue ival = { LWT_INTEGER };

/* The CON_* macros all return the shared ival, so two of them in one
 * expression read the same value; these return a copy instead. */
static int
con_get(LWControl *ctl, int tag)
{
	(*ctl->get)(ctl, tag, &ival);
	return ival.intv.value;
}

static void
con_move(LWControl *ctl, int x, int y)
{
	MOVE_CON(ctl, x, y);
}

/* Place one column of auto-placed controls with its left edge at x and its
 * rows moved by dy. Labels are right-aligned so every slider, popup and
 * check box starts at the same x. A row shared by several controls (the
 * sameRow ones go to the right of the previous control) starts at x so it
 * stays within the column, and the rows below move up by the rows freed. */
static void
place_column(LWControl **con, const PanelCtl *spec, int count, int x, int dy)
{
	int i, label, hot = 0, lift = 0, prevAuto = 0;
	for (i = 0; i < count; i++) {
		if (spec[i].sameRow) continue;
		label = con_get(con[i], CTL_HOTX) - con_get(con[i], CTL_X);
		if (label > hot) hot = label;
	}
	for (i = 0; i < count; i++) {
		int autoY = con_get(con[i], CTL_Y);
		if (spec[i].sameRow && i > 0) {
			con_move(con[i],
				con_get(con[i - 1], CTL_X) + con_get(con[i - 1], CTL_W) + ROW_GAP,
				con_get(con[i - 1], CTL_Y));
			lift += autoY - prevAuto;
		} else if (i + 1 < count && spec[i + 1].sameRow) {
			con_move(con[i], x, autoY - lift + dy);
		} else {
			label = con_get(con[i], CTL_HOTX) - con_get(con[i], CTL_X);
			con_move(con[i], x + hot - label, autoY - lift + dy);
		}
		prevAuto = autoY;
	}
}

static void
layout_panel(LWPanelFuncs *panl, LWPanelID pan)
{
	int i, edge, autoBottom = 0, bottom = 0, right = 0, margin, dy;
	for (i = 0; i < CTL_COUNT; i++) {
		edge = con_get(panelCon[i], CTL_Y) + con_get(panelCon[i], CTL_H);
		if (edge > autoBottom) autoBottom = edge;
	}
	/* Space LWPanels leaves under the controls for its OK/Cancel row. */
	autoBottom = PAN_GETH(panl, pan) - autoBottom;
	margin = con_get(panelCon[0], CTL_X);
	dy = con_get(panelCon[0], CTL_Y) - con_get(panelCon[LEFT_COUNT], CTL_Y);
	place_column(panelCon, panelCtls, LEFT_COUNT, margin, 0);
	for (i = 0; i < LEFT_COUNT; i++) {
		edge = con_get(panelCon[i], CTL_X) + con_get(panelCon[i], CTL_W);
		if (edge > right) right = edge;
	}
	place_column(panelCon + LEFT_COUNT, panelCtls + LEFT_COUNT, CTL_COUNT - LEFT_COUNT,
		right + COLUMN_GAP, dy);
	for (i = 0; i < CTL_COUNT; i++) {
		edge = con_get(panelCon[i], CTL_X) + con_get(panelCon[i], CTL_W);
		if (edge > right) right = edge;
		edge = con_get(panelCon[i], CTL_Y) + con_get(panelCon[i], CTL_H);
		if (edge > bottom) bottom = edge;
	}
	PAN_SETW(panl, pan, right + margin);
	PAN_SETH(panl, pan, bottom + autoBottom);
}

XCALL_(static int)
Interface(long version, GlobalFunc *global, PostFilterInst *inst, void *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID pan;
	int *values = &inst->region;
	int i;
	XCALL_INIT;
	if (version != 1) return AFUNC_BADVERSION;
	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	if (!panl) return AFUNC_OK;
	pan = PAN_CREATE(panl, "PostFilter v" PLUGIN_VERSION);
	if (!pan) return AFUNC_OK;
	for (i = 0; i < CTL_COUNT; i++) {
		const PanelCtl *c = &panelCtls[i];
		if (c->kind == CK_BOOL)
			panelCon[i] = BOOL_CTL(panl, pan, c->label);
		else if (c->kind == CK_SLIDER)
			panelCon[i] = SLIDER_CTL(panl, pan, c->label, SLIDER_W, c->min, c->max);
		else
			panelCon[i] = POPUP_CTL(panl, pan, c->label, c->items);
		if (!panelCon[i]) goto done;
	}
	layout_panel(panl, pan);
	for (i = 0; i < CTL_COUNT; i++) SET_INT(panelCon[i], values[panelCtls[i].field]);
	if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
		for (i = 0; i < CTL_COUNT; i++) GET_INT(panelCon[i], values[panelCtls[i].field]);
		normalise(inst);
	}
done:
	PAN_KILL(panl, pan);
	return AFUNC_OK;
}

XCALL_(int)
Activate(long version, GlobalFunc *global, void *local, void *serverData)
{
	ImageFilterHandler *handler = (ImageFilterHandler *)local;
	XCALL_INIT;
	if (version < 1) return AFUNC_BADVERSION;
	handler->create = (void *)Create; handler->destroy = (void *)Destroy; handler->copy = (void *)Copy;
	handler->load = (void *)Load; handler->save = (void *)Save; handler->process = (void *)Process;
	handler->flags = (void *)Flags; handler->descln = (void *)DescLn; handler->useItems = 0; handler->changeID = 0;
	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	return msg ? AFUNC_OK : AFUNC_BADGLOBAL;
}

ServerRecord ServerDesc[] = {
	{ "ImageFilterHandler", "PostFilter", (ActivateFunc *)Activate },
	{ "ImageFilterInterface", "PostFilter", (ActivateFunc *)Interface },
	{ 0 }
};
