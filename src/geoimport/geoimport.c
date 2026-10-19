/*
 * GEOIMPORT.C -- VideoScape 3D / Aegis Modeler 3D .geo Object Loader
 * Copyright (c) 2026 D. Panokostas
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Imports VideoScape 3D geometry files into LightWave 3D 5.x. Reads the
 * text format (3DG1) and the binary format written by Modeler 3D's
 * "Save As - Binary" (3DB1, big-endian words and Motorola FFP floats).
 * VideoScape and LightWave share the same axes (+X right, +Y up, +Z
 * forward) and clockwise polygon winding, so points and polygons are
 * passed through unchanged. Detail polygons are kept as LightWave detail
 * polygons, and each colour code becomes a surface with matching colour
 * and shading settings.
 *
 * Uses AllocMem/FreeMem and AmigaOS DOS -- no libnix runtime.
 */

#include <splug.h>
#include <lwbase.h>

#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <exec/memory.h>

extern struct DosLibrary *DOSBase;
extern struct ExecBase   *SysBase;

/* AmigaOS version string */
static const char __attribute__((used)) verstag[] =
	"\0$VER: VideoScape_GEO_loader " PLUGIN_VERSION " (c) D. Panokostas";

/* ----------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------- */

#define READ_BUF_SIZE   4096
#define MAX_POLY_PTS    200     /* LightWave 5 polygon size limit */
#define MAX_POINTS      65536   /* polygon indices are UWORDs */
#define TOKEN_SIZE      40
#define NAME_SIZE       40
#define SURF_DATA_SIZE  96
#define NUM_CODES       261     /* palette codes 0-255, specials 256-260 */

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
 * Loader state (heap allocated: plugin callbacks have little stack)
 * ---------------------------------------------------------------- */

typedef struct {
	ObjectImport   *obj;
	BPTR            fh;
	int             binary;
	int             npts;
	const char     *error;      /* set when result is OBJSTAT_FAILED */
	int             aborted;
	int             monitoring;

	/* buffered input */
	unsigned char   buf[READ_BUF_SIZE];
	long            pos;
	long            len;
	int             eof;

	/* surface id + 1 per palette code, 0 = not looked up yet */
	int             surf[NUM_CODES];

	float           pt[3];
	unsigned short  idx[MAX_POLY_PTS];
	char            token[TOKEN_SIZE];
	char            name[NAME_SIZE];
	unsigned char   sdata[SURF_DATA_SIZE];
} GeoReader;

static int
fill_buffer(GeoReader *r)
{
	if (r->eof)
		return 0;
	r->len = Read(r->fh, r->buf, READ_BUF_SIZE);
	r->pos = 0;
	if (r->len <= 0) {
		r->len = 0;
		r->eof = 1;
		return 0;
	}
	if (r->monitoring && MON_STEP(r->obj->monitor))
		r->aborted = 1;
	return 1;
}

/* Next byte, or -1 at end of file. */
static int
next_byte(GeoReader *r)
{
	if (r->pos >= r->len && !fill_buffer(r))
		return -1;
	return r->buf[r->pos++];
}

static int
is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
	       c == '\f' || c == '\v';
}

/*
 * Read one whitespace-separated token into r->token. 3DG1 is a stream of
 * numbers, so line breaks (LF, CR or CRLF) and blank lines need no special
 * handling. Returns the token length, 0 at end of file, -1 if too long.
 */
static int
read_token(GeoReader *r)
{
	int c, n = 0;

	do {
		c = next_byte(r);
	} while (c >= 0 && is_space(c));

	while (c >= 0 && !is_space(c)) {
		if (n >= TOKEN_SIZE - 1)
			return -1;
		r->token[n++] = (char)c;
		c = next_byte(r);
	}
	r->token[n] = '\0';
	return n;
}

/* ----------------------------------------------------------------
 * Number parsing
 * ---------------------------------------------------------------- */

/*
 * Parse a decimal integer, or a 0x-prefixed hex value (Blender writes
 * face colours that way). Sets *hex when the token was hexadecimal.
 */
