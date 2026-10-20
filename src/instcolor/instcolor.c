/*
 * INSTCOLOR.C -- Per-instance colour variation shader for LightWave 3D 5.x.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <safe_pluginio.h>

#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;

typedef struct {
	int polygonsPerInstance;
	int seed;
	int hueRange;        /* degrees, plus or minus */
	int saturationRange; /* percent, plus or minus */
	int valueRange;      /* percent, plus or minus */
	int multiply;
} InstanceColor;

#define INSTCOLOR_OBJECT_VERSION 1


static void *
plugin_alloc(unsigned long size)
{
	unsigned long *memory = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!memory) return 0;
	*memory = size + 4;
	return memory + 1;
}

static void
plugin_free(void *memory)
{
	unsigned long *raw;
	if (!memory) return;
	raw = ((unsigned long *)memory) - 1;
	FreeMem(raw, *raw);
}

static const char *
parse_int(const char *text, int *value)
{
	int negative = 0;
	*value = 0;
	while (*text == ' ') text++;
	if (*text == '-') {
		negative = 1;
		text++;
	}
	while (*text >= '0' && *text <= '9') {
		*value = *value * 10 + (*text - '0');
		text++;
	}
	if (negative) *value = -*value;
	return text;
}

static void
append_int(char *buffer, int *position, int value)
{
	char digits[12];
	int count = 0;
	int i;
	if (value < 0) {
		buffer[(*position)++] = '-';
		value = -value;
	}
	if (value == 0) digits[count++] = '0';
	while (value && count < 11) {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	}
	for (i = count - 1; i >= 0; i--)
		buffer[(*position)++] = digits[i];
	buffer[*position] = '\0';
}

