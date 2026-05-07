/*
 * OBJMESHSWAP.C -- Layout Surface-Preserving Object Replacement Plugin
 * Copyright (c) 2026 Dimitris Panokostas
 *
 * Automatically swaps objects based on frame number derived from
 * filename suffixes. Given a base object "Ship.lwo" (or "Ship_0"),
 * the plugin scans for Ship_010.lwo, Ship_100.lwo etc. and replaces
 * the object at the corresponding frames.
 *
 * Frame matching: exact frame match wins; otherwise the most recent
 * replacement file before the current frame is used. Before the
 * first numbered file, the original object is kept.
 *
 * Replacement files are copied next to the source object with their top-level
 * SURF chunks replaced by the SURF chunks from the base object. The SRFS
 * surface-name list remains, so polygons keep their assignments, while each
 * replacement reload carries the same surface parameters as the base object.
 *
 * NOTE: Uses AllocMem/FreeMem instead of malloc/free because
 * -nostartfiles skips libnix heap initialization.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <exec/memory.h>

extern struct DosLibrary *DOSBase;
extern struct ExecBase   *SysBase;

/* ----------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------- */

#define MAX_PATH     256
#define MAX_NAME     108
#define MAX_ENTRIES  4096
#define IO_BUF_SIZE  1024

static unsigned char ioBuf[IO_BUF_SIZE];

/* ----------------------------------------------------------------
 * Custom case-insensitive compare (avoids libnix strncasecmp
 * which may need uninitialized ctype tables)
 * ---------------------------------------------------------------- */

static int
ci_char(int c)
{
	if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
	return c;
}

static int
ci_strncmp(const char *a, const char *b, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		int ca = ci_char((unsigned char)a[i]);
		int cb = ci_char((unsigned char)b[i]);
		if (ca != cb) return ca - cb;
		if (ca == 0) return 0;
	}
	return 0;
}

static int
ci_streq(const char *a, const char *b)
{
	int len;

	if (!a || !b)
		return 0;

	len = strlen(a);
	if ((int)strlen(b) != len)
		return 0;

	return ci_strncmp(a, b, len + 1) == 0;
}

/* ----------------------------------------------------------------
 * Memory helpers (replace malloc/free since libnix heap is not init'd)
 * ---------------------------------------------------------------- */

static void *
plugin_alloc(unsigned long size)
{
	unsigned long *p;
	/* Store size before the returned pointer for FreeMem */
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
 * Simple integer-to-string (avoids sprintf / libnix stdio)
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
str_copy_bounded(char *dst, int dstMax, const char *src)
{
	if (!dst || dstMax < 1)
		return;

	dst[0] = '\0';
	if (!src)
		return;

	strncpy(dst, src, dstMax - 1);
	dst[dstMax - 1] = '\0';
}

static void
str_append_bounded(char *dst, int dstMax, const char *src)
{
	int len;

	if (!dst || !src || dstMax < 1)
		return;

	len = strlen(dst);
	if (len >= dstMax - 1)
		return;

	strncat(dst, src, dstMax - 1 - len);
	dst[dstMax - 1] = '\0';
}

static void
u32_to_hex(unsigned long val, char *buf)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;

	for (i = 7; i >= 0; i--) {
		buf[i] = hex[val & 15u];
		val >>= 4;
	}
	buf[8] = '\0';
}

/* ----------------------------------------------------------------
 * IFF/LWO helpers
 * ---------------------------------------------------------------- */

static int
id_is(const unsigned char *id, const char *tag)
{
	return id[0] == (unsigned char)tag[0]
	    && id[1] == (unsigned char)tag[1]
	    && id[2] == (unsigned char)tag[2]
	    && id[3] == (unsigned char)tag[3];
}

static unsigned long
hash_update(unsigned long h, const char *s)
{
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619UL;
	}

	return h;
}

static unsigned long
hash_string_pair(const char *a, const char *b)
{
	unsigned long h = 2166136261UL;

	h = hash_update(h, a);
	h ^= 255u;
	h *= 16777619UL;
	return hash_update(h, b);
}

static int
read_exact(BPTR fh, void *buf, long len)
{
	return Read(fh, buf, len) == len;
}

static int
write_exact(BPTR fh, const void *buf, long len)
{
	return Write(fh, (APTR)buf, len) == len;
}

