/*
 * SURFLIST.C -- Surface replacement shader for LightWave 3D
 *
 * Loads a frame/path list and applies the supported material values from
 * the selected LightWave surface file.  All file access happens while the
 * list is loaded, never from the per-pixel callback.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dosextens.h>
#include <exec/memory.h>

extern struct DosLibrary *DOSBase;
extern struct ExecBase *SysBase;

#define MAX_PATH       256
#define MAX_LIST_BYTES 65536
#define MAX_SURF_BYTES 524288
#define MAX_ENTRIES    256

typedef struct {
	double color[3];
	double luminous;
	double diffuse;
	double specular;
	double mirror;
	double transparency;
	double eta;
	double roughness;
} SurfaceParams;

typedef struct {
	int frame;
	char path[MAX_PATH];
	SurfaceParams params;
} SurfaceEntry;

typedef struct {
	char listPath[MAX_PATH];
	SurfaceEntry *entries;
	int numEntries;
	int capacity;
	int errors;
	int loaded;
	int active;
} SurfListInst;

static MessageFuncs *msg;
static char uiPath[MAX_PATH];
static char uiStatus[80];
static const char *uiLines[2];


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
int_to_str(int value, char *buf, int len)
{
	char digits[12];
	int n = 0, i, negative = 0;

	if (value < 0) { negative = 1; value = -value; }
	if (value == 0) digits[n++] = '0';
	while (value > 0 && n < (int)sizeof(digits)) {
		digits[n++] = (char)('0' + value % 10);
		value /= 10;
	}
	if (negative && len > 1) { *buf++ = '-'; len--; }
	for (i = n - 1; i >= 0 && len > 1; i--) {
		*buf++ = digits[i];
		len--;
	}
	*buf = '\0';
}

static int
read_file(const char *path, unsigned long maxSize, unsigned char **out,
          unsigned long *outSize)
{
	BPTR lock = 0, fh = 0;
	struct FileInfoBlock *fib = 0;
	struct Process *process;
	APTR oldWindow;
	unsigned long size;
	unsigned char *data = 0;
	int ok = 0;

	*out = 0;
	*outSize = 0;
	if (!DOSBase || !path[0]) return 0;

	process = (struct Process *)FindTask(0);
	oldWindow = process->pr_WindowPtr;
	process->pr_WindowPtr = (APTR)-1;

	lock = Lock((STRPTR)path, ACCESS_READ);
	if (!lock) goto done;
	fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock), MEMF_PUBLIC);
	if (!fib || !Examine(lock, fib) || fib->fib_Size < 0 ||
	    (unsigned long)fib->fib_Size > maxSize)
		goto done;
	size = (unsigned long)fib->fib_Size;
	FreeMem(fib, sizeof(struct FileInfoBlock));
	fib = 0;
	UnLock(lock);
	lock = 0;

	fh = Open((STRPTR)path, MODE_OLDFILE);
	if (!fh) goto done;
	data = (unsigned char *)plugin_alloc(size + 1);
	if (!data || (unsigned long)Read(fh, data, size) != size) goto done;
	data[size] = 0;
	*out = data;
	*outSize = size;
	data = 0;
	ok = 1;

done:
	if (data) plugin_free(data);
	if (fh) Close(fh);
	if (fib) FreeMem(fib, sizeof(struct FileInfoBlock));
	if (lock) UnLock(lock);
	process->pr_WindowPtr = oldWindow;
	return ok;
}

static unsigned long
be32(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
	       ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static unsigned int
be16(const unsigned char *p)
{
	return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static double
float_be(const unsigned char *p)
{
	union { unsigned long bits; float value; } convert;
	convert.bits = be32(p);
	return (double)convert.value;
}

static int
same_id(const unsigned char *p, const char *id)
{
	return p[0] == (unsigned char)id[0] && p[1] == (unsigned char)id[1] &&
	       p[2] == (unsigned char)id[2] && p[3] == (unsigned char)id[3];
}

static void
set_surface_defaults(SurfaceParams *params)
{
	/* LightWave surface defaults when a sub-chunk is absent */
	params->color[0] = params->color[1] = params->color[2] = 200.0 / 255.0;
	params->luminous = params->specular = 0.0;
	params->mirror = params->transparency = 0.0;
	params->diffuse = 1.0;
	params->eta = 1.0;
	params->roughness = 1.0 / 16.0;
}

