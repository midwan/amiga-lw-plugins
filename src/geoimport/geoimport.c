/*
 * GEOIMPORT.C -- VideoScape / Aegis Modeler .geo Object Loader
 * Copyright (c) 2026 D. Panokostas
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Imports VideoScape 3D geometry files (.geo) into LightWave 3D 5.x.
 * Supports both ASCII (3DG1) and binary (3DB1) format variants with
 * per-polygon color-to-surface mapping and Z-axis negation for
 * coordinate system conversion. No external library dependencies.
 *
 * Uses AllocMem/FreeMem and AmigaOS DOS — no libnix runtime.
 */

#if defined(__clang__) || defined(__INTELLISENSE__)
#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0"
#endif

typedef long BPTR;
typedef char *STRPTR;

typedef void GlobalFunc;

typedef struct st_ObjectImport {
	int               result;
	const char       *filename;
	void             *monitor;
	char             *failedBuf;
	int               failedLen;
	void             *data;
	void            (*begin) (void *, void *);
	void            (*done) (void *);
	void            (*numPoints) (void *, int total);
	void            (*points) (void *, int numPts,
				   const float *xyz);
	int             (*surfIndex) (void *, const char *name,
				      int *firstTime);
	void            (*polygon) (void *, int numPts, int surf,
				    int flags,
				    const unsigned short *);
	void            (*surfData) (void *, const char *name,
				     int size, void *data);
} ObjectImport;

typedef int ActivateFunc(long version, GlobalFunc *global, void *local, void *serverData);

typedef struct st_ServerRecord {
	const char  *serverClass;
	const char  *serverName;
	ActivateFunc *activate;
} ServerRecord;

extern BPTR  Open(const STRPTR name, long mode);
extern long  Read(BPTR fh, void *buf, long len);
extern long  Seek(BPTR fh, long offset, long mode);
extern long  Close(BPTR fh);
extern void *AllocMem(unsigned long size, unsigned long flags);
extern void  FreeMem(void *memory, unsigned long size);

#define OBJSTAT_OK       0
#define OBJSTAT_NOREC    1
#define OBJSTAT_BADFILE  2
#define OBJSTAT_ABORTED  3
#define OBJSTAT_FAILED   99
#define OBJPOLF_FACE     0
#define MODE_OLDFILE     1005
#define OFFSET_CURRENT   1
#define MEMF_PUBLIC      0x00000001UL
#define MEMF_CLEAR       0x00010000UL

#define XCALL_(t) t
#define XCALL_INIT do { } while (0)

#define AFUNC_OK          0
#define AFUNC_BADVERSION  1
#define AFUNC_BADGLOBAL   2
#define AFUNC_BADLOCAL    3

#else
#include <splug.h>
#include <lwbase.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <exec/memory.h>

extern struct DosLibrary *DOSBase;
extern struct ExecBase   *SysBase;
#endif

#include <string.h>


#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "dev"
#endif

#ifndef XCALL_
#define XCALL_(t) t
#define XCALL_INIT
#endif

/* AmigaOS version string */
static const char __attribute__((used)) verstag[] =
	"\0$VER: VideoScape_GEO_loader " PLUGIN_VERSION " (c) D. Panokostas";

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
 * Parsing helpers
 * ---------------------------------------------------------------- */

static int
is_ascii_space(int c)
{
	return (c == ' ') || (c == '\t') || (c == '\r') ||
	       (c == '\n') || (c == '\f') || (c == '\v');
}

static int
read_line(BPTR fh, char *buf, int bufsize)
{
	int pos = 0;
	unsigned char ch = 0;
	long r;

	if (!buf || bufsize <= 0) return 0;

	while (1) {
		r = Read(fh, &ch, 1);
		if (r != 1) {
			if (pos == 0)
				return -1;
			break;
		}

		if (ch == '\r') {
			if (Read(fh, &ch, 1) == 1 && ch != '\n')
				Seek(fh, -1, OFFSET_CURRENT);
			break;
		}
		if (ch == '\n')
			break;

		if (pos < bufsize - 1)
			buf[pos++] = (char)ch;
	}

	buf[pos] = '\0';
	return pos;
}

static int
str_to_int(const char *s, const char **end)
{
	long value = 0;
	int sign = 1;
	const char *p = s;

	if (!p) {
		if (end) *end = 0;
		return 0;
	}
	while (is_ascii_space((unsigned char)*p)) p++;
	if (*p == '+') {
		p++;
	} else if (*p == '-') {
		p++;
		sign = -1;
	}

	if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
		while (1) {
			if (*p >= '0' && *p <= '9')
				value = (value * 16) + (*p - '0');
			else if (*p >= 'a' && *p <= 'f')
				value = (value * 16) + (*p - 'a' + 10);
			else if (*p >= 'A' && *p <= 'F')
				value = (value * 16) + (*p - 'A' + 10);
			else
				break;
			p++;
		}
	} else {
		while (*p >= '0' && *p <= '9') {
			value = (value * 10) + (*p - '0');
			p++;
		}
	}

	if (end) *end = p;

	return (sign < 0) ? (int)(0 - value) : (int)value;
}