static int
read_u32be_file(BPTR fh, unsigned long *value)
{
	unsigned char b[4];

	if (!read_exact(fh, b, 4))
		return 0;

	*value = ((unsigned long)b[0] << 24)
	       | ((unsigned long)b[1] << 16)
	       | ((unsigned long)b[2] << 8)
	       | (unsigned long)b[3];

	return 1;
}

static int
write_u32be_file(BPTR fh, unsigned long value)
{
	unsigned char b[4];

	b[0] = (unsigned char)((value >> 24) & 255u);
	b[1] = (unsigned char)((value >> 16) & 255u);
	b[2] = (unsigned char)((value >> 8) & 255u);
	b[3] = (unsigned char)(value & 255u);

	return write_exact(fh, b, 4);
}

static int
copy_bytes(BPTR in, BPTR out, unsigned long count)
{
	while (count > 0) {
		long chunk = (count > IO_BUF_SIZE)
		           ? IO_BUF_SIZE : (long)count;

		if (!read_exact(in, ioBuf, chunk))
			return 0;
		if (!write_exact(out, ioBuf, chunk))
			return 0;

		count -= (unsigned long)chunk;
	}

	return 1;
}

static int
skip_bytes(BPTR in, unsigned long count)
{
	while (count > 0) {
		long chunk = (count > IO_BUF_SIZE)
		           ? IO_BUF_SIZE : (long)count;

		if (!read_exact(in, ioBuf, chunk))
			return 0;

		count -= (unsigned long)chunk;
	}

	return 1;
}

static int
file_exists(const char *path)
{
	BPTR lock;

	lock = Lock((STRPTR)path, ACCESS_READ);
	if (!lock)
		return 0;

	UnLock(lock);
	return 1;
}

static int
copy_lwo_chunks(BPTR in, BPTR out, unsigned long formSize,
                int copySurf, unsigned long *outSize,
                int *surfCount)
{
	unsigned char id[4];
	unsigned long remaining, len, payload;

	if (formSize < 4)
		return 0;

	remaining = formSize - 4;
	while (remaining >= 8) {
		if (!read_exact(in, id, 4))
			return 0;
		if (!read_u32be_file(in, &len))
			return 0;

		remaining -= 8;
		payload = len + (len & 1u);
		if (payload > remaining)
			return 0;

		if (id_is(id, "SURF") == copySurf) {
			if (!write_exact(out, id, 4))
				return 0;
			if (!write_u32be_file(out, len))
				return 0;
			if (!copy_bytes(in, out, payload))
				return 0;
			*outSize += 8 + payload;
			if (copySurf)
				(*surfCount)++;
		} else {
			if (!skip_bytes(in, payload))
				return 0;
		}

		remaining -= payload;
	}

	if (remaining != 0)
		return 0;

	return 1;
}

static int
open_lwo(BPTR *fh, const char *path, unsigned long *formSize,
         unsigned char *formType)
{
	unsigned char id[4];

	*fh = Open((STRPTR)path, MODE_OLDFILE);
	if (!*fh)
		return 0;

	if (!read_exact(*fh, id, 4) || !id_is(id, "FORM"))
		goto done;
	if (!read_u32be_file(*fh, formSize))
		goto done;
	if (!read_exact(*fh, formType, 4))
		goto done;

	if (!id_is(formType, "LWOB") && !id_is(formType, "LWLO"))
		goto done;
	if (*formSize < 4)
		goto done;

	return 1;

done:
	Close(*fh);
	*fh = 0;
	return 0;
}