static int
parse_surface_chunk(const unsigned char *data, unsigned long length,
                    SurfaceParams *params)
{
	unsigned long pos = 0;
	int explicitLuminous = 0;
	unsigned int flags = 0;

	while (pos < length && data[pos]) pos++;
	if (pos >= length) return 0;
	pos++;
	if (pos & 1) pos++;

	while (pos + 6 <= length) {
		const unsigned char *value = data + pos + 6;
		unsigned int size = be16(data + pos + 4);
		unsigned long next = pos + 6 + (unsigned long)size;

		if (next > length) break;
		if (same_id(data + pos, "COLR") && size >= 3) {
			params->color[0] = value[0] / 255.0;
			params->color[1] = value[1] / 255.0;
			params->color[2] = value[2] / 255.0;
		} else if (same_id(data + pos, "FLAG") && size >= 2) {
			flags = be16(value);
		} else if (same_id(data + pos, "LUMI") && size >= 2) {
			params->luminous = be16(value) / 256.0;
			explicitLuminous = 1;
		} else if (same_id(data + pos, "VLUM") && size >= 4) {
			params->luminous = float_be(value);
			explicitLuminous = 1;
		} else if (same_id(data + pos, "DIFF") && size >= 2) {
			params->diffuse = be16(value) / 256.0;
		} else if (same_id(data + pos, "VDIF") && size >= 4) {
			params->diffuse = float_be(value);
		} else if (same_id(data + pos, "SPEC") && size >= 2) {
			params->specular = be16(value) / 256.0;
		} else if (same_id(data + pos, "VSPC") && size >= 4) {
			params->specular = float_be(value);
		} else if (same_id(data + pos, "REFL") && size >= 2) {
			params->mirror = be16(value) / 256.0;
		} else if (same_id(data + pos, "VRFL") && size >= 4) {
			params->mirror = float_be(value);
		} else if (same_id(data + pos, "TRAN") && size >= 2) {
			params->transparency = be16(value) / 256.0;
		} else if (same_id(data + pos, "VTRN") && size >= 4) {
			params->transparency = float_be(value);
		} else if (same_id(data + pos, "GLOS") && size >= 2) {
			unsigned int gloss = be16(value);
			/* ShaderAccess.roughness is the reciprocal of glossiness */
			if (gloss >= 1)
				params->roughness = 1.0 / (double)gloss;
		} else if (same_id(data + pos, "RIND") && size >= 4) {
			params->eta = float_be(value);
		}
		pos = next + (next & 1);
	}
	if (!explicitLuminous && (flags & 1)) params->luminous = 1.0;
	return 1;
}

static int
load_surface(const char *path, SurfaceParams *params)
{
	unsigned char *data;
	unsigned long size, pos;
	int found = 0;

	if (!read_file(path, MAX_SURF_BYTES, &data, &size)) return 0;
	if (size < 12 || !same_id(data, "FORM") || !same_id(data + 8, "LWOB")) {
		plugin_free(data);
		return 0;
	}

	set_surface_defaults(params);
	pos = 12;
	while (pos + 8 <= size) {
		unsigned long chunkSize = be32(data + pos + 4);
		unsigned long next;
		if (chunkSize > size - pos - 8)
			break;
		next = pos + 8 + chunkSize;
		if (same_id(data + pos, "SURF")) {
			found = parse_surface_chunk(data + pos + 8, chunkSize, params);
			break;
		}
		pos = next + (next & 1);
	}
	plugin_free(data);
	return found;
}

static int
has_extension(const char *path)
{
	const char *p;
	for (p = path + strlen(path); p > path; p--) {
		if (p[-1] == '.') return 1;
		if (p[-1] == ':' || p[-1] == '/') return 0;
	}
	return 0;
}

static void
free_entries(SurfListInst *inst)
{
	if (inst->entries) plugin_free(inst->entries);
	inst->entries = 0;
	inst->numEntries = 0;
	inst->capacity = 0;
	inst->active = -1;
}

static int
append_entry(SurfListInst *inst, int frame, const char *path)
{
	SurfaceEntry *entry;
	if (inst->numEntries == inst->capacity) {
		SurfaceEntry *newEntries;
		int i, newCapacity = inst->capacity ? inst->capacity * 2 : 16;
		if (newCapacity > MAX_ENTRIES) return 0;
		newEntries = (SurfaceEntry *)plugin_alloc(sizeof(SurfaceEntry) * newCapacity);
		if (!newEntries) return 0;
		for (i = 0; i < inst->numEntries; i++) newEntries[i] = inst->entries[i];
		if (inst->entries) plugin_free(inst->entries);
		inst->entries = newEntries;
		inst->capacity = newCapacity;
	}
	entry = &inst->entries[inst->numEntries];
	entry->frame = frame;
	strncpy(entry->path, path, MAX_PATH - 1);
	entry->path[MAX_PATH - 1] = '\0';
	if (!load_surface(entry->path, &entry->params)) {
		if (!has_extension(entry->path) && strlen(entry->path) + 4 < MAX_PATH) {
			strcat(entry->path, ".srf");
			if (load_surface(entry->path, &entry->params)) {
				inst->numEntries++;
				return 1;
			}
		}
		return 0;
	}
	inst->numEntries++;
	return 1;
}

