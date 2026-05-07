#
# Makefile for LightWave plugins using m68k-amigaos-gcc
# Run inside Docker: sacredbanana/amiga-compiler:m68k-amigaos
#

.DEFAULT_GOAL := all

CC       = m68k-amigaos-gcc
AS       = m68k-amigaos-as
AR       = m68k-amigaos-ar

SDK_INC  = sdk/include
SDK_SRC  = sdk/source
SDK_LIB  = sdk/lib

SRC      = src
BUILD    = build
PVER     = $(shell cat VERSION)

CFLAGS   = -noixemul -m68020 -O2 -Wall -I$(SDK_INC) -DPLUGIN_VERSION=\"$(PVER)\"
LDFLAGS  = -noixemul -nostartfiles -m68020
LIBS     = $(SDK_LIB)/server.a -lm -lgcc

STARTUP  = $(SDK_LIB)/serv_gcc.o
STUBS    = $(BUILD)/stubs.o

# ---- SDK library build ----

SLIB_OBJS = $(BUILD)/slib1.o $(BUILD)/slib2.o $(BUILD)/slib3.o $(BUILD)/slib4.o

$(BUILD):
	mkdir -p $(BUILD)

$(SDK_LIB):
	mkdir -p $(SDK_LIB)

$(BUILD)/serv_gcc.o: $(SDK_SRC)/serv_gcc.s | $(BUILD)
	$(AS) -m68020 -o $@ $<

$(BUILD)/slib%.o: $(SDK_SRC)/slib%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/stubs.o: $(SDK_SRC)/stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SDK_LIB)/server.a: $(SLIB_OBJS) | $(SDK_LIB)
	$(AR) rcs $@ $^

$(SDK_LIB)/serv_gcc.o: $(BUILD)/serv_gcc.o | $(SDK_LIB)
	cp $< $@

sdk: $(SDK_LIB)/server.a $(SDK_LIB)/serv_gcc.o $(STUBS)

# ---- Plugin build rule ----
# Usage: $(call build-plugin,output.p,source.o [source2.o ...])
define build-plugin
$(CC) $(LDFLAGS) -o $(1) $(STARTUP) $(2) $(STUBS) $(LIBS)
endef

# ---- Plugins ----

# ObjSwap - Object replacement by frame number
$(BUILD)/objswap.o: $(SRC)/objswap/objswap.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/objswap.p: $(BUILD)/objswap.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

objswap: $(BUILD)/objswap.p

# ObjMeshSwap - Object replacement preserving base surfaces
$(BUILD)/objmeshswap.o: $(SRC)/objmeshswap/objmeshswap.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/objmeshswap.p: $(BUILD)/objmeshswap.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

objmeshswap: $(BUILD)/objmeshswap.p

# Fresnel - Physically-based Fresnel shader
$(BUILD)/fresnel.o: $(SRC)/fresnel/fresnel.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/fresnel.p: $(BUILD)/fresnel.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

fresnel: $(BUILD)/fresnel.p

# PBR - Combined PBR-lite shader
$(BUILD)/pbr.o: $(SRC)/pbr/pbr.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/pbr.p: $(BUILD)/pbr.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

pbr: $(BUILD)/pbr.p

# LensFlare - Specular lens flare image filter
$(BUILD)/lensflare.o: $(SRC)/lensflare/lensflare.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/lensflare.p: $(BUILD)/lensflare.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

lensflare: $(BUILD)/lensflare.p

# PNGsaver - PNG image saver
$(BUILD)/pngsaver.o: $(SRC)/pngsaver/pngsaver.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/pngsaver.p: $(BUILD)/pngsaver.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

pngsaver: $(BUILD)/pngsaver.p

# PNGloader - PNG image loader
$(BUILD)/pngloader.o: $(SRC)/pngloader/pngloader.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/pngloader.p: $(BUILD)/pngloader.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

pngloader: $(BUILD)/pngloader.p

# NormalMap - Normal map texture shader
$(BUILD)/normalmap.o: $(SRC)/normalmap/normalmap.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/normalmap.p: $(BUILD)/normalmap.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

normalmap: $(BUILD)/normalmap.p

# Motion - Procedural motion (wiggle/bounce/shake)
$(BUILD)/motion.o: $(SRC)/motion/motion.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/motion.p: $(BUILD)/motion.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

motion: $(BUILD)/motion.p

# Toon - Cel-shading image filter
$(BUILD)/toon.o: $(SRC)/toon/toon.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/toon.p: $(BUILD)/toon.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

toon: $(BUILD)/toon.p

# ---- Targets ----

all: sdk objswap objmeshswap fresnel pbr lensflare pngsaver pngloader normalmap motion toon

clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.p $(SDK_LIB)/server.a $(SDK_LIB)/serv_gcc.o

.PHONY: all sdk objswap objmeshswap fresnel pbr lensflare pngsaver pngloader normalmap motion toon clean
