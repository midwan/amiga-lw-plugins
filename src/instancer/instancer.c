/*
 * INSTANCER.C -- Modeler geometry copier for LightWave 3D 5.x.
 *
 * Copies the primary foreground layer onto all background-layer points.
 * The renderer has no native instancing API, so this creates real geometry.
 */

#include <splug.h>
#include <lwmod.h>
#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;

#define DEG_TO_RAD 0.017453292519943295

/* Values persist for the Modeler session. */
static double headingMin = 0.0, headingMax = 360.0;
static double pitchMin = 0.0, pitchMax = 0.0;
static double bankMin = 0.0, bankMax = 0.0;
static double scaleMin = 1.0, scaleMax = 1.0;
static int randomSeed = 1;

typedef struct {
	PntID id;
	double position[3];
	PntID copy;
} SourcePoint;

typedef struct {
	double position[3];
} TargetPoint;

typedef struct {
	MeshEditOp *op;
	SourcePoint *points;
	int pointCount;
	int pointIndex;
	TargetPoint *targets;
	int targetCount;
	int targetIndex;
	PntID *polygonPoints;
	int maxPolygonPoints;
	double target[3];
	double sinHeading, cosHeading;
	double sinPitch, cosPitch;
	double sinBank, cosBank;
	double scale;
	int copiedFaces;
} InstanceWork;
static unsigned int
next_random(unsigned int *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state;
}

static double
random_range(unsigned int *state, double minimum, double maximum)
{
	double unit;
	if (maximum < minimum) {
		double temp = minimum;
		minimum = maximum;
		maximum = temp;
	}
	unit = (double)(next_random(state) & 0x00ffffffu) / 16777215.0;
	return minimum + (maximum - minimum) * unit;
}

/* Self-contained sine/cosine: no libm calls, so no double-return ABI
 * dependency in FPU builds (AGENTS.md FPU notes). Taylor series after
 * range reduction to [-pi/2, pi/2]; sin(x) = x*(1 + x2*(c1 + x2*(...))),
 * cos(x) = 1 + x2*(c1 + x2*(...)). Max error below 1e-9 on that range. */
static double
reduce_angle(double x)
{
	double two_pi = 6.283185307179586;
	double pi = 3.141592653589793;
	int turns;

	if (x > 1000000.0 || x < -1000000.0)
		return 0.0;
	turns = (int)(x / two_pi);
	x = x - (double)turns * two_pi;
	if (x > pi)
		x = x - two_pi;
	else if (x < -pi)
		x = x + two_pi;
	return x;
}
static double
sin_approx(double angle)
{
	static const double c[8] = {
		1.0,
		-0.16666666666666666,
		0.008333333333333333,
		-0.0001984126984126984,
		2.7557319223985893e-06,
		-2.505210838544172e-08,
		1.6059043836821613e-10,
		-7.647163731819816e-13
	};
	double x = reduce_angle(angle);
	double x2;
	double acc;
	int i;

	if (x > 1.5707963267948966)
		x = 3.141592653589793 - x;
	else if (x < -1.5707963267948966)
		x = -3.141592653589793 - x;
	x2 = x * x;
	acc = c[7];
	for (i = 6; i >= 0; i--)
		acc = acc * x2 + c[i];
	return x * acc;
}

static double
cos_approx(double angle)
{
	static const double c[9] = {
		1.0,
		-0.5,
		0.041666666666666664,
		-0.001388888888888889,
		2.48015873015873e-05,
		-2.755731922398589e-07,
		2.08767569878681e-09,
		-1.1470745597729725e-11,
		4.779477332387385e-14
	};
	double x = reduce_angle(angle);
	double x2;
	double acc;
	int negate = 0;
	int i;

	if (x > 1.5707963267948966) {
		x = 3.141592653589793 - x;
		negate = 1;
	} else if (x < -1.5707963267948966) {
		x = -3.141592653589793 - x;
		negate = 1;
	}
	x2 = x * x;
	acc = c[8];
	for (i = 7; i >= 0; i--)
		acc = acc * x2 + c[i];
	return negate ? -acc : acc;
}

