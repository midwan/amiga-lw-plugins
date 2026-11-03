/*
 * LWSDK Header File
 * Copyright 1995,1997,1998  NewTek, Inc.
 */
#ifndef LW_RAN_H
#define LW_RAN_H

#include <lwbase.h>

typedef unsigned char     BufferValue;
typedef void *           LWItemID;
#define LWITEM_NULL      ((LWItemID) 0)
typedef int              LWItemType;
#define LWI_OBJECT       0
#define LWI_LIGHT        1
#define LWI_CAMERA       2
#define LWI_BONE         3
typedef int              LWItemParam;
#define LWIP_POSITION    1
#define LWIP_RIGHT       2
#define LWIP_UP          3
#define LWIP_FORWARD     4
#define LWIP_ROTATION    5
#define LWIP_SCALING     6
#define LWIP_PIVOT       7
#define LWIP_W_POSITION  8
#define LWIP_W_RIGHT     9
#define LWIP_W_UP        10
#define LWIP_W_FORWARD   11
typedef int              LWFrame;
typedef double           LWTime;
typedef const char *     LWError;
typedef void *           LWInstance;
#define LWIO_OBJECT      0
#define LWIO_SCENE       1
typedef struct st_LWLoadState {
	int               ioMode;
	void             *readData;
	int             (*read) (void *readData, char *buf,
				 int len);
} LWLoadState;

typedef struct st_LWSaveState {
	int               ioMode;
	void             *writeData;
	void            (*write) (void *writeData, char *buf,
				  int len);
} LWSaveState;
#define LWVECF_0        (1<<0)
#define LWVECF_1        (1<<1)
#define LWVECF_2        (1<<2)
#define LWOSHAD_SELF     (1<<0)
#define LWOSHAD_CAST     (1<<1)
#define LWOSHAD_RECEIVE  (1<<2)
#define LWBONEF_ACTIVE          (1<<0)
#define LWBONEF_LIMITEDRANGE    (1<<1)
#define LWLIGHT_DISTANT  0
#define LWLIGHT_POINT    1
#define LWLIGHT_SPOT     2
#define LWLIGHT_LINEAR   3
#define LWLIGHT_AREA     4
#define LWLSHAD_OFF      0
#define LWLSHAD_RAYTRACE 1
#define LWLSHAD_MAP      2
#define LWLFL_LIMITED_RANGE      (1<<0)
#define LWLFL_NO_DIFFUSE         (1<<1)
#define LWLFL_NO_SPECULAR        (1<<2)
#define LWRTYPE_WIRE            0
#define LWRTYPE_QUICK           1
#define LWRTYPE_REALISTIC       2
typedef void *           LWImageID;
typedef void *          MemChunk;
#define LWBUF_SPECIAL    0

#define LWBUF_LUMINOUS   1
#define LWBUF_DIFFUSE    2
#define LWBUF_SPECULAR   3
#define LWBUF_MIRROR     4
#define LWBUF_TRANS      5
#define LWBUF_RAW_RED    6
#define LWBUF_RAW_GREEN  7
#define LWBUF_RAW_BLUE   8

#define LWBUF_SHADING    9
#define LWBUF_SHADOW     10
#define LWBUF_GEOMETRY   11
#define LWBUF_DEPTH      12

#define LWBUF_DIFFSHADE  13
#define LWBUF_SPECSHADE  14

#define LWBUF_RED        32
#define LWBUF_GREEN      33
#define LWBUF_BLUE       34
#define LWBUF_ALPHA      35
typedef struct st_FilterAccess {
	int               width, height;
	LWFrame           frame;
	LWTime            start, end;
	BufferValue *   (*bufLine) (int type, int y);
	float *         (*fltLine) (int type, int y);
	void            (*setRGB)   (int x, int y, BufferValue[3]);
	void            (*setAlpha) (int x, int y, BufferValue);
	Monitor          *monitor;
} FilterAccess;
typedef struct st_ImageFilterHandler {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	void            (*process) (LWInstance,
				    const FilterAccess *);
	unsigned int    (*flags) (LWInstance);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance,
				     const LWItemID *);
} ImageFilterHandler;
struct st_ShaderAccess;
typedef struct st_PixelAccess {
	double            sx, sy;
	void            (*bufVal) (int type, int num, BufferValue *);
	void            (*fltVal) (int type, int num, float *);
	void            (*setRGBA) (BufferValue[4]);
	int             (*illuminate) (LWItemID light,
				       const double position[3],
				       double direction[3],
				       double color[3]);
	double          (*rayTrace) (const double position[3],
				     const double direction[3],
				     double color[3]);
	double          (*rayCast) (const double position[3],
				    const double direction[3]);
	double          (*rayShade) (const double position[3],
				     const double direction[3],
				     struct st_ShaderAccess *);
} PixelAccess;
typedef struct st_PixelFilterHandler {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	LWError         (*init) (LWInstance);
	void            (*cleanup) (LWInstance);
	LWError         (*newTime) (LWInstance, LWFrame, LWTime);
	void            (*evaluate) (LWInstance,
				     const PixelAccess *);
	unsigned int    (*flags) (LWInstance);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance,
				     const LWItemID *);
} PixelFilterHandler;