static int
parse_int(const char *s, long *out, int *hex)
{
	unsigned long v = 0;
	int neg = 0, digits = 0, d;

	*hex = 0;
	if (*s == '+' || *s == '-')
		neg = (*s++ == '-');

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		*hex = 1;
		s += 2;
		for (;; s++) {
			if (*s >= '0' && *s <= '9')      d = *s - '0';
			else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
			else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
			else break;
			if (++digits > 8)
				return 0;
			v = (v << 4) | (unsigned long)d;
		}
	} else {
		for (; *s >= '0' && *s <= '9'; s++) {
			if (++digits > 9)
				return 0;
			v = v * 10 + (unsigned long)(*s - '0');
		}
	}

	if (digits == 0 || *s != '\0')
		return 0;
	*out = neg ? -(long)v : (long)v;
	return 1;
}

static const double pow10_tab[] = {
	1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9
};

/*
 * Parse a decimal number with optional fraction and exponent ("-.25",
 * "1.0", "3e-05"). Up to nine significant digits are kept.
 */
static int
parse_float(const char *s, float *out)
{
	unsigned long mant = 0;
	int neg = 0, digits = 0, sig = 0, exp10 = 0, e, eneg;
	double v;

	if (*s == '+' || *s == '-')
		neg = (*s++ == '-');

	for (; *s >= '0' && *s <= '9'; s++, digits++) {
		if (sig < 9) {
			mant = mant * 10 + (unsigned long)(*s - '0');
			if (mant) sig++;
		} else {
			exp10++;
		}
	}
	if (*s == '.') {
		for (s++; *s >= '0' && *s <= '9'; s++, digits++) {
			if (sig < 9) {
				mant = mant * 10 + (unsigned long)(*s - '0');
				if (mant) sig++;
				exp10--;
			}
		}
	}
	if (digits == 0)
		return 0;

	if (*s == 'e' || *s == 'E') {
		s++;
		eneg = 0;
		if (*s == '+' || *s == '-')
			eneg = (*s++ == '-');
		if (*s < '0' || *s > '9')
			return 0;
		for (e = 0; *s >= '0' && *s <= '9'; s++)
			if (e < 1000)
				e = e * 10 + (*s - '0');
		exp10 += eneg ? -e : e;
	}
	if (*s != '\0')
		return 0;

	v = (double)mant;
	if (mant) {
		if (exp10 > 38)
			return 0;
		if (exp10 < -45)
			mant = 0, v = 0.0, exp10 = 0;
		while (exp10 > 9)  { v *= 1e9; exp10 -= 9; }
		while (exp10 < -9) { v /= 1e9; exp10 += 9; }
		if (exp10 > 0)
			v *= pow10_tab[exp10];
		else if (exp10 < 0)
			v /= pow10_tab[-exp10];
	}

	*out = (float)(neg ? -v : v);
	return 1;
}

/*
 * Motorola FFP: bits 31-8 mantissa (normalised, top bit set), bit 7 sign,
 * bits 6-0 exponent biased by 64. The value is mant/2^24 * 2^(exp-64),
 * which maps to an IEEE single with exponent exp+62 and the mantissa's
 * implicit top bit dropped.
 */
static float
ffp_to_float(unsigned long ffp)
{
	unsigned long ieee;
	float out;

	if (!(ffp & 0x80000000UL))
		return 0.0f;
	ieee = ((ffp & 0x80UL) << 24) |
	       (((ffp & 0x7FUL) + 62UL) << 23) |
	       ((ffp >> 8) & 0x007FFFFFUL);
	memcpy(&out, &ieee, sizeof(out));
	return out;
}

/* ----------------------------------------------------------------
 * Typed reads for both formats
 * ---------------------------------------------------------------- */

/* Binary big-endian word. Returns 0 at end of file. */
static int
read_word(GeoReader *r, unsigned long *out)
{
	int hi, lo;

	if ((hi = next_byte(r)) < 0) return 0;
	if ((lo = next_byte(r)) < 0) return 0;
	*out = ((unsigned long)hi << 8) | (unsigned long)lo;
	return 1;
}

/*
 * Read an integer field: a word in 3DB1, a token in 3DG1. Returns 1 on
 * success, 0 at end of file, -1 on malformed data.
 */