static int
make_surface_preserved_copy(const char *src, const char *surfaceSrc,
                            const char *dest)
{
	BPTR          in = 0, surfIn = 0, out = 0;
	unsigned char formType[4], surfaceFormType[4];
	unsigned long formSize, surfaceFormSize, outSize;
	int           ok = 0, surfCount = 0;

	if (!DOSBase)
		return 0;

	if (!open_lwo(&in, src, &formSize, formType))
		return 0;

	out = Open((STRPTR)dest, MODE_NEWFILE);
	if (!out)
		goto done;

	if (!write_exact(out, "FORM", 4))
		goto done;
	if (!write_u32be_file(out, 0))
		goto done;
	if (!write_exact(out, formType, 4))
		goto done;

	outSize = 4;
	if (!copy_lwo_chunks(in, out, formSize, 0,
	                     &outSize, &surfCount))
		goto done;
	Close(in);
	in = 0;

	if (!open_lwo(&surfIn, surfaceSrc, &surfaceFormSize,
	              surfaceFormType))
		goto done;
	if (!copy_lwo_chunks(surfIn, out, surfaceFormSize, 1,
	                     &outSize, &surfCount))
		goto done;

	if (surfCount == 0) {
		ok = 2;
		goto done;
	}

	if (Seek(out, 4, OFFSET_BEGINNING) == -1)
		goto done;
	if (!write_u32be_file(out, outSize))
		goto done;

	ok = 1;

done:
	if (out)
		Close(out);
	if (surfIn)
		Close(surfIn);
	if (in)
		Close(in);

	if (!ok)
		DeleteFile((STRPTR)dest);

	return ok;
}

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

typedef struct {
	int  frame;
	char filename[MAX_PATH];
} FrameEntry;

typedef struct {
	char        basePath[MAX_PATH];
	char        baseDir[MAX_PATH];
	char        baseName[MAX_NAME];
	char        origPath[MAX_PATH];  /* base object file (no _N suffix) */
	char        cacheSrc[MAX_PATH];
	char        cachePath[MAX_PATH];
	char        cacheTempPath[MAX_PATH];
	char        loadPath[MAX_PATH];
	char        scanName[MAX_NAME];
	char        messageBuf[128];
	char        numberBuf[12];
	FrameEntry *entries;
	int         numEntries;
	int         capacity;
	int         scanned;
} ObjMeshSwapInst;

static int
make_cache_path(ObjMeshSwapInst *inst, const char *src, char *dest, int destMax)
{
	char srcHex[9];
	int  dirLen, nameLen;

	u32_to_hex(hash_string_pair(src, inst->basePath), srcHex);

	dirLen = strlen(inst->baseDir);
	nameLen = 17 + 8 + 4; /* "ObjMeshSwapCache-" + hash + ".lwo" */
	if (dirLen + nameLen >= destMax) {
		dest[0] = '\0';
		return 0;
	}

	strcpy(dest, inst->baseDir);
	strcat(dest, "ObjMeshSwapCache-");
	strcat(dest, srcHex);
	strcat(dest, ".lwo");

	return 1;
}

static void
make_cache_temp_path(const char *cachePath, char *tempPath)
{
	strcpy(tempPath, cachePath);
	tempPath[strlen(tempPath) - 3] = 't';
	tempPath[strlen(tempPath) - 2] = 'm';
	tempPath[strlen(tempPath) - 1] = 'p';
}

static const char *
surface_preserved_filename(ObjMeshSwapInst *inst, const char *src)
{
	int made;

	if (inst->cacheSrc[0] && strcmp(inst->cacheSrc, src) == 0)
		return inst->cachePath[0] ? inst->cachePath : src;

	if (!make_cache_path(inst, src, inst->cachePath, MAX_PATH))
		return src;

	make_cache_temp_path(inst->cachePath, inst->cacheTempPath);
	made = make_surface_preserved_copy(src, inst->basePath,
	                                   inst->cacheTempPath);

	strncpy(inst->cacheSrc, src, MAX_PATH - 1);
	inst->cacheSrc[MAX_PATH - 1] = '\0';

	if (made == 1) {
		if (!Rename((STRPTR)inst->cacheTempPath,
		            (STRPTR)inst->cachePath)) {
			if (file_exists(inst->cachePath)) {
				if (DeleteFile((STRPTR)inst->cachePath) &&
				    Rename((STRPTR)inst->cacheTempPath,
				           (STRPTR)inst->cachePath))
					return inst->cachePath;

				DeleteFile((STRPTR)inst->cacheTempPath);
				if (file_exists(inst->cachePath))
					return inst->cachePath;
			}

			DeleteFile((STRPTR)inst->cacheTempPath);
			inst->cachePath[0] = '\0';
			return src;
		}
		return inst->cachePath;
	}

	if (made == 2)
		DeleteFile((STRPTR)inst->cacheTempPath);
	inst->cachePath[0] = '\0';
	return src;
}

/* ----------------------------------------------------------------
 * Globals (set once in Activate)
 * ---------------------------------------------------------------- */

static MessageFuncs *msg;

