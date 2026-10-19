/*
 * LWSDK Header File
 * Copyright 1995,1997,1998  NewTek, Inc.
 */
#ifndef LW_BASE_H
#define LW_BASE_H

#include <moni.h>
#include <plug.h>

typedef struct st_ObjectImport {
	int               result;
	const char       *filename;
	Monitor          *monitor;
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
#define OBJPOLF_FACE      0
#define OBJPOLF_CURVE    (1<<0)
#define OBJPOLF_DETAIL   (1<<1)
#define OBJPOLF_STARTCC  (1<<2)
#define OBJPOLF_ENDCC    (1<<3)
#define OBJSTAT_OK       0
#define OBJSTAT_NOREC    1
#define OBJSTAT_BADFILE  2
#define OBJSTAT_ABORTED  3
#define OBJSTAT_FAILED   99
struct st_FileReq_Local;
typedef int             FileActivateFunc (int version,
				struct st_FileReq_Local *);
typedef int             FileReqFunc (const char *hail, char *name,
				     char *path, char *fullName,
				     int buflen);
typedef struct st_MessageFuncs {
	void            (*info)    (const char *, const char *);
	void            (*error)   (const char *, const char *);
	void            (*warning) (const char *, const char *);
} MessageFuncs;
#define LWSYS_TYPEBITS          0xF0000000
#define LWSYS_SERIALBITS        0x0FFFFFFF

#define LWSYS_LAYOUT            0x00000000
#define LWSYS_MODELER           0x10000000
#define LWSYS_SCREAMERNET       0x20000000
#define LWINF_PRODUCT           0x0000000F
#define LWINF_BUILD             0x0000FFF0
#define LWINF_MINORREV          0x000F0000
#define LWINF_MAJORREV          0x00F00000
#define LWINF_RESERVED          0xFF000000

#define LWINF_PRODLWAV          0x00000001
#define LWINF_PRODINSP3D        0x00000002
#define LWINF_PRODOTHER         0x00000004

#define LWINF_GETMAJOR(x)       (((x) & LWINF_MAJORREV) >> 20)
#define LWINF_GETMINOR(x)       (((x) & LWINF_MINORREV) >> 16)
#define LWINF_GETBUILD(x)       (((x) & LWINF_BUILD) >> 4)
#define LWLOC_LANGID            0x0000FFFF
#define LWLOC_RESERVED          0xFFFF0000

#define LWLOC_GERMAN            0x00000407
#define LWLOC_USENGLISH         0x00000409
#define LWLOC_UKENGLISH         0x00000809
#define LWLOC_FRENCH            0x0000040c
#define LWLOC_ITALIAN           0x00000410
#define LWLOC_JAPANESE          0x00000411
#define LWLOC_RUSSIAN           0x00000419
#define LWLOC_SWEDISH           0x0000041D
typedef    const char *DirInfoFunc (const char *);

#endif
