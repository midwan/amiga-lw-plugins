#ifndef QUIET_DOS_H
#define QUIET_DOS_H

/*
 * Lock/Open a user or scene path without DOS requesters.
 *
 * A path on a missing volume (e.g. "3d:objects/x.lwo") makes AmigaDOS show
 * "Please insert volume", which blocks Layout, Modeler and unattended
 * ScreamerNet renders until someone answers it. These wrappers set the
 * calling process's pr_WindowPtr to -1 for the call, so the call just fails
 * and the plugin reports the error itself.
 */

#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#ifdef __GNUC__
#define QDOS_UNUSED __attribute__((unused))
#else
#define QDOS_UNUSED
#endif

static struct Process *
QDOS_UNUSED
quiet_begin(APTR *oldWindow)
{
	struct Process *process = (struct Process *)FindTask(0);

	if (!process || process->pr_Task.tc_Node.ln_Type != NT_PROCESS)
		return 0;
	*oldWindow = process->pr_WindowPtr;
	process->pr_WindowPtr = (APTR)-1;
	return process;
}

static void
QDOS_UNUSED
quiet_end(struct Process *process, APTR oldWindow)
{
	if (process)
		process->pr_WindowPtr = oldWindow;
}

static BPTR
QDOS_UNUSED
quiet_lock(const char *path, long mode)
{
	APTR oldWindow = 0;
	struct Process *process = quiet_begin(&oldWindow);
	BPTR lock = Lock((STRPTR)path, mode);

	quiet_end(process, oldWindow);
	return lock;
}

static BPTR
QDOS_UNUSED
quiet_open(const char *path, long mode)
{
	APTR oldWindow = 0;
	struct Process *process = quiet_begin(&oldWindow);
	BPTR fh = Open((STRPTR)path, mode);

	quiet_end(process, oldWindow);
	return fh;
}

#endif