/* ----------------------------------------------------------------
 * Path helpers
 * ---------------------------------------------------------------- */

static void
split_path(const char *fullpath, char *dir, int dirmax,
           char *file, int filemax)
{
	const char *p, *lastSep = 0;
	int         len;

	for (p = fullpath; *p; p++) {
		if (*p == '/' || *p == ':')
			lastSep = p;
	}

	if (lastSep) {
		len = (int)(lastSep - fullpath + 1);
		if (len >= dirmax) len = dirmax - 1;
		memcpy(dir, fullpath, len);
		dir[len] = '\0';
		strncpy(file, lastSep + 1, filemax - 1);
		file[filemax - 1] = '\0';
	} else {
		dir[0] = '\0';
		strncpy(file, fullpath, filemax - 1);
		file[filemax - 1] = '\0';
	}
}

static void
extract_base_name(const char *filename, char *base, int basemax)
{
	/* Use base buffer as scratch space to avoid stack allocation */
	const char *dot = 0, *p, *lastUnderscore = 0;
	int         len;

	for (p = filename; *p; p++) {
		if (*p == '.') dot = p;
	}
	if (dot) {
		len = (int)(dot - filename);
		if (len >= basemax) len = basemax - 1;
		memcpy(base, filename, len);
		base[len] = '\0';
	} else {
		strncpy(base, filename, basemax - 1);
		base[basemax - 1] = '\0';
	}

	for (p = base; *p; p++) {
		if (*p == '_') lastUnderscore = p;
	}

	if (lastUnderscore && lastUnderscore[1]) {
		int allDigits = 1;
		for (p = lastUnderscore + 1; *p; p++) {
			if (*p < '0' || *p > '9') {
				allDigits = 0;
				break;
			}
		}
		if (allDigits) {
			/* Truncate at the underscore */
			*((char *)lastUnderscore) = '\0';
			return;
		}
	}
	/* base already contains the right value */
}

static int
parse_frame_number(const char *filename, const char *baseName)
{
	int         baseLen = strlen(baseName);
	const char *p;
	int         frame;

	if (ci_strncmp(filename, baseName, baseLen) != 0)
		return -1;

	if (filename[baseLen] != '_')
		return -1;

	p = &filename[baseLen + 1];
	if (*p < '0' || *p > '9')
		return -1;

	frame = 0;
	while (*p >= '0' && *p <= '9') {
		frame = frame * 10 + (*p - '0');
		p++;
	}

	if (*p != '\0' && *p != '.')
		return -1;

	return frame;
}

/* ----------------------------------------------------------------
 * Shell sort using byte-level swap (avoids putting a full FrameEntry
 * on the stack and is much faster than bubble sort for large lists)
 * ---------------------------------------------------------------- */

static void
swap_entries(FrameEntry *a, FrameEntry *b)
{
	char *pa = (char *)a, *pb = (char *)b;
	int   i;
	char  t;
	for (i = 0; i < (int)sizeof(FrameEntry); i++) {
		t = pa[i]; pa[i] = pb[i]; pb[i] = t;
	}
}

static void
sort_entries(FrameEntry *entries, int n)
{
	int gap, i, j;

	for (gap = n / 2; gap > 0; gap /= 2) {
		for (i = gap; i < n; i++) {
			for (j = i; j >= gap; j -= gap) {
				if (entries[j - gap].frame <= entries[j].frame)
					break;
				swap_entries(&entries[j - gap], &entries[j]);
			}
		}
	}
}