#define LWPFF_RAYTRACE  (1<<31)
typedef struct st_ShaderAccess {
	int              sx, sy;
	double           oPos[3], wPos[3];
	double           gNorm[3];
	double           spotSize;
	double           raySource[3];
	double           rayLength;
	double           cosine;
	double           oXfrm[9],  wXfrm[9];
	LWItemID         objID;
	int              polNum;
	double           wNorm[3];
	double           color[3];
	double           luminous;
	double           diffuse;
	double           specular;
	double           mirror;
	double           transparency;
	double           eta;
	double           roughness;
	int             (*illuminate) (LWItemID light,
				       const double position[3],
				       double direction[3],
				       double color[3]);
	double          (*rayTrace) (const double position[3],
				     const double direction[3],
				     double color[3]);
	double          (*rayCast) (const double position[3],
				    const double direction[3]);
	double          (*rayShade) (const double position[3],
				     const double direction[3],
				     struct st_ShaderAccess *);
} ShaderAccess;
#define LWSHF_NORMAL    (1<<0)
#define LWSHF_COLOR     (1<<1)
#define LWSHF_LUMINOUS  (1<<2)
#define LWSHF_DIFFUSE   (1<<3)
#define LWSHF_SPECULAR  (1<<4)
#define LWSHF_MIRROR    (1<<5)
#define LWSHF_TRANSP    (1<<6)
#define LWSHF_ETA       (1<<7)
#define LWSHF_ROUGH     (1<<8)
#define LWSHF_RAYTRACE  (1<<10)
typedef struct st_ShaderHandler {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	LWError         (*init) (LWInstance);
	void            (*cleanup) (LWInstance);
	LWError         (*newTime) (LWInstance, LWFrame, LWTime);
	void            (*evaluate) (LWInstance, ShaderAccess *);
	unsigned int    (*flags) (LWInstance);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance,
				     const LWItemID *);
} ShaderHandler;
typedef struct st_DisplacementAccess {
	double           oPos[3];
	double           source[3];
} DisplacementAccess;
typedef struct st_DisplacementHandler {
	LWInstance      (*create) (LWError *, LWItemID);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to,
				 LWItemID);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	LWError         (*newTime) (LWInstance, LWItemID,
				    LWFrame, LWTime);
	void            (*evaluate) (LWInstance,
				     DisplacementAccess *);
	unsigned int    (*flags) (LWInstance);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance,
				     const LWItemID *);
} DisplacementHandler;