static int
read_int(GeoReader *r, long *out, int *hex)
{
	unsigned long w;
	int n;

	*hex = 0;
	if (r->binary) {
		if (!read_word(r, &w))
			return 0;
		*out = (long)w;
		return 1;
	}
	n = read_token(r);
	if (n == 0)
		return 0;
	if (n < 0 || !parse_int(r->token, out, hex))
		return -1;
	return 1;
}

static int
read_coord(GeoReader *r, float *out)
{
	unsigned long hi, lo;
	int n;

	if (r->binary) {
		if (!read_word(r, &hi) || !read_word(r, &lo))
			return 0;
		*out = ffp_to_float((hi << 16) | lo);
		return 1;
	}
	n = read_token(r);
	if (n == 0)
		return 0;
	if (n < 0 || !parse_float(r->token, out))
		return -1;
	return 1;
}

/* ----------------------------------------------------------------
 * Surfaces
 *
 * VideoScape colour code (Modeler 3D manual, Appendix F):
 *   bits 0-3  colour (16-entry palette)
 *   +16 glossy, +32 unshaded, +48 outline, +64 transparent, +128 smooth
 *   256-258 invisible variants, 259/260 chrome (flat/smoothed)
 * Blender exports 0xBBGGRR hex values instead, with emission in bits
 * 24-31.
 * ---------------------------------------------------------------- */

static const char *const color_names[16] = {
	"Black", "Dk.Blue", "Dk.Green", "Dk.Cyan",
	"Dk.Red", "Dk.Purple", "Brown", "Gray",
	"Black", "Lt.Blue", "Lt.Green", "Lt.Cyan",
	"Lt.Red", "Lt.Purple", "Yellow", "White"
};

/* RGB per palette entry, as used by the AmigaWorld GEO ray tracer */
static const unsigned char color_rgb[16][3] = {
	{   0,   0,   0 }, {   0,   0, 127 }, {   0, 127,   0 }, {   0, 127, 127 },
	{ 127,   0,   0 }, { 127,   0, 127 }, { 255, 100,   0 }, { 170, 170, 170 },
	{   0,   0,   0 }, {   0,   0, 255 }, {   0, 255,   0 }, {   0, 255, 255 },
	{ 255,   0,   0 }, { 255,   0, 255 }, { 255, 255,   0 }, { 255, 255, 255 }
};

static const char *const special_names[5] = {
	"Invisible", "Invisible Darken", "Invisible Brighten",
	"Chrome", "Chrome Smooth"
};

/* LWOB SURF FLAG bits */
#define SF_OUTLINE      (1 << 1)
#define SF_SMOOTHING    (1 << 2)
#define SF_ADDITIVE     (1 << 9)

/* LWOB SURF sub-chunk values (256 = 100%) */
typedef struct {
	unsigned char rgb[3];
	unsigned short flags, lumi, diff, spec, glos, refl, tran;
} SurfParams;

static char *
append(char *p, char *end, const char *s)
{
	while (*s && p < end)
		*p++ = *s++;
	*p = '\0';
	return p;
}

static char *
append_uint(char *p, char *end, unsigned long v)
{
	char tmp[11];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < 10);
	while (n > 0 && p < end)
		*p++ = tmp[--n];
	*p = '\0';
	return p;
}

static char *
append_hex(char *p, char *end, unsigned long v, int digits)
{
	static const char hexd[] = "0123456789ABCDEF";

	while (digits-- > 0 && p < end)
		*p++ = hexd[(v >> (digits * 4)) & 15];
	*p = '\0';
	return p;
}