static int
find_best_entry(FrameEntry *entries, int n, int frame)
{
	int lo = 0, hi = n - 1, best = -1;

	while (lo <= hi) {
		int mid = lo + ((hi - lo) / 2);

		if (entries[mid].frame <= frame) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return best;
}

/* ----------------------------------------------------------------
 * Directory scanning
 * ---------------------------------------------------------------- */

static void
free_entries(ObjMeshSwapInst *inst)
{
	if (inst->entries) {
		plugin_free(inst->entries);
		inst->entries = 0;
	}
	inst->numEntries = 0;
	inst->capacity = 0;
}

static void
do_scan(ObjMeshSwapInst *inst)
{
	BPTR                 lock;
	struct FileInfoBlock *fib;
	int                   frame, fileCount;
	char                 *dest;
	int                   dirLen, nameLen;

	free_entries(inst);

	if (!DOSBase)
		return;

	if (inst->baseDir[0])
		lock = Lock((STRPTR)inst->baseDir, ACCESS_READ);
	else
		lock = Lock((STRPTR)"", ACCESS_READ);

	if (!lock)
		return;

	fib = (struct FileInfoBlock *)
	       AllocMem(sizeof(struct FileInfoBlock), MEMF_PUBLIC);
	if (!fib) {
		UnLock(lock);
		return;
	}

	if (!Examine(lock, fib)) {
		FreeMem(fib, sizeof(struct FileInfoBlock));
		UnLock(lock);
		return;
	}

	inst->capacity = 64;
	inst->entries = (FrameEntry *)
		plugin_alloc(sizeof(FrameEntry) * inst->capacity);
	if (!inst->entries) {
		FreeMem(fib, sizeof(struct FileInfoBlock));
		UnLock(lock);
		return;
	}

	dirLen = strlen(inst->baseDir);
	nameLen = strlen(inst->baseName);
	fileCount = 0;
	inst->numEntries = 0;
	inst->origPath[0] = '\0';

	while (ExNext(lock, fib)) {
		const char *fname = (const char *)fib->fib_FileName;

		if (fib->fib_DirEntryType > 0)
			continue;

		if (++fileCount > 10000)
			break;

		/* Do NOT process .info files!
		 * Certain AmigaOS/Workbench flavors like to
		 * make these for every file whenever the FS
		 * is accessed. They will be read by the obj
		 * scanner, which will crash the plugin.
		*/

		{
			char *ext = strrchr(fname, '.');
			if (ext && ci_strncmp(ext, ".info", 6) == 0)
				continue;
		}

		/*
		 * Check if this file IS the base object (matches
		 * baseName exactly, with optional extension, no _N).
		 * e.g., "digit" or "digit.lwo" when baseName is "digit"
		 */
		if (!inst->origPath[0] &&
		    ci_strncmp(fname, inst->baseName, nameLen) == 0 &&
		    (fname[nameLen] == '\0' || fname[nameLen] == '.'))
		{
			dest = inst->origPath;
			if (dirLen > 0) {
				strncpy(dest, inst->baseDir, MAX_PATH - 1);
				dest[MAX_PATH - 1] = '\0';
				strncat(dest, fname,
				        MAX_PATH - 1 - dirLen);
			} else {
				strncpy(dest, fname, MAX_PATH - 1);
				dest[MAX_PATH - 1] = '\0';
			}
		}

		frame = parse_frame_number(
			(const char *)fib->fib_FileName,
			inst->baseName);
		if (frame < 0)
			continue;

		/* Grow array if needed */
		if (inst->numEntries >= inst->capacity) {
			FrameEntry *newArr;
			int         newCap = inst->capacity * 2;
			int         i;

			if (newCap > MAX_ENTRIES)
				break;

			newArr = (FrameEntry *)
				plugin_alloc(sizeof(FrameEntry) * newCap);
			if (!newArr)
				break;

			for (i = 0; i < inst->numEntries; i++)
				newArr[i] = inst->entries[i];

			plugin_free(inst->entries);
			inst->entries = newArr;
			inst->capacity = newCap;
		}

		/* Build full path directly into entry (no stack buffer) */
		dest = inst->entries[inst->numEntries].filename;
		if (dirLen > 0) {
			strncpy(dest, inst->baseDir, MAX_PATH - 1);
			dest[MAX_PATH - 1] = '\0';
			strncat(dest,
			        (const char *)fib->fib_FileName,
			        MAX_PATH - 1 - dirLen);
		} else {
			strncpy(dest,
			        (const char *)fib->fib_FileName,
			        MAX_PATH - 1);
			dest[MAX_PATH - 1] = '\0';
		}

		inst->entries[inst->numEntries].frame = frame;
		inst->numEntries++;
	}

	FreeMem(fib, sizeof(struct FileInfoBlock));
	UnLock(lock);

	if (inst->numEntries > 0)
		sort_entries(inst->entries, inst->numEntries);

	inst->scanned = 1;
}

static void
scan_from_path(ObjMeshSwapInst *inst, const char *objPath)
{
	strncpy(inst->basePath, objPath, MAX_PATH - 1);
	inst->basePath[MAX_PATH - 1] = '\0';
	inst->cacheSrc[0] = '\0';
	inst->cachePath[0] = '\0';
	inst->cacheTempPath[0] = '\0';

	split_path(objPath, inst->baseDir, MAX_PATH,
	           inst->scanName, MAX_NAME);
	extract_base_name(inst->scanName, inst->baseName, MAX_NAME);

	do_scan(inst);
}

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err)
{
	ObjMeshSwapInst *inst;

	XCALL_INIT;

	inst = (ObjMeshSwapInst *)plugin_alloc(sizeof(ObjMeshSwapInst));
	if (!inst)
		return 0;

	/* plugin_alloc zeroes memory via MEMF_CLEAR */
	return inst;
}