static int
parse_frame(const char *text, int *frame)
{
	int negative = 0, value = 0, digits = 0;
	while (*text == ' ' || *text == '\t') text++;
	if (*text == '-') { negative = 1; text++; }
	while (*text >= '0' && *text <= '9') {
		if (value > (2147483647 - (*text - '0')) / 10)
			return 0;
		value = value * 10 + (*text - '0');
		text++;
		digits++;
	}
	while (*text == ' ' || *text == '\t') text++;
	if (!digits || *text) return 0;
	*frame = negative ? -value : value;
	return 1;
}

static char *
trim_line(char *text)
{
	char *end;
	while (*text == ' ' || *text == '\t') text++;
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
	return text;
}

static void
sort_entries(SurfaceEntry *entries, int count)
{
	int gap, i, j, k;
	for (gap = count / 2; gap > 0; gap /= 2) {
		for (i = gap; i < count; i++) {
			for (j = i; j >= gap && entries[j - gap].frame > entries[j].frame; j -= gap) {
				char *left = (char *)&entries[j - gap];
				char *right = (char *)&entries[j];
				for (k = 0; k < (int)sizeof(SurfaceEntry); k++) {
					char swap = left[k]; left[k] = right[k]; right[k] = swap;
				}
			}
		}
	}
}

static void
load_list(SurfListInst *inst)
{
	unsigned char *data;
	unsigned long size, pos = 0;
	int waitingForPath = 0, frame = 0;

	free_entries(inst);
	inst->errors = 0;
	inst->loaded = 1;
	/* no path selected: a valid, empty state, not an error */
	if (!inst->listPath[0]) return;
	if (!read_file(inst->listPath, MAX_LIST_BYTES, &data, &size)) {
		inst->errors = 1;
		return;
	}
	while (pos < size) {
		unsigned long start = pos;
		char *text;
		while (pos < size && data[pos] != '\n' && data[pos] != '\r')
			pos++;
		data[pos] = '\0';
		text = trim_line((char *)data + start);
		if (pos < size) {
			pos++;
			while (pos < size && (data[pos] == '\n' || data[pos] == '\r'))
				pos++;
		}
		if (!text[0] || text[0] == '#')
			continue;
		if (!waitingForPath) {
			if (parse_frame(text, &frame))
				waitingForPath = 1;
			else
				inst->errors++;
		} else {
		if (!append_entry(inst, frame, text))
			inst->errors++;
		waitingForPath = 0;
		}
	}
	if (waitingForPath)
		inst->errors++;
	plugin_free(data);
	if (inst->numEntries > 1)
		sort_entries(inst->entries, inst->numEntries);
}

static int
find_entry(const SurfListInst *inst, int frame)
{
	int lo = 0, hi = inst->numEntries - 1, best = -1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (inst->entries[mid].frame <= frame) { best = mid; lo = mid + 1; }
		else hi = mid - 1;
	}
	return best;
}

XCALL_(static LWInstance)
Create(LWError *err)
{
	SurfListInst *inst;
	XCALL_INIT;
	(void)err;
	inst = (SurfListInst *)plugin_alloc(sizeof(SurfListInst));
	if (!inst) return 0;
	inst->active = -1;
	return inst;
}

XCALL_(static void)
Destroy(SurfListInst *inst)
{
	XCALL_INIT;
	if (inst) { free_entries(inst); plugin_free(inst); }
}

XCALL_(static LWError)
Copy(SurfListInst *from, SurfListInst *to)
{
	int i;
	XCALL_INIT;
	free_entries(to);
	strcpy(to->listPath, from->listPath);
	to->errors = from->errors;
	to->loaded = from->loaded;
	to->active = from->active;
	if (from->numEntries) {
		to->entries = (SurfaceEntry *)plugin_alloc(sizeof(SurfaceEntry) * from->numEntries);
		if (!to->entries) return "Out of memory";
		to->capacity = to->numEntries = from->numEntries;
		for (i = 0; i < from->numEntries; i++) to->entries[i] = from->entries[i];
	}
	return 0;
}

XCALL_(static LWError)
Load(SurfListInst *inst, const LWLoadState *ls)
{
	XCALL_INIT;
	if (spi_read_string_record(ls, inst->listPath, sizeof(inst->listPath)) <= 0)
		inst->listPath[0] = '\0';
	inst->loaded = 0;
	load_list(inst);
	return 0;
}

XCALL_(static LWError)
Save(SurfListInst *inst, const LWSaveState *ss)
{
	XCALL_INIT;
	spi_write_string_record(ss, inst->listPath);
	return 0;
}