static float
str_to_float(const char *s, const char **end)
{
	float value = 0.0f;
	float frac = 0.1f;
	int sign = 1;
	const char *p = s;

	if (!p) {
		if (end) *end = 0;
		return 0.0f;
	}
	while (is_ascii_space((unsigned char)*p)) p++;
	if (*p == '+') {
		p++;
	} else if (*p == '-') {
		p++;
		sign = -1;
	}

	while (*p >= '0' && *p <= '9') {
		value = (value * 10.0f) + (float)(*p - '0');
		p++;
	}

	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9') {
			value += (float)(*p - '0') * frac;
			frac *= 0.1f;
			p++;
		}
	}

	if (end) *end = p;

	return (sign < 0) ? -value : value;
}

static float
ffp_to_float(unsigned long ffp)
{
	unsigned long ieee;
	unsigned long sign;
	unsigned long exp;
	unsigned long mant;
	float out = 0.0f;

	if (ffp == 0)
		return 0.0f;

	/* FFP layout: bits 31-8 = mantissa, bit 7 = sign, bits 6-0 = exponent */
	sign = (ffp >> 7) & 1;
	exp  = ffp & 0x7FUL;
	mant = (ffp >> 8) & 0x00FFFFFFUL;

	if (exp == 0 || mant == 0)
		return 0.0f;

	ieee = (sign << 31) | ((exp + 62UL) << 23) | (mant & 0x007FFFFFUL);
	memcpy(&out, &ieee, sizeof(out));
	return out;
}

static void
color_to_surface_name(int color, char *buf, int buflen)
{
	if (!buf || buflen <= 0) return;

	if (color < 0) color = -color;
	if (color > 15) color = 15;

	if (buflen < 9) {
		buf[0] = '\0';
		return;
	}

	buf[0] = 'C';
	buf[1] = 'o';
	buf[2] = 'l';
	buf[3] = 'o';
	buf[4] = 'r';
	buf[5] = '_';
	if (color < 10) {
		buf[6] = (char)('0' + color);
		buf[7] = '\0';
	} else {
		buf[6] = '1';
		buf[7] = (char)('0' + (color - 10));
		buf[8] = '\0';
	}
}

char ServerClass[] = "ObjectLoader";
char ServerName[]  = "VideoScape_GEO(.geo)";

/* ----------------------------------------------------------------
 * ASCII format parser (3DG1)
 * ---------------------------------------------------------------- */

static int
parse_3dg1(BPTR fh, ObjectImport *obj)
{
	char linebuf[56];
	char nameBuf[12];
	int len, npts, i, j, nv, color, si, ft;
	float *pts;
	unsigned short *idx;
	const char *p;

	/* Skip remaining bytes of the "3DG1" header line */
	for (;;) {
		len = read_line(fh, linebuf, sizeof(linebuf));
		if (len < 0) return OBJSTAT_BADFILE;
		if (len > 0) break;
	}
	npts = str_to_int(linebuf, 0);
	if (npts < 0) return OBJSTAT_BADFILE;

	pts = 0;
	if (npts > 0) {
		pts = (float *)plugin_alloc(
			(unsigned long)npts * 3 * sizeof(float));
		if (!pts) return OBJSTAT_BADFILE;

		for (i = 0; i < npts; i++) {
			if (read_line(fh, linebuf, sizeof(linebuf)) < 0) {
				plugin_free(pts);
				return OBJSTAT_BADFILE;
			}
			p = linebuf;
			pts[i * 3 + 0] = str_to_float(p, &p);
			pts[i * 3 + 1] = str_to_float(p, &p);
			pts[i * 3 + 2] = -str_to_float(p, &p);
		}
	}

	obj->begin(obj->data, 0);
	obj->numPoints(obj->data, npts);
	if (npts > 0)
		obj->points(obj->data, npts, pts);

	while (read_line(fh, linebuf, sizeof(linebuf)) >= 0) {
		if (linebuf[0] == '\0') continue;
		p = linebuf;
		nv = str_to_int(p, &p);
		if (nv <= 0) continue;

		idx = (unsigned short *)plugin_alloc(
			(unsigned long)nv * sizeof(unsigned short));
		if (!idx) {
			obj->done(obj->data);
			plugin_free(pts);
			return OBJSTAT_BADFILE;
		}

		for (j = 0; j < nv; j++)
			idx[j] = (unsigned short)str_to_int(p, &p);
		color = str_to_int(p, &p);

		color_to_surface_name(color, nameBuf, sizeof(nameBuf));
		si = obj->surfIndex(obj->data, nameBuf, &ft);
		obj->polygon(obj->data, nv, si, OBJPOLF_FACE, idx);
		plugin_free(idx);
	}

	obj->done(obj->data);
	plugin_free(pts);
	return OBJSTAT_OK;
}