XCALL_(static void)
Destroy(ObjMeshSwapInst *inst)
{
	XCALL_INIT;
	if (!inst) return;
	free_entries(inst);
	plugin_free(inst);
}

XCALL_(static LWError)
Copy(ObjMeshSwapInst *from, ObjMeshSwapInst *to)
{
	int i;

	XCALL_INIT;

	strcpy(to->basePath, from->basePath);
	strcpy(to->baseDir, from->baseDir);
	strcpy(to->baseName, from->baseName);
	strcpy(to->origPath, from->origPath);
	to->cacheSrc[0] = '\0';
	to->cachePath[0] = '\0';
	to->cacheTempPath[0] = '\0';
	to->scanned = from->scanned;

	free_entries(to);
	if (from->numEntries > 0 && from->entries) {
		to->entries = (FrameEntry *)
			plugin_alloc(sizeof(FrameEntry) * from->numEntries);
		if (to->entries) {
			to->numEntries = from->numEntries;
			to->capacity = from->numEntries;
			for (i = 0; i < from->numEntries; i++)
				to->entries[i] = from->entries[i];
		}
	}

	return 0;
}

XCALL_(static LWError)
Load(ObjMeshSwapInst *inst, const LWLoadState *ls)
{
	XCALL_INIT;

	if (spi_read_string_record(ls, inst->loadPath,
	                           sizeof(inst->loadPath)) > 0
	    && inst->loadPath[0])
		scan_from_path(inst, inst->loadPath);

	return 0;
}

XCALL_(static LWError)
Save(ObjMeshSwapInst *inst, const LWSaveState *ss)
{
	XCALL_INIT;

	if (inst->basePath[0])
		spi_write_string_record(ss, inst->basePath);

	return 0;
}

