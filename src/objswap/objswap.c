/*
 * OBJSWAP.C -- Layout Object Replacement Plugin
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
	FrameEntry *entries;
	int         numEntries;
	int         capacity;
	int         scanned;
} ObjSwapInst;

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
free_entries(ObjSwapInst *inst)
{
	if (inst->entries) {
		plugin_free(inst->entries);
		inst->entries = 0;
	}
	inst->numEntries = 0;
	inst->capacity = 0;
}

static void
do_scan(ObjSwapInst *inst)
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

		/* Do NOT process .info files!
		 * Certain AmigaOS/Workbench flavors like to
		 * make these for every file whenever the FS
		 * is accessed. They will be read by the obj
		 * scanner, which will crash the plugin.
		*/

		char * ext = strrchr(fname, '.');
		if(strcmp(ext, ".info") == 0)
			continue;

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
scan_from_path(ObjSwapInst *inst, const char *objPath)
{
	char filename[MAX_NAME];

	strncpy(inst->basePath, objPath, MAX_PATH - 1);
	inst->basePath[MAX_PATH - 1] = '\0';

	split_path(objPath, inst->baseDir, MAX_PATH,
	           filename, MAX_NAME);
	extract_base_name(filename, inst->baseName, MAX_NAME);

	do_scan(inst);
}

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err)
{
	ObjSwapInst *inst;

	XCALL_INIT;

	inst = (ObjSwapInst *)plugin_alloc(sizeof(ObjSwapInst));
	if (!inst)
		return 0;

	/* plugin_alloc zeroes memory via MEMF_CLEAR */
	return inst;
}

XCALL_(static void)
Destroy(ObjSwapInst *inst)
{
	XCALL_INIT;
	if (!inst) return;
	free_entries(inst);
	plugin_free(inst);
}

XCALL_(static LWError)
Copy(ObjSwapInst *from, ObjSwapInst *to)
{
	int i;

	XCALL_INIT;

	strcpy(to->basePath, from->basePath);
	strcpy(to->baseDir, from->baseDir);
	strcpy(to->baseName, from->baseName);
	strcpy(to->origPath, from->origPath);
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
Load(ObjSwapInst *inst, const LWLoadState *ls)
{
	char buf[MAX_PATH];

	XCALL_INIT;

	if (spi_read_string_record(ls, buf, sizeof(buf)) > 0 && buf[0])
		scan_from_path(inst, buf);

	return 0;
}

XCALL_(static LWError)
Save(ObjSwapInst *inst, const LWSaveState *ss)
{
	XCALL_INIT;

	if (inst->basePath[0])
		spi_write_string_record(ss, inst->basePath);

	return 0;
}

XCALL_(static void)
Evaluate(ObjSwapInst *inst, ObjReplacementAccess *oa)
{
	int bestIdx;

	XCALL_INIT;

	if (!inst->scanned && oa->curFilename && oa->curFilename[0])
		scan_from_path(inst, oa->curFilename);

	if (inst->numEntries == 0)
		return;

	bestIdx = find_best_entry(inst->entries, inst->numEntries,
	                          oa->newFrame);

	if (bestIdx >= 0)
		oa->newFilename = inst->entries[bestIdx].filename;
	else if (inst->origPath[0])
		oa->newFilename = inst->origPath;
	else
		oa->newFilename = inst->basePath;
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static ObjSwapInst *ui_inst;
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
	ObjSwapInst *inst,
	void          *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl    *listCtl, *infoCtl;
	char          infoBuf[128];
	char          numBuf[12];
	const char   *infoLines[3];

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

		pan = PAN_CREATE(panl, "ObjSwap v" PLUGIN_VERSION " (c) D. Panokostas");
		if (!pan)
			goto fallback;

		if (inst->baseName[0]) {
			strcpy(infoBuf, "Base Object: ");
			strcat(infoBuf, inst->baseName);
			strcat(infoBuf, "  |  Found: ");
			int_to_str(inst->numEntries, numBuf,
			           sizeof(numBuf));
			strcat(infoBuf, numBuf);
			strcat(infoBuf, " files");
		} else {
			strcpy(infoBuf,
			       "No object scanned yet. "
			       "Preview a frame first.");
		}

		infoLines[0] = infoBuf;
		infoLines[1] = 0;
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
		strcpy(infoBuf, "Found ");
		int_to_str(inst->numEntries, numBuf, sizeof(numBuf));
		strcat(infoBuf, numBuf);
		strcat(infoBuf, " replacement(s) for ");
		strcat(infoBuf, inst->baseName);
		(*msg->info)("ObjSwap", infoBuf);
	} else if (inst->basePath[0]) {
		strcpy(infoBuf, "No replacements for ");
		strcat(infoBuf, inst->baseName);
		(*msg->info)("ObjSwap", infoBuf);
	} else {
		(*msg->info)("ObjSwap",
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
	{ "ObjReplacementHandler",   "ObjSwap",
	  (ActivateFunc *)Activate },
	{ "ObjReplacementInterface", "ObjSwap",
	  (ActivateFunc *)Interface },
	{ 0 }
};
