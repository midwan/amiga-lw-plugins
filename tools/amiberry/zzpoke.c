/* Scripted input for GUI smoke tests: drives a window by real input
 * events (input.device), coordinates relative to the window's top-left.
 * Script lines:
 *   wait <ticks>            Delay (50 = 1 s)
 *   click <x> <y>           move + left click inside the window
 *   key <rawkey> [qual]     press/release (qual hex, e.g. 80 = RAmiga)
 *   log <text>              append text to the log
 *   windows                 log every window on the default screen
 * Usage: zzpoke <title-prefix> <script> <log>
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase = NULL;
static struct MsgPort *port;
static struct IOStdReq *io;
static BPTR logf;

static void logline(const char *s)
{
	if (logf) { Write(logf, (APTR)s, strlen(s)); Write(logf, (APTR)"\n", 1); }
}

static struct Window *find_window(const char *prefix, struct Screen **scr)
{
	ULONG lock = LockIBase(0);
	struct Screen *s;
	struct Window *w, *found = NULL;
	for (s = IntuitionBase->FirstScreen; s && !found; s = s->NextScreen)
		for (w = s->FirstWindow; w; w = w->NextWindow)
			if (w->Title && strncmp((char *)w->Title, prefix, strlen(prefix)) == 0) {
				found = w; *scr = s; break;
			}
	UnlockIBase(lock);
	return found;
}

static void send(struct InputEvent *ev)
{
	io->io_Command = IND_WRITEEVENT;
	io->io_Data = ev;
	io->io_Length = sizeof(*ev);
	DoIO((struct IORequest *)io);
}

static void move_to(struct Screen *scr, WORD x, WORD y)
{
	struct InputEvent ev;
	struct IEPointerPixel pp;
	memset(&ev, 0, sizeof(ev));
	pp.iepp_Screen = scr;
	pp.iepp_Position.X = x;
	pp.iepp_Position.Y = y;
	ev.ie_Class = IECLASS_NEWPOINTERPOS;
	ev.ie_SubClass = IESUBCLASS_PIXEL;
	ev.ie_EventAddress = &pp;
	send(&ev);
}

static void button(UWORD code)
{
	struct InputEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.ie_Class = IECLASS_RAWMOUSE;
	ev.ie_Code = code;
	ev.ie_Qualifier = IEQUALIFIER_RELATIVEMOUSE;
	send(&ev);
}

static void rawkey(UWORD code, UWORD qual)
{
	struct InputEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.ie_Class = IECLASS_RAWKEY;
	ev.ie_Code = code;
	ev.ie_Qualifier = qual;
	send(&ev);
	Delay(2);
	ev.ie_Code = code | IECODE_UP_PREFIX;
	send(&ev);
}

int main(int argc, char **argv)
{
	static char line[256], msg[300];
	FILE *script;
	if (argc < 4) return 20;
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
	logf = Open((CONST_STRPTR)argv[3], MODE_NEWFILE);
	port = CreateMsgPort();
	io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
	if (!IntuitionBase || !io || OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)io, 0)) {
		logline("setup failed"); return 20;
	}
	script = fopen(argv[2], "r");
	if (!script) { logline("no script"); return 20; }
	while (fgets(line, sizeof(line), script)) {
		char cmd[16]; int a = 0, b = 0;
		struct Screen *scr = NULL;
		struct Window *w;
		char *nl = strpbrk(line, "\r\n");
		if (nl) *nl = 0;
		if (!line[0] || line[0] == '#') continue;
		sscanf(line, "%15s %d %d", cmd, &a, &b);
		if (!strcmp(cmd, "wait")) { Delay(a); continue; }
		if (!strcmp(cmd, "log")) { logline(line + 4); continue; }
		if (!strcmp(cmd, "key")) {
			unsigned code = 0, qual = 0;
			sscanf(line + 4, "%x %x", &code, &qual);
			rawkey((UWORD)code, (UWORD)qual);
			snprintf(msg, sizeof(msg), "key %x %x", code, qual); logline(msg);
			continue;
		}
		w = find_window(argv[1], &scr);
		if (!strcmp(cmd, "windows")) {
			snprintf(msg, sizeof(msg), "window %s at %d,%d size %dx%d", w ? "found" : "missing",
				w ? w->LeftEdge : -1, w ? w->TopEdge : -1, w ? w->Width : -1, w ? w->Height : -1);
			logline(msg);
			continue;
		}
		if (!strcmp(cmd, "click")) {
			if (!w) { logline("click: window missing"); continue; }
			move_to(scr, (WORD)(w->LeftEdge + a), (WORD)(w->TopEdge + b));
			Delay(3);
			button(IECODE_LBUTTON);
			Delay(3);
			button(IECODE_LBUTTON | IECODE_UP_PREFIX);
			snprintf(msg, sizeof(msg), "click %d,%d", a, b); logline(msg);
			continue;
		}
		snprintf(msg, sizeof(msg), "unknown: %s", line); logline(msg);
	}
	fclose(script);
	logline("done");
	CloseDevice((struct IORequest *)io);
	DeleteIORequest((struct IORequest *)io);
	DeleteMsgPort(port);
	if (logf) Close(logf);
	CloseLibrary((struct Library *)IntuitionBase);
	return 0;
}