XCALL_(static LWError)
Init(SurfListInst *inst)
{
	XCALL_INIT;
	if (!inst->loaded) load_list(inst);
	return 0;
}

XCALL_(static void)
Cleanup(SurfListInst *inst)
{
	XCALL_INIT;
	(void)inst;
}

XCALL_(static LWError)
NewTime(SurfListInst *inst, LWFrame frame, LWTime time)
{
	XCALL_INIT;
	(void)time;
	if (!inst->loaded) load_list(inst);
	inst->active = find_entry(inst, frame);
	return 0;
}

XCALL_(static void)
Evaluate(SurfListInst *inst, ShaderAccess *sa)
{
	SurfaceParams *params;
	XCALL_INIT;
	if (inst->active < 0 || inst->active >= inst->numEntries) return;
	params = &inst->entries[inst->active].params;
	sa->color[0] = params->color[0];
	sa->color[1] = params->color[1];
	sa->color[2] = params->color[2];
	sa->luminous = params->luminous;
	sa->diffuse = params->diffuse;
	sa->specular = params->specular;
	sa->mirror = params->mirror;
	sa->transparency = params->transparency;
	sa->eta = params->eta;
	sa->roughness = params->roughness;
}

XCALL_(static unsigned int)
Flags(SurfListInst *inst)
{
	XCALL_INIT;
	(void)inst;
	return LWSHF_COLOR | LWSHF_LUMINOUS | LWSHF_DIFFUSE | LWSHF_SPECULAR |
	       LWSHF_MIRROR | LWSHF_TRANSP | LWSHF_ETA | LWSHF_ROUGH;
}

static void
status_text(const SurfListInst *inst, char *text, int length)
{
	char number[12];
	strncpy(text, "Parsed ", length - 1);
	text[length - 1] = '\0';
	int_to_str(inst->numEntries, number, sizeof(number));
	strncat(text, number, length - (int)strlen(text) - 1);
	strncat(text, " entries, ", length - (int)strlen(text) - 1);
	int_to_str(inst->errors, number, sizeof(number));
	strncat(text, number, length - (int)strlen(text) - 1);
	strncat(text, " errors", length - (int)strlen(text) - 1);
}

XCALL_(static int)
Interface(long version, GlobalFunc *global, SurfListInst *inst, void *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID pan;
	LWControl *pathCtl, *infoCtl;
	static LWPanControlDesc desc;
	static LWValue sval = { LWT_STRING };
	XCALL_INIT;
	(void)serverData;
	if (version != 1) return AFUNC_BADVERSION;
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	if (!panl) return AFUNC_BADGLOBAL;
	pan = PAN_CREATE(panl, "SurfList v" PLUGIN_VERSION);
	if (!pan) return AFUNC_BADGLOBAL;
	/* the file control needs this to reach the file requester */
	panl->globalFun = global;
	pathCtl = FILE_CTL(panl, pan, "List file", 300);
	status_text(inst, uiStatus, sizeof(uiStatus));
	uiLines[0] = uiStatus;
	uiLines[1] = 0;
	infoCtl = TEXT_CTL(panl, pan, "", uiLines);
	(void)infoCtl;
	strcpy(uiPath, inst->listPath);
	SET_STR(pathCtl, uiPath, sizeof(uiPath));
	if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
		GET_STR(pathCtl, uiPath, sizeof(uiPath));
		if (strcmp(uiPath, inst->listPath) != 0) {
			strcpy(inst->listPath, uiPath);
			inst->loaded = 0;
		}
		/* retry an unchanged path whose previous load failed */
		if (!inst->loaded || inst->errors)
			load_list(inst);
	}
	PAN_KILL(panl, pan);
	return AFUNC_OK;
}

XCALL_(int)
Activate(long version, GlobalFunc *global, void *local, void *serverData)
{
	ShaderHandler *handler = (ShaderHandler *)local;
	XCALL_INIT;
	(void)serverData;
	if (version < 1) return AFUNC_BADVERSION;
	handler->create = (void *)Create;
	handler->destroy = (void *)Destroy;
	handler->load = (void *)Load;
	handler->save = (void *)Save;
	handler->copy = (void *)Copy;
	handler->init = (void *)Init;
	handler->cleanup = (void *)Cleanup;
	handler->newTime = (void *)NewTime;
	handler->evaluate = (void *)Evaluate;
	handler->flags = (void *)Flags;
	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!msg) return AFUNC_BADGLOBAL;
	return AFUNC_OK;
}

ServerRecord ServerDesc[] = {
	{ "ShaderHandler", "SurfList", (ActivateFunc *)Activate },
	{ "ShaderInterface", "SurfList", (ActivateFunc *)Interface },
	{ 0 }
};