#define LWDMF_WORLD             (1<<0)
#define LWDMF_BEFOREBONES       (1<<1)
typedef struct st_DisplacementHandler_V1 {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	LWError         (*newTime) (LWInstance, LWItemID,
				    LWFrame, LWTime);
	void            (*evaluate) (LWInstance,
				     DisplacementAccess *);
	unsigned int    (*flags) (LWInstance);
} DisplacementHandler_V1;
typedef struct st_ItemMotionAccess {
	LWItemID          item;
	LWFrame           frame;
	LWTime            time;
	void            (*getParam) (LWItemParam, LWTime,
				     double vector[3]);
	void            (*setParam) (LWItemParam,
				     const double vector[3]);
} ItemMotionAccess;
typedef struct st_ItemMotionHandler {
	LWInstance      (*create) (LWError *, LWItemID);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to,
				 LWItemID);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	void            (*evaluate) (LWInstance,
				     const ItemMotionAccess *);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance,
				     const LWItemID *);
} ItemMotionHandler;
typedef struct st_ItemMotionHandler_V1 {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	void            (*evaluate) (LWInstance,
				     const ItemMotionAccess *);
} ItemMotionHandler_V1;
#define OBJREP_NONE      0
#define OBJREP_PREVIEW   1
#define OBJREP_RENDER    2
typedef struct st_ObjReplacementAccess {
	LWItemID         objectID;
	LWFrame          curFrame, newFrame;
	LWTime           curTime,  newTime;
	int              curType,  newType;
	const char      *curFilename;
	const char      *newFilename;
} ObjReplacementAccess;
typedef struct st_ObjReplacementHandler {
	LWInstance      (*create) (LWError *, LWItemID);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to,
				 LWItemID);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	void            (*evaluate) (LWInstance,
				     ObjReplacementAccess *);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance,
				     const LWItemID *);
} ObjReplacementHandler;
typedef struct st_ObjReplacementHandler_V1 {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	void            (*evaluate) (LWInstance,
				     ObjReplacementAccess *);
} ObjReplacementHandler_V1;
typedef struct st_FrameBufferHandler {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	LWError         (*open) (LWInstance, int w, int h);
	void            (*close) (LWInstance);
	LWError         (*begin) (LWInstance);
	LWError         (*write) (LWInstance,
				  const BufferValue *R,
				  const BufferValue *G,
				  const BufferValue *B,
				  const BufferValue *alpha);
	void            (*pause) (LWInstance);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance, const LWItemID *);
} FrameBufferHandler;
typedef struct st_AnimSaverHandler {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	LWError         (*open) (LWInstance, int w, int h,
				 const char *filename);
	void            (*close) (LWInstance);
	LWError         (*begin) (LWInstance);
	LWError         (*write) (LWInstance,
				  const BufferValue *R,
				  const BufferValue *G,
				  const BufferValue *B,
				  const BufferValue *alpha);

	const char *    (*descln) (LWInstance);

	const LWItemID* (*useItems) (LWInstance);
	void            (*changeID) (LWInstance, const LWItemID *);
} AnimSaverHandler;
typedef struct st_SceneConverter {
	const char       *filename;
	LWError           readFailure;
	const char       *tmpScene;
	void            (*deleteTmp) (const char *tmpScene);
} SceneConverter;
typedef struct st_LayoutGeneric {
	int             (*saveScene) (const char *file);
	int             (*loadScene) (const char *file,
				      const char *name);
} LayoutGeneric;
typedef struct st_LWItemInfo {
	LWItemID        (*first)  (LWItemType, LWItemID);
	LWItemID        (*next)   (LWItemID);
	LWItemID        (*firstChild) (LWItemID parent);
	LWItemID        (*nextChild) (LWItemID parent, LWItemID prevChild);
	LWItemID        (*parent) (LWItemID);
	LWItemID        (*target) (LWItemID);
	LWItemID        (*goal)   (LWItemID);
	LWItemType      (*type)   (LWItemID);
	const char *    (*name)   (LWItemID);
	void            (*param)  (LWItemID, LWItemParam, LWTime,
				   double vector[3]);
	unsigned int    (*limits) (LWItemID, LWItemParam,
				   double min[3], double max[3]);

	const char *    (*getTag) (LWItemID, int);
	void            (*setTag) (LWItemID, int, const char *);
} LWItemInfo;
typedef struct st_LWObjectInfo {
	const char *    (*filename) (LWItemID);
	int             (*numPoints) (LWItemID);
	int             (*numPolygons) (LWItemID);
	unsigned int    (*shadowOpts) (LWItemID);
	double          (*dissolve) (LWItemID, LWTime);
} LWObjectInfo;
typedef struct st_LWBoneInfo {
	unsigned int    (*flags) (LWItemID);
	void            (*restParam) (LWItemID, LWItemParam,
				      double vector[3]);
	double          (*restLength) (LWItemID);
	void            (*limits) (LWItemID, double *inner,
				   double *outer);
} LWBoneInfo;
typedef struct st_LWLightInfo {
	void            (*ambient) (LWTime, double color[3]);
	int             (*type) (LWItemID);
	void            (*color) (LWItemID, LWTime, double color[3]);
	int             (*shadowType) (LWItemID);
	void            (*coneAngles) (LWItemID, double *radius,
				       double *edge);
	unsigned int    (*flags) (LWItemID);
	double          (*range) (LWItemID);
} LWLightInfo;
typedef struct st_LWCameraInfo {
	double          (*zoomFactor) (LWItemID, LWTime);
	double          (*focalLength) (LWItemID, LWTime);
	double          (*focalDistance) (LWItemID, LWTime);
	double          (*fStop) (LWItemID, LWTime);
	double          (*blurLength) (LWItemID, LWTime);
	void            (*fovAngles) (LWItemID, LWTime,
				      double *horizontal,
				      double *vertical);
} LWCameraInfo;
#define LWROPT_SHADOWTRACE      (1<<0)
#define LWROPT_REFLECTTRACE     (1<<1)
#define LWROPT_REFRACTTRACE     (1<<2)
#define LWROPT_FIELDS           (1<<3)
#define LWROPT_EVENFIELDS       (1<<4)
#define LWROPT_MOTIONBLUR       (1<<5)
#define LWROPT_DEPTHOFFIELD     (1<<6)
#define LWROPT_LIMITEDREGION    (1<<7)
typedef struct st_LWSceneInfo {
	const char      *name;
	const char      *filename;
	int              numPoints;
	int              numPolygons;
	int              renderType;
	int              renderOpts;
	LWFrame          frameStart;
	LWFrame          frameEnd;
	LWFrame          frameStep;
	double           framesPerSecond;
	int              frameWidth;
	int              frameHeight;
	double           pixelAspect;
	int              minSamplesPerPixel;
	int              maxSamplesPerPixel;
	int              limitedRegion[4];      /* x0, y0, x1, y1 */

	int              recursionDepth;
} LWSceneInfo;
typedef struct st_LWImageList {
	LWImageID       (*first) (void);
	LWImageID       (*next) (LWImageID);
	LWImageID       (*load) (const char *);
	const char *    (*name) (LWImageID);
	const char *    (*filename) (LWImageID, LWFrame);
	int             (*isColor) (LWImageID);
	void            (*needAA) (LWImageID);
	void            (*size) (LWImageID, int *w, int *h);
	BufferValue     (*luma) (LWImageID, int x, int y);
	void            (*RGB)  (LWImageID, int x, int y,
				 BufferValue[3]);
	double          (*lumaSpot) (LWImageID, double x, double y,
				     double spotSize, int blend);
	void            (*RGBSpot)  (LWImageID, double x, double y,
				     double spotSize, int blend,
				     double[3]);
	void            (*clear) (LWImageID);
} LWImageList;
typedef struct st_LWCompInfo {
	LWImageID        bg;
	LWImageID        fg;
	LWImageID        fgAlpha;
} LWCompInfo;
typedef struct st_LWBackdropInfo {
	void            (*backdrop) (LWTime, const double ray[3],
				     double color[3]);
} LWBackdropInfo;
#define LWFOG_NONE               0
#define LWFOG_LINEAR             1
#define LWFOG_NONLINEAR1         2
#define LWFOG_NONLINEAR2         3
#define LWFOGF_BACKGROUND       (1<<0)
typedef struct st_LWFogInfo {
	int               type;
	int               flags;
	double          (*minDist) (LWTime);
	double          (*maxDist) (LWTime);
	double          (*minAmt) (LWTime);
	double          (*maxAmt) (LWTime);
	void            (*color) (LWTime, double col[3]);
} LWFogInfo;
typedef struct st_LWEnvelopeHandler {
	LWInstance      (*create) (LWError *);
	void            (*destroy) (LWInstance);
	LWError         (*copy) (LWInstance from, LWInstance to);
	LWError         (*load) (LWInstance, const LWLoadState *);
	LWError         (*save) (LWInstance, const LWSaveState *);

	double          (*evaluate) (LWInstance, LWTime);
	int             (*interface) (LWInstance);
} LWEnvelopeHandler;
typedef struct st_GlobalPool {
	MemChunk        (*first) (void);
	MemChunk        (*next) (MemChunk);
	const char *    (*ID)   (MemChunk);
	int             (*size) (MemChunk);
	MemChunk        (*find)   (const char *ID);
	MemChunk        (*create) (const char *ID, int size);
} GlobalPool;

#endif