static void
append_int(char *buffer, int *pos, int value)
{
	char digits[12];
	int count = 0;
	int i;

	if (value < 0) {
		buffer[(*pos)++] = '-';
		value = -value;
	}
	if (value == 0)
		digits[count++] = '0';
	while (value > 0 && count < 11) {
		digits[count++] = (char)('0' + (value % 10));
		value /= 10;
	}
	for (i = count - 1; i >= 0; i--)
		buffer[(*pos)++] = digits[i];
	buffer[*pos] = '\0';
}

static EDError
collect_target(void *data, const PointInfo *info)
{
	InstanceWork *work = (InstanceWork *)data;
	if (work->targetIndex >= work->targetCount)
		return EDERR_BADARGS;
	work->targets[work->targetIndex].position[0] = info->position[0];
	work->targets[work->targetIndex].position[1] = info->position[1];
	work->targets[work->targetIndex].position[2] = info->position[2];
	work->targetIndex++;
	return EDERR_NONE;
}

static EDError
collect_source(void *data, const PointInfo *info)
{
	InstanceWork *work = (InstanceWork *)data;
	SourcePoint *point;

	if (work->pointIndex >= work->pointCount || !info->userData)
		return EDERR_BADARGS;
	point = &work->points[work->pointIndex];
	point->id = info->pnt;
	point->position[0] = info->position[0];
	point->position[1] = info->position[1];
	point->position[2] = info->position[2];
	*((int *)info->userData) = work->pointIndex;
	work->pointIndex++;
	return EDERR_NONE;
}

static EDError
find_max_polygon(void *data, const PolygonInfo *info)
{
	InstanceWork *work = (InstanceWork *)data;
	if (info->numPnts > work->maxPolygonPoints)
		work->maxPolygonPoints = info->numPnts;
	return EDERR_NONE;
}

static void
transform_point(InstanceWork *work, const double source[3], double destination[3])
{
	double x = source[0] * work->scale;
	double y = source[1] * work->scale;
	double z = source[2] * work->scale;
	double nextX, nextY, nextZ;

	/* Pitch is X, heading is Y, bank is Z (the Modeler H/P/B convention). */
	nextY = y * work->cosPitch - z * work->sinPitch;
	nextZ = y * work->sinPitch + z * work->cosPitch;
	y = nextY;
	z = nextZ;
	nextX = x * work->cosHeading + z * work->sinHeading;
	nextZ = -x * work->sinHeading + z * work->cosHeading;
	x = nextX;
	z = nextZ;
	nextX = x * work->cosBank - y * work->sinBank;
	nextY = x * work->sinBank + y * work->cosBank;
	destination[0] = nextX + work->target[0];
	destination[1] = nextY + work->target[1];
	destination[2] = z + work->target[2];
}