XCALL_(static void)
Evaluate(ObjMeshSwapInst *inst, ObjReplacementAccess *oa)
{
	int bestIdx;
	const char *target;

	XCALL_INIT;

	if (!inst->scanned && oa->curFilename && oa->curFilename[0])
		scan_from_path(inst, oa->curFilename);

	if (inst->numEntries == 0)
		return;

	bestIdx = find_best_entry(inst->entries, inst->numEntries,
	                          oa->newFrame);

	if (bestIdx >= 0) {
		if (ci_streq(inst->entries[bestIdx].filename, inst->basePath))
			target = inst->basePath;
		else
			target = surface_preserved_filename(
				inst, inst->entries[bestIdx].filename);
	} else if (inst->origPath[0]) {
		if (ci_streq(inst->origPath, inst->basePath))
			target = inst->basePath;
		else
			target = surface_preserved_filename(inst,
			                                   inst->origPath);
	} else
		target = inst->basePath;

	if (!ci_streq(oa->curFilename, target))
		oa->newFilename = target;
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static ObjMeshSwapInst *ui_inst;
static char           ui_buf[80];

static int
ui_entryCount(void *data)
{
	(void)data;
	return ui_inst ? ui_inst->numEntries : 0;
}

static char *
ui_entryName(void *data, int idx)
{
	char numBuf[12];
	int  len;

	(void)data;
	if (!ui_inst || idx < 0 || idx >= ui_inst->numEntries)
		return "";

	/* Build "Frame NNN: filename" without sprintf */
	strcpy(ui_buf, "Frame ");
	int_to_str(ui_inst->entries[idx].frame, numBuf, sizeof(numBuf));
	strcat(ui_buf, numBuf);
	strcat(ui_buf, ": ");
	len = strlen(ui_buf);
	strncpy(ui_buf + len,
	        ui_inst->entries[idx].filename,
	        sizeof(ui_buf) - len - 1);
	ui_buf[sizeof(ui_buf) - 1] = '\0';
	return ui_buf;
}

XCALL_(static int)
Interface(
	long           version,
	GlobalFunc    *global,
	ObjMeshSwapInst *inst,
	void          *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl    *listCtl, *infoCtl;
	const char   *infoLines[3];
	char         *infoBuf = inst->messageBuf;
	char         *numBuf = inst->numberBuf;

	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;

	if (!inst->scanned && inst->basePath[0])
		scan_from_path(inst, inst->basePath);

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);

	panl = (LWPanelFuncs *)
		(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);

	if (panl) {
		static LWPanControlDesc desc;
		static LWValue ival = {LWT_INTEGER};
		(void)ival;

		pan = PAN_CREATE(panl, "ObjMeshSwap v" PLUGIN_VERSION " (c) D. Panokostas");
		if (!pan)
			goto fallback;

		if (inst->baseName[0]) {
			str_copy_bounded(infoBuf, sizeof(inst->messageBuf),
			                 "Base Object: ");
			str_append_bounded(infoBuf, sizeof(inst->messageBuf),
			                   inst->baseName);
			str_append_bounded(infoBuf, sizeof(inst->messageBuf),
			                   "  |  Found: ");
			int_to_str(inst->numEntries, numBuf,
			           sizeof(inst->numberBuf));
			str_append_bounded(infoBuf, sizeof(inst->messageBuf),
			                   numBuf);
			str_append_bounded(infoBuf, sizeof(inst->messageBuf),
			                   " files");
		} else {
			str_copy_bounded(infoBuf, sizeof(inst->messageBuf),
			                 "No object scanned yet. "
			                 "Preview a frame first.");
		}

		infoLines[0] = infoBuf;
		infoLines[1] = "Surface data: preserved from base object";
		infoLines[2] = 0;
		infoCtl = TEXT_CTL(panl, pan, "", infoLines);
		(void)infoCtl;

		if (inst->numEntries > 0) {
			ui_inst = inst;
			listCtl = LISTBOX_CTL(panl, pan, "Replacements",
			                      400,
			                      inst->numEntries < 15
			                        ? inst->numEntries : 15,
			                      ui_entryName, ui_entryCount);
			(void)listCtl;
		}

		(*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL);
		PAN_KILL(panl, pan);
		ui_inst = 0;
		return AFUNC_OK;
	}

fallback:
	if (!msg)
		return AFUNC_BADGLOBAL;

	if (inst->numEntries > 0) {
		str_copy_bounded(infoBuf, sizeof(inst->messageBuf), "Found ");
		int_to_str(inst->numEntries, numBuf,
		           sizeof(inst->numberBuf));
		str_append_bounded(infoBuf, sizeof(inst->messageBuf), numBuf);
		str_append_bounded(infoBuf, sizeof(inst->messageBuf),
		                   " replacement(s) for ");
		str_append_bounded(infoBuf, sizeof(inst->messageBuf),
		                   inst->baseName);
		(*msg->info)("ObjMeshSwap", infoBuf);
	} else if (inst->basePath[0]) {
		str_copy_bounded(infoBuf, sizeof(inst->messageBuf),
		                 "No replacements for ");
		str_append_bounded(infoBuf, sizeof(inst->messageBuf),
		                   inst->baseName);
		(*msg->info)("ObjMeshSwap", infoBuf);
	} else {
		(*msg->info)("ObjMeshSwap",
		             "Preview a frame to detect files.");
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
	ObjReplacementHandler_V1 *h = (ObjReplacementHandler_V1 *)local;

	XCALL_INIT;

	if (version < 1)
		return AFUNC_BADVERSION;

	h->create   = (void *)Create;
	h->destroy  = (void *)Destroy;
	h->load     = (void *)Load;
	h->save     = (void *)Save;
	h->copy     = (void *)Copy;
	h->evaluate = (void *)Evaluate;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!msg)
		return AFUNC_BADGLOBAL;

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Server description
 * ---------------------------------------------------------------- */

ServerRecord ServerDesc[] = {
	{ "ObjReplacementHandler",   "ObjMeshSwap",
	  (ActivateFunc *)Activate },
	{ "ObjReplacementInterface", "ObjMeshSwap",
	  (ActivateFunc *)Interface },
	{ 0 }
};