/* ----------------------------------------------------------------
 * Binary format parser (3DB1)
 * ---------------------------------------------------------------- */

static int
parse_3db1(BPTR fh, ObjectImport *obj)
{
	unsigned char b2[2];
	unsigned char b4[4];
	char nameBuf[12];
	int npts, i, j, nv, color, si, ft;
	float *pts;
	unsigned short *idx;
	unsigned long ffp;
	float fv;

	if (Read(fh, b2, 2) != 2) return OBJSTAT_BADFILE;
	npts = (b2[0] << 8) | b2[1];

	pts = 0;
	if (npts > 0) {
		pts = (float *)plugin_alloc(
			(unsigned long)npts * 3 * sizeof(float));
		if (!pts) return OBJSTAT_BADFILE;

		for (i = 0; i < npts; i++) {
			for (j = 0; j < 3; j++) {
				if (Read(fh, b4, 4) != 4) {
					plugin_free(pts);
					return OBJSTAT_BADFILE;
				}
				ffp = ((unsigned long)b4[0] << 24) |
				      ((unsigned long)b4[1] << 16) |
				      ((unsigned long)b4[2] << 8)  |
				      (unsigned long)b4[3];
				fv = ffp_to_float(ffp);
				pts[i * 3 + j] = (j == 2) ? -fv : fv;
			}
		}
	}

	obj->begin(obj->data, 0);
	obj->numPoints(obj->data, npts);
	if (npts > 0)
		obj->points(obj->data, npts, pts);

	for (;;) {
		if (Read(fh, b2, 2) != 2) break;
		nv = (b2[0] << 8) | b2[1];
		if (nv == 0) break;

		idx = (unsigned short *)plugin_alloc(
			(unsigned long)nv * sizeof(unsigned short));
		if (!idx) {
			obj->done(obj->data);
			plugin_free(pts);
			return OBJSTAT_BADFILE;
		}

		for (j = 0; j < nv; j++) {
			if (Read(fh, b2, 2) != 2) {
				plugin_free(idx);
				obj->done(obj->data);
				plugin_free(pts);
				return OBJSTAT_BADFILE;
			}
			idx[j] = (unsigned short)((b2[0] << 8) | b2[1]);
		}

		if (Read(fh, b2, 2) != 2) {
			plugin_free(idx);
			obj->done(obj->data);
			plugin_free(pts);
			return OBJSTAT_BADFILE;
		}
		color = (int)(short)((b2[0] << 8) | b2[1]);

		color_to_surface_name(color, nameBuf, sizeof(nameBuf));
		si = obj->surfIndex(obj->data, nameBuf, &ft);
		obj->polygon(obj->data, nv, si, OBJPOLF_FACE, idx);
		plugin_free(idx);
	}

	obj->done(obj->data);
	plugin_free(pts);
	return OBJSTAT_OK;
}

/* ----------------------------------------------------------------
 * Activation
 * ---------------------------------------------------------------- */

XCALL_(int)
Activate(
	long        version,
	GlobalFunc  *global,
	void        *local,
	void        *serverData)
{
	ObjectImport *obj = (ObjectImport *)local;
	BPTR          fh = 0;
	char          sig[4];

	XCALL_INIT;
	(void)global;
	(void)serverData;

	if (version < 1)
		return AFUNC_BADVERSION;

	if (!obj || !obj->filename) {
		if (obj)
			obj->result = OBJSTAT_BADFILE;
		return AFUNC_OK;
	}

	fh = Open((STRPTR)obj->filename, MODE_OLDFILE);
	if (!fh) {
		obj->result = OBJSTAT_BADFILE;
		return AFUNC_OK;
	}

	if (Read(fh, sig, 4) != 4) {
		Close(fh);
		obj->result = OBJSTAT_BADFILE;
		return AFUNC_OK;
	}

	if (memcmp(sig, "3DG1", 4) == 0) {
		obj->result = parse_3dg1(fh, obj);
		Close(fh);
		return AFUNC_OK;
	}

	if (memcmp(sig, "3DB1", 4) == 0) {
		obj->result = parse_3db1(fh, obj);
		Close(fh);
		return AFUNC_OK;
	}

	Close(fh);
	obj->result = OBJSTAT_NOREC;
	return AFUNC_OK;
}

ServerRecord ServerDesc[] = {
	{ ServerClass, ServerName, (ActivateFunc *)Activate },
	{ 0 }
};