static EDError
copy_polygon(void *data, const PolygonInfo *info)
{
	InstanceWork *work = (InstanceWork *)data;
	PolID copied;
	int i;

	/* MeshEditOp cannot create a detail polygon or set PPDF_DETAIL. */
	if (info->flags & PPDF_DETAIL)
		return EDERR_NONE;
	if (info->numPnts > work->maxPolygonPoints)
		return EDERR_BADARGS;

	for (i = 0; i < info->numPnts; i++) {
		PointInfo *sourceInfo = (*work->op->pointInfo)(work->op->state,
		                                               info->points[i]);
		int pointIndex;
		if (!sourceInfo || !sourceInfo->userData)
			return EDERR_BADARGS;
		pointIndex = *((int *)sourceInfo->userData);
		if (pointIndex < 0 || pointIndex >= work->pointCount)
			return EDERR_BADARGS;
		work->polygonPoints[i] = work->points[pointIndex].copy;
	}

	if (info->flags & PPDF_CURVE) {
		copied = (*work->op->addCurve)(work->op->state, info->surface,
		                               info->numPnts, work->polygonPoints,
		                               info->flags & (PPDF_CCSTART | PPDF_CCEND));
	} else {
		copied = (*work->op->addPoly)(work->op->state, info->surface,
		                              info->numPnts, work->polygonPoints);
	}
	if (!copied)
		return EDERR_NOMEMORY;
	if (!(info->flags & PPDF_CURVE))
		work->copiedFaces++;
	return EDERR_NONE;
}
static EDError
copy_instance(InstanceWork *work, unsigned int *state)
{
	int i;
	double xyz[3];
	double heading, pitch, bank;

	heading = random_range(state, headingMin, headingMax) * DEG_TO_RAD;
	pitch = random_range(state, pitchMin, pitchMax) * DEG_TO_RAD;
	bank = random_range(state, bankMin, bankMax) * DEG_TO_RAD;
	work->scale = random_range(state, scaleMin, scaleMax);
	work->sinHeading = sin_approx(heading);
	work->cosHeading = cos_approx(heading);
	work->sinPitch = sin_approx(pitch);
	work->cosPitch = cos_approx(pitch);
	work->sinBank = sin_approx(bank);
	work->cosBank = cos_approx(bank);

	for (i = 0; i < work->pointCount; i++) {
		transform_point(work, work->points[i].position, xyz);
		work->points[i].copy = (*work->op->addPoint)(work->op->state, xyz);
		if (!work->points[i].copy)
			return EDERR_NOMEMORY;
	}

	return (*work->op->polyScan)(work->op->state, copy_polygon, work,
	                             OPLYR_PRIMARY);
}

static int
post_options(GlobalFunc *global)
{
	DynaReqFuncs *request;
	DynaRequestID dialog;
	DyReqControlDesc desc;
	DynaValue value;
	int controls[11];
	int i;
	static const char *labels[11] = {
		"Heading minimum", "Heading maximum", "Pitch minimum", "Pitch maximum",
		"Bank minimum", "Bank maximum", "Scale minimum", "Scale maximum",
		"Random seed", "", ""
	};

	request = (DynaReqFuncs *)(*global)("LWM: Dynamic Request", GFUSE_TRANSIENT);
	if (!request)
		return 0;
	dialog = (*request->create)("Instancer");
	if (!dialog)
		return 0;
	desc.type = DY_FLOAT;
	for (i = 0; i < 8; i++) {
		controls[i] = (*request->addCtrl)(dialog, labels[i], &desc);
		if (controls[i] < 0) {
			(*request->destroy)(dialog);
			return 0;
		}
	}
	desc.type = DY_INTEGER;
	controls[8] = (*request->addCtrl)(dialog, labels[8], &desc);
	if (controls[8] < 0) {
		(*request->destroy)(dialog);
		return 0;
	}

	value.type = DY_FLOAT;
	value.flt.value = headingMin; value.flt.defVal = headingMin;
	(*request->valueSet)(dialog, controls[0], &value);
	value.flt.value = headingMax; value.flt.defVal = headingMax;
	(*request->valueSet)(dialog, controls[1], &value);
	value.flt.value = pitchMin; value.flt.defVal = pitchMin;
	(*request->valueSet)(dialog, controls[2], &value);
	value.flt.value = pitchMax; value.flt.defVal = pitchMax;
	(*request->valueSet)(dialog, controls[3], &value);
	value.flt.value = bankMin; value.flt.defVal = bankMin;
	(*request->valueSet)(dialog, controls[4], &value);
	value.flt.value = bankMax; value.flt.defVal = bankMax;
	(*request->valueSet)(dialog, controls[5], &value);
	value.flt.value = scaleMin; value.flt.defVal = scaleMin;
	(*request->valueSet)(dialog, controls[6], &value);
	value.flt.value = scaleMax; value.flt.defVal = scaleMax;
	(*request->valueSet)(dialog, controls[7], &value);
	value.type = DY_INTEGER;
	value.intv.value = randomSeed; value.intv.defVal = randomSeed;
	(*request->valueSet)(dialog, controls[8], &value);

	if (!(*request->post)(dialog)) {
		(*request->destroy)(dialog);
		return 0;
	}
	value.type = DY_FLOAT;
	(*request->valueGet)(dialog, controls[0], &value); headingMin = value.flt.value;
	(*request->valueGet)(dialog, controls[1], &value); headingMax = value.flt.value;
	(*request->valueGet)(dialog, controls[2], &value); pitchMin = value.flt.value;
	(*request->valueGet)(dialog, controls[3], &value); pitchMax = value.flt.value;
	(*request->valueGet)(dialog, controls[4], &value); bankMin = value.flt.value;
	(*request->valueGet)(dialog, controls[5], &value); bankMax = value.flt.value;
	(*request->valueGet)(dialog, controls[6], &value); scaleMin = value.flt.value;
	(*request->valueGet)(dialog, controls[7], &value); scaleMax = value.flt.value;
	value.type = DY_INTEGER;
	(*request->valueGet)(dialog, controls[8], &value); randomSeed = value.intv.value;
	(*request->destroy)(dialog);
	return 1;
}