static unsigned int
hash_instance(unsigned int value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

static double
hash_range(unsigned int instance, unsigned int channel, int seed)
{
	unsigned int h = hash_instance(instance ^ ((unsigned int)seed * 0x9e3779b9u)
	                              ^ (channel * 0x85ebca6bu));
	return (double)(h & 0xffffu) / 32767.5 - 1.0;
}

static double
clamp(double value, double minimum, double maximum)
{
	if (value < minimum) return minimum;
	if (value > maximum) return maximum;
	return value;
}

static void
rgb_to_hsv(const double rgb[3], double hsv[3])
{
	double minimum = rgb[0];
	double maximum = rgb[0];
	double delta;
	if (rgb[1] < minimum) minimum = rgb[1];
	if (rgb[2] < minimum) minimum = rgb[2];
	if (rgb[1] > maximum) maximum = rgb[1];
	if (rgb[2] > maximum) maximum = rgb[2];
	delta = maximum - minimum;
	hsv[2] = maximum;
	if (maximum <= 0.0) {
		hsv[0] = 0.0;
		hsv[1] = 0.0;
		return;
	}
	hsv[1] = delta / maximum;
	if (delta <= 0.000001) {
		hsv[0] = 0.0;
		return;
	}
	if (maximum == rgb[0])
		hsv[0] = (rgb[1] - rgb[2]) / delta;
	else if (maximum == rgb[1])
		hsv[0] = 2.0 + (rgb[2] - rgb[0]) / delta;
	else
		hsv[0] = 4.0 + (rgb[0] - rgb[1]) / delta;
	hsv[0] *= 60.0;
	if (hsv[0] < 0.0) hsv[0] += 360.0;
}

static void
hsv_to_rgb(const double hsv[3], double rgb[3])
{
	double hue = hsv[0];
	double saturation = hsv[1];
	double value = hsv[2];
	double c, x, m, fraction;
	int sector;

	while (hue < 0.0) hue += 360.0;
	while (hue >= 360.0) hue -= 360.0;
	c = value * saturation;
	sector = (int)(hue / 60.0);
	fraction = hue / 60.0 - sector;
	if ((sector & 1) == 0)
		x = c * fraction;
	else
		x = c * (1.0 - fraction);
	m = value - c;
	if (sector == 0) { rgb[0] = c; rgb[1] = x; rgb[2] = 0.0; }
	else if (sector == 1) { rgb[0] = x; rgb[1] = c; rgb[2] = 0.0; }
	else if (sector == 2) { rgb[0] = 0.0; rgb[1] = c; rgb[2] = x; }
	else if (sector == 3) { rgb[0] = 0.0; rgb[1] = x; rgb[2] = c; }
	else if (sector == 4) { rgb[0] = x; rgb[1] = 0.0; rgb[2] = c; }
	else { rgb[0] = c; rgb[1] = 0.0; rgb[2] = x; }
	rgb[0] += m;
	rgb[1] += m;
	rgb[2] += m;
}
XCALL_(static LWInstance)
Create(LWError *error)
{
	InstanceColor *instance;
	XCALL_INIT;
	instance = (InstanceColor *)plugin_alloc(sizeof(InstanceColor));
	if (!instance) return 0;
	instance->polygonsPerInstance = 1;
	instance->seed = 1;
	instance->hueRange = 20;
	instance->saturationRange = 15;
	instance->valueRange = 15;
	instance->multiply = 0;
	return instance;
}

XCALL_(static void)
Destroy(InstanceColor *instance)
{
	XCALL_INIT;
	plugin_free(instance);
}

XCALL_(static LWError)
Copy(InstanceColor *from, InstanceColor *to)
{
	XCALL_INIT;
	*to = *from;
	return 0;
}

XCALL_(static LWError)
Load(InstanceColor *instance, const LWLoadState *load)
{
	static char buffer[96];
	const char *text;
	int version;
	XCALL_INIT;
	if (load->ioMode == LWIO_SCENE) {
		if (!spi_read_line(load, buffer, sizeof(buffer))) return 0;
		text = parse_int(buffer, &version);
		if (version != INSTCOLOR_OBJECT_VERSION) return 0;
		text = parse_int(text, &instance->polygonsPerInstance);
		text = parse_int(text, &instance->seed);
		text = parse_int(text, &instance->hueRange);
		text = parse_int(text, &instance->saturationRange);
		text = parse_int(text, &instance->valueRange);
		parse_int(text, &instance->multiply);
	} else {
		if (!spi_read_i32be(load, &version) || version != INSTCOLOR_OBJECT_VERSION) return 0;
		if (!spi_read_i32be(load, &instance->polygonsPerInstance)) return 0;
		if (!spi_read_i32be(load, &instance->seed)) return 0;
		if (!spi_read_i32be(load, &instance->hueRange)) return 0;
		if (!spi_read_i32be(load, &instance->saturationRange)) return 0;
		if (!spi_read_i32be(load, &instance->valueRange)) return 0;
		if (!spi_read_i32be(load, &instance->multiply)) return 0;
	}
	if (instance->polygonsPerInstance < 1) instance->polygonsPerInstance = 1;
	if (instance->hueRange < 0) instance->hueRange = 0;
	if (instance->hueRange > 180) instance->hueRange = 180;
	if (instance->saturationRange < 0) instance->saturationRange = 0;
	if (instance->saturationRange > 100) instance->saturationRange = 100;
	if (instance->valueRange < 0) instance->valueRange = 0;
	if (instance->valueRange > 100) instance->valueRange = 100;
	instance->multiply = instance->multiply ? 1 : 0;
	return 0;
}

XCALL_(static LWError)
Save(InstanceColor *instance, const LWSaveState *save)
{
	static char buffer[96];
	int position = 0;
	XCALL_INIT;
	if (save->ioMode == LWIO_SCENE) {
		append_int(buffer, &position, INSTCOLOR_OBJECT_VERSION);
		buffer[position++] = ' ';
		append_int(buffer, &position, instance->polygonsPerInstance);
		buffer[position++] = ' ';
		append_int(buffer, &position, instance->seed);
		buffer[position++] = ' ';
		append_int(buffer, &position, instance->hueRange);
		buffer[position++] = ' ';
		append_int(buffer, &position, instance->saturationRange);
		buffer[position++] = ' ';
		append_int(buffer, &position, instance->valueRange);
		buffer[position++] = ' ';
		append_int(buffer, &position, instance->multiply);
		buffer[position] = '\0';
		spi_write_line(save, buffer);
	} else {
		spi_write_i32be(save, INSTCOLOR_OBJECT_VERSION);
		spi_write_i32be(save, instance->polygonsPerInstance);
		spi_write_i32be(save, instance->seed);
		spi_write_i32be(save, instance->hueRange);
		spi_write_i32be(save, instance->saturationRange);
		spi_write_i32be(save, instance->valueRange);
		spi_write_i32be(save, instance->multiply);
	}
	return 0;
}

XCALL_(static LWError)
Init(InstanceColor *instance)
{
	XCALL_INIT;
	return 0;
}

XCALL_(static void)
Cleanup(InstanceColor *instance)
{
	XCALL_INIT;
}

XCALL_(static LWError)
NewTime(InstanceColor *instance, LWFrame frame, LWTime time)
{
	XCALL_INIT;
	return 0;
}

XCALL_(static unsigned int)
Flags(InstanceColor *instance)
{
	XCALL_INIT;
	return LWSHF_COLOR;
}

XCALL_(static void)
Evaluate(InstanceColor *instance, ShaderAccess *access)
{
	double hsv[3];
	double varied[3];
	unsigned int instanceNumber;
	XCALL_INIT;
	if (instance->polygonsPerInstance < 1) return;
	instanceNumber = (unsigned int)(access->polNum / instance->polygonsPerInstance);
	rgb_to_hsv(access->color, hsv);
	hsv[0] += hash_range(instanceNumber, 0u, instance->seed) * instance->hueRange;
	hsv[1] = clamp(hsv[1] + hash_range(instanceNumber, 1u, instance->seed)
	               * instance->saturationRange / 100.0, 0.0, 1.0);
	hsv[2] = clamp(hsv[2] + hash_range(instanceNumber, 2u, instance->seed)
	               * instance->valueRange / 100.0, 0.0, 1.0);
	hsv_to_rgb(hsv, varied);
	if (instance->multiply) {
		access->color[0] *= varied[0];
		access->color[1] *= varied[1];
		access->color[2] *= varied[2];
	} else {
		access->color[0] = varied[0];
		access->color[1] = varied[1];
		access->color[2] = varied[2];
	}
}

XCALL_(static const char *)
DescLn(InstanceColor *instance)
{
	static char text[32];
	int position = 0;
	XCALL_INIT;
	append_int(text, &position, instance->polygonsPerInstance);
	text[position++] = ' ';
	text[position++] = 'p';
	text[position++] = 'o';
	text[position++] = 'l';
	text[position++] = 'y';
	text[position++] = 's';
	text[position] = '\0';
	return text;
}

static const char *modeNames[] = { "Replace", "Multiply", 0 };

XCALL_(static int)
Interface(long version, GlobalFunc *global, InstanceColor *instance, void *serverData)
{
	LWPanelFuncs *panels;
	LWPanelID panel;
	LWControl *polygons, *seed, *hue, *saturation, *value, *mode;
	static LWPanControlDesc desc;
	static LWValue ival = { LWT_INTEGER };
	XCALL_INIT;
	if (version != 1) return AFUNC_BADVERSION;
	panels = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);
	if (!panels) {
		MessageFuncs *message = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
		if (message) (*message->info)("InstanceColor", "LWPanelServices is not available.");
		return AFUNC_OK;
	}
	panel = PAN_CREATE(panels, "InstanceColor v" PLUGIN_VERSION);
	if (!panel) return AFUNC_OK;
	polygons = INT_CTL(panels, panel, "Polygons per instance");
	seed = INT_CTL(panels, panel, "Seed");
	hue = SLIDER_CTL(panels, panel, "Hue range +/-", 160, 0, 180);
	saturation = SLIDER_CTL(panels, panel, "Saturation range +/-", 160, 0, 100);
	value = SLIDER_CTL(panels, panel, "Value range +/-", 160, 0, 100);
	mode = POPUP_CTL(panels, panel, "Colour mode", modeNames);
	SET_INT(polygons, instance->polygonsPerInstance);
	SET_INT(seed, instance->seed);
	SET_INT(hue, instance->hueRange);
	SET_INT(saturation, instance->saturationRange);
	SET_INT(value, instance->valueRange);
	SET_INT(mode, instance->multiply);
	if ((*panels->open)(panel, PANF_BLOCKING | PANF_CANCEL)) {
		GET_INT(polygons, instance->polygonsPerInstance);
		GET_INT(seed, instance->seed);
		GET_INT(hue, instance->hueRange);
		GET_INT(saturation, instance->saturationRange);
		GET_INT(value, instance->valueRange);
		GET_INT(mode, instance->multiply);
		if (instance->polygonsPerInstance < 1) instance->polygonsPerInstance = 1;
		if (instance->hueRange < 0) instance->hueRange = 0;
		if (instance->hueRange > 180) instance->hueRange = 180;
		if (instance->saturationRange < 0) instance->saturationRange = 0;
		if (instance->saturationRange > 100) instance->saturationRange = 100;
		if (instance->valueRange < 0) instance->valueRange = 0;
		if (instance->valueRange > 100) instance->valueRange = 100;
		instance->multiply = instance->multiply ? 1 : 0;
	}
	PAN_KILL(panels, panel);
	return AFUNC_OK;
}

XCALL_(int)
Activate(long version, GlobalFunc *global, void *local, void *serverData)
{
	ShaderHandler *handler = (ShaderHandler *)local;
	XCALL_INIT;
	if (version < 1) return AFUNC_BADVERSION;
	handler->create = (void *)Create;
	handler->destroy = (void *)Destroy;
	handler->copy = (void *)Copy;
	handler->load = (void *)Load;
	handler->save = (void *)Save;
	handler->init = (void *)Init;
	handler->cleanup = (void *)Cleanup;
	handler->newTime = (void *)NewTime;
	handler->evaluate = (void *)Evaluate;
	handler->flags = (void *)Flags;
	handler->descln = (void *)DescLn;
	return AFUNC_OK;
}

ServerRecord ServerDesc[] = {
	{ "ShaderHandler", "InstanceColor", (ActivateFunc *)Activate },
	{ "ShaderInterface", "InstanceColor", (ActivateFunc *)Interface },
	{ 0 }
};