/* Name and parameters for a VideoScape colour code (0-260 or other). */
static void
describe_code(long code, char *name, SurfParams *sp)
{
	char *p = name, *end = name + NAME_SIZE - 1;
	int c = (int)(code & 15);

	memset(sp, 0, sizeof(*sp));
	sp->diff = 256;

	if (code >= 256 && code <= 260) {
		p = append(p, end, special_names[code - 256]);
		switch (code) {
		case 256:
			sp->tran = 256;
			break;
		case 257:
			sp->tran = 128;
			break;
		case 258:
			sp->rgb[0] = sp->rgb[1] = sp->rgb[2] = 255;
			sp->tran = 128;
			sp->flags = SF_ADDITIVE;
			break;
		default:
			sp->rgb[0] = sp->rgb[1] = sp->rgb[2] = 200;
			sp->diff = 64;
			sp->spec = 256;
			sp->glos = 256;
			sp->refl = 256;
			if (code == 260)
				sp->flags = SF_SMOOTHING;
			break;
		}
	} else if (code < 0 || code > 260) {
		p = append(p, end, "Code");
		sp->rgb[0] = sp->rgb[1] = sp->rgb[2] = 200;
	} else {
		p = append(p, end, color_names[c]);
		sp->rgb[0] = color_rgb[c][0];
		sp->rgb[1] = color_rgb[c][1];
		sp->rgb[2] = color_rgb[c][2];

		switch (code & 48) {
		case 16:
			p = append(p, end, " Glossy");
			sp->spec = 128;
			sp->glos = 64;
			break;
		case 32:
			p = append(p, end, " Unshaded");
			sp->lumi = 256;
			sp->diff = 0;
			break;
		case 48:
			p = append(p, end, " Outline");
			sp->flags |= SF_OUTLINE;
			break;
		}
		if (code & 64) {
			p = append(p, end, " Transparent");
			sp->tran = 128;
		}
		if (code & 128) {
			p = append(p, end, " Smooth");
			sp->flags |= SF_SMOOTHING;
		}
	}

	p = append(p, end, " (");
	p = append_uint(p, end, (unsigned long)(code < 0 ? -code : code));
	append(p, end, ")");
}

/* Name and parameters for a Blender 0xEEBBGGRR colour. */
static void
describe_rgb(unsigned long bgr, char *name, SurfParams *sp)
{
	char *p = name, *end = name + NAME_SIZE - 1;

	memset(sp, 0, sizeof(*sp));
	sp->rgb[0] = (unsigned char)(bgr & 0xFF);
	sp->rgb[1] = (unsigned char)((bgr >> 8) & 0xFF);
	sp->rgb[2] = (unsigned char)((bgr >> 16) & 0xFF);
	sp->diff = 256;
	sp->lumi = (unsigned short)(((bgr >> 24) & 0xFF) * 256 / 255);

	p = append(p, end, "RGB ");
	p = append_hex(p, end, ((unsigned long)sp->rgb[0] << 16) |
				((unsigned long)sp->rgb[1] << 8) | sp->rgb[2], 6);
	if (sp->lumi) {
		p = append(p, end, " Lum ");
		append_uint(p, end, (bgr >> 24) & 0xFF);
	}
}

static unsigned char *
put_sub(unsigned char *p, const char *id, int size)
{
	memcpy(p, id, 4);
	p[4] = 0;
	p[5] = (unsigned char)size;
	return p + 6;
}

static unsigned char *
put_short(unsigned char *p, const char *id, unsigned short v)
{
	p = put_sub(p, id, 2);
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
	return p + 2;
}

/* Build LWOB SURF sub-chunks (without the name) and send them. */
static void
send_surface(GeoReader *r, const SurfParams *sp)
{
	unsigned char *p = r->sdata;

	p = put_sub(p, "COLR", 4);
	p[0] = sp->rgb[0];
	p[1] = sp->rgb[1];
	p[2] = sp->rgb[2];
	p[3] = 0;
	p += 4;
	p = put_short(p, "FLAG", sp->flags);
	p = put_short(p, "LUMI", sp->lumi);
	p = put_short(p, "DIFF", sp->diff);
	p = put_short(p, "SPEC", sp->spec);
	p = put_short(p, "GLOS", sp->glos ? sp->glos : 64);
	p = put_short(p, "REFL", sp->refl);
	p = put_short(p, "TRAN", sp->tran);
	if (sp->flags & SF_SMOOTHING) {
		/* SMAN 89.5 degrees, IEEE single 0x42B30000 */
		p = put_sub(p, "SMAN", 4);
		p[0] = 0x42; p[1] = 0xB3; p[2] = 0; p[3] = 0;
		p += 4;
	}

	r->obj->surfData(r->obj->data, r->name, (int)(p - r->sdata), r->sdata);
}