static EDError
run_instancer(GlobalFunc *global, MeshEditBegin *local, int quiet)
{
	MeshEditOp *op;
	static InstanceWork work;
	static char report[96];
	MessageFuncs *message;
	EDError error = EDERR_NONE;
	unsigned int state;
	int sourcePolygons;
	int i;
	int reportPos = 0;

	message = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!message)
		return EDERR_BADARGS;
	if (!quiet && !post_options(global))
		return EDERR_NONE;

	op = (*local)(sizeof(int), 0, OPSEL_GLOBAL);
	if (!op)
		return EDERR_NONE;
	work.op = op;
	work.pointCount = (*op->pointCount)(op->state, OPLYR_PRIMARY, EDCOUNT_ALL);
	work.targetCount = (*op->pointCount)(op->state, OPLYR_BG, EDCOUNT_ALL);
	work.points = 0;
	work.targets = 0;
	work.polygonPoints = 0;
	work.pointIndex = 0;
	work.targetIndex = 0;
	work.maxPolygonPoints = 0;
	work.copiedFaces = 0;
	if (work.pointCount < 1 || work.targetCount < 1) {
		(*op->done)(op->state, EDERR_NONE, 0);
		if (!quiet)
			(*message->warning)("Instancer", "Put source geometry in the primary foreground layer and target points in background layers.");
		return EDERR_NONE;
	}

	work.points = (SourcePoint *)AllocMem((unsigned long)work.pointCount * sizeof(SourcePoint),
	                                      MEMF_PUBLIC | MEMF_CLEAR);
	work.targets = (TargetPoint *)AllocMem((unsigned long)work.targetCount * sizeof(TargetPoint),
	                                       MEMF_PUBLIC | MEMF_CLEAR);
	if (!work.points || !work.targets) {
		error = EDERR_NOMEMORY;
		goto finish;
	}
	error = (*op->pointScan)(op->state, collect_source, &work, OPLYR_PRIMARY);
	if (error) goto finish;
	error = (*op->pointScan)(op->state, collect_target, &work, OPLYR_BG);
	if (error) goto finish;
	error = (*op->polyScan)(op->state, find_max_polygon, &work, OPLYR_PRIMARY);
	if (error) goto finish;
	if (work.maxPolygonPoints > 0) {
		work.polygonPoints = (PntID *)AllocMem((unsigned long)work.maxPolygonPoints * sizeof(PntID),
		                                      MEMF_PUBLIC | MEMF_CLEAR);
		if (!work.polygonPoints) {
			error = EDERR_NOMEMORY;
			goto finish;
		}
	}

	state = (unsigned int)randomSeed;
	for (i = 0; i < work.targetCount; i++) {
		work.target[0] = work.targets[i].position[0];
		work.target[1] = work.targets[i].position[1];
		work.target[2] = work.targets[i].position[2];
		error = copy_instance(&work, &state);
		if (error) goto finish;
	}

	sourcePolygons = work.targetCount ? work.copiedFaces / work.targetCount : 0;
	if (!quiet)
		(*message->info)("Instancer", "Instances completed. See details in the next message.");
	report[0] = '\0';
	append_int(report, &reportPos, work.targetCount);
	{
		const char *text = " instances, ";
		for (i = 0; text[i]; i++) report[reportPos++] = text[i];
	}
	append_int(report, &reportPos, sourcePolygons);
	{
		const char *text = " polygons per instance.";
		for (i = 0; text[i]; i++) report[reportPos++] = text[i];
	}
	report[reportPos] = '\0';
	if (!quiet)
		(*message->info)("Instancer", report);

