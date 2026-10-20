/*
 * OBJLIST.C -- Layout Object Replacement Plugin
 *
 * Replaces an object's geometry from a frame-to-file list.  Settings and
 * parsed entries live in instance data because Layout can use more than one
 * ObjList instance in a scene.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/memory.h>

extern struct DosLibrary *DOSBase;
extern struct ExecBase   *SysBase;

#define MAX_PATH     256
#define MAX_ENTRIES  4096

typedef struct {
	int  frame;
	char filename[MAX_PATH];
} FrameEntry;

typedef struct {
	char        listPath[MAX_PATH];
	char        listDir[MAX_PATH];
	char        objectPath[MAX_PATH];
	FrameEntry *entries;
	int         numEntries;
	int         capacity;
	int         listReadable;
} ObjListInst;

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

static int
ci_char(int c)
{
	if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
	return c;
}

static int
ci_streq(const char *a, const char *b)
{
	while (*a && *b) {
		if (ci_char((unsigned char)*a) != ci_char((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

static void
int_to_str(int value, char *buf, int bufLen)
{
	char digits[12];
	int count, negative, i;

	count = 0;
	negative = value < 0;
	if (negative) value = -value;
	if (value == 0) digits[count++] = '0';
	while (value > 0 && count < (int)sizeof(digits)) {
		digits[count++] = (char)('0' + (value % 10));
		value /= 10;
	}
	if (negative) {
		if (bufLen <= 1) {
			*buf = '\0';
			return;
		}
		*buf++ = '-';
		bufLen--;
	}
	for (i = count - 1; i >= 0 && bufLen > 1; i--) {
		*buf++ = digits[i];
		bufLen--;
	}
	*buf = '\0';
}

static void
copy_path(char *dest, const char *src)
{
	strncpy(dest, src, MAX_PATH - 1);
	dest[MAX_PATH - 1] = '\0';
}

static void
split_path(const char *fullPath, char *dir, char *name)
{
	const char *p;
	const char *lastSep;
	int len;

	lastSep = 0;
	for (p = fullPath; *p; p++) {
		if (*p == ':' || *p == '/') lastSep = p;
	}
	if (!lastSep) {
		dir[0] = '\0';
		copy_path(name, fullPath);
		return;
	}
	len = (int)(lastSep - fullPath + 1);
	if (len >= MAX_PATH) len = MAX_PATH - 1;
	memcpy(dir, fullPath, len);
	dir[len] = '\0';
	copy_path(name, lastSep + 1);
}

static void
extract_dir(const char *fullPath, char *dir)
{
	const char *p;
	const char *lastSep;
	int len;

	lastSep = 0;
	for (p = fullPath; *p; p++) {
		if (*p == ':' || *p == '/') lastSep = p;
	}
	if (!lastSep) {
		dir[0] = '\0';
		return;
	}
	len = (int)(lastSep - fullPath + 1);
	if (len >= MAX_PATH) len = MAX_PATH - 1;
	memcpy(dir, fullPath, len);
	dir[len] = '\0';
}

static int
is_absolute_path(const char *path)
{
	while (*path) {
		if (*path++ == ':') return 1;
	}
	return 0;
}

static char *
trim_line(char *line)
{
	char *end;
	char *comment;

	while (*line == ' ' || *line == '\t') line++;
	comment = line;
	while (*comment) {
		if (*comment == '#') {
			*comment = '\0';
			break;
		}
		comment++;
	}
	end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
		*--end = '\0';
	}
	return line;
}

static int
parse_frame(const char *line, int *frame)
{
	int value;

	if (*line < '0' || *line > '9') return 0;
	value = 0;
	while (*line >= '0' && *line <= '9') {
		if (value > 214748364) return 0;
		value = value * 10 + (*line - '0');
		line++;
	}
	if (*line != '\0') return 0;
	*frame = value;
	return 1;
}

static void
free_entries(ObjListInst *inst)
{
	if (inst->entries) {
		plugin_free(inst->entries);
		inst->entries = 0;
	}
	inst->numEntries = 0;
	inst->capacity = 0;
}

static int
add_entry(ObjListInst *inst, int frame, const char *filename)
{
	FrameEntry *newEntries;
	int newCapacity;
	int i;
	char *dest;

	if (inst->numEntries >= MAX_ENTRIES) return 0;
	if (inst->numEntries == inst->capacity) {
		newCapacity = inst->capacity ? inst->capacity * 2 : 32;
		if (newCapacity > MAX_ENTRIES) newCapacity = MAX_ENTRIES;
		newEntries = (FrameEntry *)plugin_alloc(sizeof(FrameEntry) * newCapacity);
		if (!newEntries) return 0;
		for (i = 0; i < inst->numEntries; i++) newEntries[i] = inst->entries[i];
		plugin_free(inst->entries);
		inst->entries = newEntries;
		inst->capacity = newCapacity;
	}

	dest = inst->entries[inst->numEntries].filename;
	if (!is_absolute_path(filename) && inst->listDir[0]) {
		copy_path(dest, inst->listDir);
		strncat(dest, filename, MAX_PATH - 1 - strlen(dest));
	} else {
		copy_path(dest, filename);
	}
	inst->entries[inst->numEntries].frame = frame;
	inst->numEntries++;
	return 1;
}

static void
swap_entries(FrameEntry *a, FrameEntry *b)
{
	char *left;
	char *right;
	char temp;
	int i;

	left = (char *)a;
	right = (char *)b;
	for (i = 0; i < (int)sizeof(FrameEntry); i++) {
		temp = left[i];
		left[i] = right[i];
		right[i] = temp;
	}
}

static void
sort_entries(FrameEntry *entries, int count)
{
	int gap;
	int i;
	int j;

	for (gap = count / 2; gap > 0; gap /= 2) {
		for (i = gap; i < count; i++) {
			for (j = i; j >= gap; j -= gap) {
				if (entries[j - gap].frame <= entries[j].frame) break;
				swap_entries(&entries[j - gap], &entries[j]);
			}
		}
	}
}

static BPTR
quiet_lock(const char *path)
{
	struct Process *process;
	APTR oldWindow;
	BPTR lock;

	process = (struct Process *)FindTask(0);
	if (process) {
		oldWindow = process->pr_WindowPtr;
		process->pr_WindowPtr = (APTR)-1;
	}
	lock = Lock((STRPTR)path, ACCESS_READ);
	if (process) process->pr_WindowPtr = oldWindow;
	return lock;
}


static void
parse_list(ObjListInst *inst)
{
	BPTR file;
	struct Process *process;
	APTR oldWindow;
	char *line;
	char ch;
	char *text;
	int lineLen;
	int haveFrame;
	int frame;

	free_entries(inst);
	inst->listReadable = 0;
	if (!DOSBase || !inst->listPath[0]) return;

	process = (struct Process *)FindTask(0);
	if (process) {
		oldWindow = process->pr_WindowPtr;
		process->pr_WindowPtr = (APTR)-1;
	}
	file = Open((STRPTR)inst->listPath, MODE_OLDFILE);
	if (process) process->pr_WindowPtr = oldWindow;
	if (!file) return;
	inst->listReadable = 1;
	line = (char *)plugin_alloc(MAX_PATH);
	if (!line) {
		Close(file);
		return;
	}

	lineLen = 0;
	haveFrame = 0;
	frame = 0;
	while (Read(file, &ch, 1) == 1) {
		if (ch == '\r') continue;
		if (ch != '\n' && lineLen < MAX_PATH - 1) {
			line[lineLen++] = ch;
			continue;
		}
		if (ch != '\n') continue;
		line[lineLen] = '\0';
		text = trim_line(line);
		if (*text) {
			if (!haveFrame) {
				if (parse_frame(text, &frame)) haveFrame = 1;
			} else {
				if (add_entry(inst, frame, text)) haveFrame = 0;
			}
		}
		lineLen = 0;
	}
	if (lineLen > 0) {
		line[lineLen] = '\0';
		text = trim_line(line);
		if (*text && haveFrame) add_entry(inst, frame, text);
	}
	Close(file);
	plugin_free(line);
	if (inst->numEntries > 1) sort_entries(inst->entries, inst->numEntries);
}

static void
set_list_path(ObjListInst *inst, const char *path)
{
	copy_path(inst->listPath, path);
	extract_dir(inst->listPath, inst->listDir);
	parse_list(inst);
}

static int
find_entry(const ObjListInst *inst, int frame)
{
	int low;
	int high;
	int middle;
	int found;

	low = 0;
	high = inst->numEntries - 1;
	found = -1;
	while (low <= high) {
		middle = low + (high - low) / 2;
		if (inst->entries[middle].frame <= frame) {
			found = middle;
			low = middle + 1;
		} else {
			high = middle - 1;
		}
	}
	return found;
}

XCALL_(static LWInstance)
Create(LWError *error)
{
	ObjListInst *inst;

	XCALL_INIT;
	(void)error;
	inst = (ObjListInst *)plugin_alloc(sizeof(ObjListInst));
	return inst;
}

XCALL_(static void)
Destroy(ObjListInst *inst)
{
	XCALL_INIT;
	if (!inst) return;
	free_entries(inst);
	plugin_free(inst);
}

XCALL_(static LWError)
Copy(ObjListInst *from, ObjListInst *to)
{
	int i;

	XCALL_INIT;
	copy_path(to->listPath, from->listPath);
	copy_path(to->listDir, from->listDir);
	copy_path(to->objectPath, from->objectPath);
	to->listReadable = from->listReadable;
	free_entries(to);
	if (from->numEntries == 0) return 0;
	to->entries = (FrameEntry *)plugin_alloc(sizeof(FrameEntry) * from->numEntries);
	if (!to->entries) return "Not enough memory for ObjList entries.";
	to->numEntries = from->numEntries;
	to->capacity = from->numEntries;
	for (i = 0; i < from->numEntries; i++) to->entries[i] = from->entries[i];
	return 0;
}

XCALL_(static LWError)
Load(ObjListInst *inst, const LWLoadState *loadState)
{
	XCALL_INIT;
	if (spi_read_string_record(loadState, inst->objectPath,
	                           sizeof(inst->objectPath)) > 0 &&
	    inst->objectPath[0])
		set_list_path(inst, inst->objectPath);
	inst->objectPath[0] = '\0';
	return 0;
}

XCALL_(static LWError)
Save(ObjListInst *inst, const LWSaveState *saveState)
{
	XCALL_INIT;
	spi_write_string_record(saveState, inst->listPath);
	return 0;
}

XCALL_(static void)
Evaluate(ObjListInst *inst, ObjReplacementAccess *access)
{
	const char *target;
	BPTR lock;
	int entry;

	XCALL_INIT;
	if (!inst->objectPath[0] && access->curFilename && access->curFilename[0])
		copy_path(inst->objectPath, access->curFilename);
	entry = find_entry(inst, access->newFrame);
	target = 0;
	if (entry >= 0) {
		lock = quiet_lock(inst->entries[entry].filename);
		if (!lock) return;
		UnLock(lock);
		target = inst->entries[entry].filename;
	} else if (inst->objectPath[0]) {
		target = inst->objectPath;
	}
	if (target && (!access->curFilename ||
	    !ci_streq(access->curFilename, target)))
		access->newFilename = target;
}

static ObjListInst *uiInst;
static GlobalFunc *uiGlobal;
static LWControl *uiPathControl;
static LWControl *uiInfoControl;
static LWControl *uiListControl;
static char uiPath[MAX_PATH];
static char uiDir[MAX_PATH];
static char uiName[MAX_PATH];
static char uiFullName[MAX_PATH];
static char uiOriginalPath[MAX_PATH];
static char uiInfo[96];
static char uiEntry[80];
static const char *uiInfoLines[2];
static LWValue uiStringValue = { LWT_STRING };

static void
set_ui_path(void)
{
	uiStringValue.str.buf = uiPath;
	uiStringValue.str.bufLen = sizeof(uiPath);
	(*uiPathControl->set)(uiPathControl, CTL_VALUE, &uiStringValue);
}


static void
update_ui_info(void)
{
	char number[12];

	if (!uiInst || !uiInst->listPath[0]) {
		strcpy(uiInfo, "No list file selected.");
	} else if (!uiInst->listReadable) {
		strcpy(uiInfo, "Could not open list file.");
	} else {
		strcpy(uiInfo, "Parsed entries: ");
		int_to_str(uiInst->numEntries, number, sizeof(number));
		strcat(uiInfo, number);
	}
	uiInfoLines[0] = uiInfo;
	uiInfoLines[1] = 0;
}

static int
ui_entry_count(void *data)
{
	(void)data;
	return uiInst ? uiInst->numEntries : 0;
}

static char *
ui_entry_name(void *data, int index)
{
	char number[12];
	int len;

	(void)data;
	if (!uiInst || index < 0 || index >= uiInst->numEntries) return "";
	strcpy(uiEntry, "Frame ");
	int_to_str(uiInst->entries[index].frame, number, sizeof(number));
	strcat(uiEntry, number);
	strcat(uiEntry, " -> ");
	len = strlen(uiEntry);
	strncpy(uiEntry + len, uiInst->entries[index].filename,
	        sizeof(uiEntry) - len - 1);
	uiEntry[sizeof(uiEntry) - 1] = '\0';
	return uiEntry;
}

static void
pick_list(LWControlID control, void *data)
{
	FileReqFunc *fileRequest;
	int nameLen;

	(void)control;
	(void)data;
	if (!uiInst || !uiGlobal) return;

	if (uiInst->listPath[0]) {
		split_path(uiInst->listPath, uiDir, uiName);
	} else if (uiInst->objectPath[0]) {
		split_path(uiInst->objectPath, uiDir, uiName);
		uiName[0] = '\0';
	} else {
		uiDir[0] = '\0';
		uiName[0] = '\0';
	}
	fileRequest = (FileReqFunc *)(*uiGlobal)("File Request", GFUSE_TRANSIENT);
	if (!fileRequest) return;
	nameLen = strlen(uiName);
	if (nameLen < MAX_PATH - 1) {
		memmove(uiName + 1, uiName, nameLen + 1);
		uiName[0] = '<';
	}
	uiFullName[0] = '\0';
	if ((*fileRequest)("Select ObjList file", uiName, uiDir, uiFullName,
	                   MAX_PATH) <= 0 || !uiFullName[0])
		return;

	set_list_path(uiInst, uiFullName);
	copy_path(uiPath, uiInst->listPath);
	update_ui_info();
	set_ui_path();
	REDRAW_CON(uiPathControl);
	REDRAW_CON(uiInfoControl);
	REDRAW_CON(uiListControl);
}

XCALL_(static int)
Interface(long version, GlobalFunc *global, ObjListInst *inst, void *serverData)
{
	LWPanelFuncs *panelFuncs;
	LWPanelID panel;
	LWControl *pickControl;
	int accepted;
	static LWPanControlDesc desc;
	static LWValue ival = { LWT_INTEGER };
	XCALL_INIT;
	(void)serverData;
	(void)ival;
	if (version != 1) return AFUNC_BADVERSION;
	panelFuncs = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	if (!panelFuncs) return AFUNC_BADGLOBAL;

	uiInst = inst;
	uiGlobal = global;
	copy_path(uiOriginalPath, inst->listPath);
	copy_path(uiPath, inst->listPath);
	update_ui_info();
	panel = PAN_CREATE(panelFuncs, "ObjList v" PLUGIN_VERSION);
	if (!panel) {
		uiInst = 0;
		return AFUNC_BADLOCAL;
	}
	panelFuncs->globalFun = global;
	uiPathControl = STRRO_CTL(panelFuncs, panel, "List file", 50);
	set_ui_path();
	pickControl = BUTTON_CTL(panelFuncs, panel, "Pick List...");
	CON_SETEVENT(pickControl, pick_list, 0);
	uiInfoControl = TEXT_CTL(panelFuncs, panel, "", uiInfoLines);
	uiListControl = LISTBOX_CTL(panelFuncs, panel, "Frame -> file", 400, 12,
	                            ui_entry_name, ui_entry_count);
	accepted = (*panelFuncs->open)(panel, PANF_BLOCKING | PANF_CANCEL);
	if (!accepted)
		set_list_path(inst, uiOriginalPath);
	PAN_KILL(panelFuncs, panel);
	uiInst = 0;
	uiGlobal = 0;
	return AFUNC_OK;
}

XCALL_(int)
Activate(long version, GlobalFunc *global, void *local, void *serverData)
{
	ObjReplacementHandler_V1 *handler;

	XCALL_INIT;
	(void)serverData;
	if (version < 1) return AFUNC_BADVERSION;
	handler = (ObjReplacementHandler_V1 *)local;
	handler->create = (void *)Create;
	handler->destroy = (void *)Destroy;
	handler->copy = (void *)Copy;
	handler->load = (void *)Load;
	handler->save = (void *)Save;
	handler->evaluate = (void *)Evaluate;
	return AFUNC_OK;
}

ServerRecord ServerDesc[] = {
	{ "ObjReplacementHandler", "ObjList", (ActivateFunc *)Activate },
	{ "ObjReplacementInterface", "ObjList", (ActivateFunc *)Interface },
	{ 0 }
};