/* Surface index for a polygon colour; defines the surface on first use. */
static int
surface_for(GeoReader *r, long code, int hex)
{
	SurfParams sp;
	int first = 0, si;

	if (!hex && code >= 0 && code < NUM_CODES && r->surf[code])
		return r->surf[code] - 1;

	if (hex)
		describe_rgb((unsigned long)code, r->name, &sp);
	else
		describe_code(code, r->name, &sp);

	si = r->obj->surfIndex(r->obj->data, r->name, &first);
	if (first)
		send_surface(r, &sp);

	if (!hex && code >= 0 && code < NUM_CODES)
		r->surf[code] = si + 1;
	return si;
}

/* ----------------------------------------------------------------
 * Mesh transfer
 * ---------------------------------------------------------------- */

static int
fail(GeoReader *r, const char *msg)
{
	if (!r->error)
		r->error = msg;
	return 0;
}

/*
 * The interface allows several points per points() call, but Modeler
 * 5.00f stores the first point of each call for all of them, so send
 * one point per call.
 */
static int
read_points(GeoReader *r)
{
	int i, k, rc;

	for (i = 0; i < r->npts; i++) {
		for (k = 0; k < 3; k++) {
			rc = read_coord(r, &r->pt[k]);
			if (rc == 0)
				return fail(r, "File ends inside the point list.");
			if (rc < 0)
				return fail(r, "Invalid point coordinate.");
		}
		r->obj->points(r->obj->data, 1, r->pt);
		if (r->aborted)
			return 0;
	}
	return 1;
}

/*
 * Read one polygon record: count, indices, colour. A negative colour
 * means the polygon has detail polygons; their count follows and they
 * are sent with OBJPOLF_DETAIL. Returns 1 on success, 0 on error, and
 * -1 at a clean end of file before a top-level polygon.
 */
static int
read_polygon(GeoReader *r, int detail)
{
	long nv, v, color, ndetail;
	int i, rc, hex, si;

	rc = read_int(r, &nv, &hex);
	if (rc == 0) {
		if (detail)
			return fail(r, "File ends inside a detail polygon list.");
		return -1;
	}
	if (rc < 0 || hex)
		return fail(r, "Invalid polygon point count.");
	if (r->binary && nv == 0 && !detail)
		return -1;          /* optional terminator */
	if (nv < 1)
		return fail(r, "Invalid polygon point count.");
	if (nv > MAX_POLY_PTS)
		return fail(r, "Polygon has more than 200 points.");

	for (i = 0; i < nv; i++) {
		rc = read_int(r, &v, &hex);
		if (rc == 0)
			return fail(r, "File ends inside a polygon.");
		if (rc < 0 || hex || v < 0 || v >= r->npts)
			return fail(r, "Polygon refers to a point that does not exist.");
		r->idx[i] = (unsigned short)v;
	}

	rc = read_int(r, &color, &hex);
	if (rc == 0)
		return fail(r, "File ends inside a polygon.");
	if (rc < 0)
		return fail(r, "Invalid polygon colour.");
	if (r->binary && color >= 0x8000)
		color -= 0x10000;   /* colour word is signed */

	ndetail = 0;
	if (color < 0 && !hex) {
		if (detail)
			return fail(r, "Detail polygons cannot have details.");
		color = -color;
		rc = read_int(r, &ndetail, &hex);
		if (rc == 0)
			return fail(r, "File ends before a detail polygon count.");
		if (rc < 0 || hex || ndetail < 0)
			return fail(r, "Invalid detail polygon count.");
	}

	si = surface_for(r, color, hex);
	r->obj->polygon(r->obj->data, (int)nv, si,
			detail ? OBJPOLF_DETAIL : OBJPOLF_FACE, r->idx);

	while (ndetail-- > 0)
		if (read_polygon(r, 1) != 1)
			return 0;
	return 1;
}