finish:
	if (work.polygonPoints)
		FreeMem(work.polygonPoints, (unsigned long)work.maxPolygonPoints * sizeof(PntID));
	if (work.targets)
		FreeMem(work.targets, (unsigned long)work.targetCount * sizeof(TargetPoint));
	if (work.points)
		FreeMem(work.points, (unsigned long)work.pointCount * sizeof(SourcePoint));
	(*op->done)(op->state, error, 0);
	if (!quiet && error)
			(*message->error)("Instancer", "Could not create the instances; no changes were made.");
	return error;
}


/* Batch argument: nine numbers override the remembered settings in the
 * order hmin hmax pmin pmax bmin bmax smin smax seed. Anything else (or
 * nothing) keeps the settings from the last interactive run. */
static const char *
parse_double(const char *text, double *value, int *found)
{
	double number = 0.0, scale = 0.1;
	int negative = 0, digits = 0, dot = 0;

	while (*text == ' ') text++;
	if (*text == '-') { negative = 1; text++; }
	while ((*text >= '0' && *text <= '9') || *text == '.') {
		if (*text == '.') {
			if (dot) break;
			dot = 1;
		} else if (!dot) {
			number = number * 10.0 + (*text - '0');
			digits = 1;
		} else {
			number += (*text - '0') * scale;
			scale *= 0.1;
			digits = 1;
		}
		text++;
	}
	if (negative) number = -number;
	*found = digits;
	*value = number;
	return text;
}

static void
apply_batch_argument(const char *argument)
{
	double v[9];
	int found = 0, i;

	if (!argument) return;
	for (i = 0; i < 9; i++) {
		argument = parse_double(argument, &v[i], &found);
		if (!found) return;
	}
	headingMin = v[0]; headingMax = v[1];
	pitchMin = v[2]; pitchMax = v[3];
	bankMin = v[4]; bankMax = v[5];
	scaleMin = v[6]; scaleMax = v[7];
	randomSeed = (int)v[8];
}

XCALL_(static int)
CmdActivate(long version, GlobalFunc *global, ModCommand *local, void *serverData)
{
	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;
	if (!local || !local->editBegin)
		return AFUNC_BADLOCAL;
	/* An argument selects unattended use: the settings dialog is skipped.
	 * Nine numbers in the argument also override the rotation, scale and
	 * seed settings (see apply_batch_argument). */
	if (local->argument && local->argument[0])
		apply_batch_argument(local->argument);
	run_instancer(global, local->editBegin,
	              local->argument && local->argument[0]);
	return AFUNC_OK;
}

XCALL_(int)
Activate(long version, GlobalFunc *global, MeshEditBegin *local, void *serverData)
{
	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;
	run_instancer(global, local, 0);
	return AFUNC_OK;
}

ServerRecord ServerDesc[] = {
	{ "MeshDataEdit",    "Instancer", (ActivateFunc *)Activate },
	{ "CommandSequence", "InstancerBatch", (ActivateFunc *)CmdActivate },
	{ 0 }
};