static int
read_mesh(GeoReader *r)
{
	unsigned long w;
	long n;
	int rc, hex;

	if (r->binary) {
		if (!read_word(r, &w))
			return fail(r, "File ends before the point count.");
		n = (long)w;
	} else {
		rc = read_int(r, &n, &hex);
		if (rc == 0)
			return fail(r, "File ends before the point count.");
		if (rc < 0 || hex || n < 0 || n > MAX_POINTS)
			return fail(r, "Invalid point count.");
	}
	r->npts = (int)n;

	r->obj->begin(r->obj->data, 0);
	r->obj->numPoints(r->obj->data, r->npts);
	if (!read_points(r))
		return 0;

	for (;;) {
		if (r->aborted)
			return 0;
		rc = read_polygon(r, 0);
		if (rc < 0)
			break;
		if (rc == 0)
			return 0;
	}

	r->obj->done(r->obj->data);
	return 1;
}

/* ----------------------------------------------------------------
 * Activation
 * ---------------------------------------------------------------- */

static unsigned long
file_blocks(BPTR fh)
{
	long size;

	if (Seek(fh, 0, OFFSET_END) < 0)
		return 0;
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size < 0)
		return 0;
	return (unsigned long)size / READ_BUF_SIZE + 1;
}

XCALL_(int)
Activate(
	long         version,
	GlobalFunc  *global,
	void        *local,
	void        *serverData)
{
	ObjectImport  *obj = (ObjectImport *)local;
	GeoReader     *r;
	BPTR           fh;
	unsigned long  blocks;
	int            ok;

	XCALL_INIT;
	(void)global;
	(void)serverData;

	if (version < 1)
		return AFUNC_BADVERSION;
	if (!obj)
		return AFUNC_BADLOCAL;
	if (!obj->filename) {
		obj->result = OBJSTAT_BADFILE;
		return AFUNC_OK;
	}

	fh = Open((STRPTR)obj->filename, MODE_OLDFILE);
	if (!fh) {
		obj->result = OBJSTAT_BADFILE;
		return AFUNC_OK;
	}

	r = (GeoReader *)plugin_alloc(sizeof(GeoReader));
	if (!r) {
		Close(fh);
		obj->result = OBJSTAT_FAILED;
		if (obj->failedBuf && obj->failedLen > 0) {
			strncpy(obj->failedBuf, "Out of memory.", obj->failedLen - 1);
			obj->failedBuf[obj->failedLen - 1] = '\0';
		}
		return AFUNC_OK;
	}
	r->obj = obj;
	r->fh = fh;

	blocks = file_blocks(fh);
	if (next_byte(r) != '3' || next_byte(r) != 'D') {
		obj->result = OBJSTAT_NOREC;
	} else {
		int c3 = next_byte(r), c4 = next_byte(r);

		if (c3 == 'G' && c4 == '1' && r->pos < r->len &&
		    is_space(r->buf[r->pos]))
			r->binary = 0;
		else if (c3 == 'B' && c4 == '1')
			r->binary = 1;
		else
			c3 = -1;

		if (c3 < 0) {
			obj->result = OBJSTAT_NOREC;
		} else {
			if (obj->monitor) {
				MON_INIT(obj->monitor, blocks);
				r->monitoring = 1;
			}
			ok = read_mesh(r);
			if (r->monitoring)
				MON_DONE(obj->monitor);

			if (ok) {
				obj->result = OBJSTAT_OK;
			} else if (r->aborted) {
				obj->result = OBJSTAT_ABORTED;
			} else {
				obj->result = OBJSTAT_FAILED;
				if (obj->failedBuf && obj->failedLen > 0) {
					strncpy(obj->failedBuf, r->error ? r->error :
						"Read error.", obj->failedLen - 1);
					obj->failedBuf[obj->failedLen - 1] = '\0';
				}
			}
		}
	}

	Close(fh);
	plugin_free(r);
	return AFUNC_OK;
}

/*
 * Modeler offers a file to the object loaders in name order and stops at
 * the first one that does not answer NOREC. LightwaveRTG's
 * Translator3D-Import fails on .geo files, so the name must sort before it.
 */
ServerRecord ServerDesc[] = {
	{ "ObjectLoader", "GEO(.geo)", (ActivateFunc *)Activate },
	{ 0 }
};
