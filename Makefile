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
CPUFLAGS = -m68020
BUILD    = build
PVER     = $(shell cat VERSION)

CFLAGS   = -noixemul $(CPUFLAGS) -O2 -Wall -I$(SDK_INC) -DPLUGIN_VERSION=\"$(PVER)\"
LDFLAGS  = -noixemul -nostartfiles $(CPUFLAGS)
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
	$(AS) $(filter -m680%,$(CPUFLAGS)) -o $@ $<

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

# GeoImport - VideoScape GEO object loader
$(BUILD)/geoimport.o: $(SRC)/geoimport/geoimport.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/geoimport.p: $(BUILD)/geoimport.o sdk $(STUBS)
	$(call build-plugin,$@,$<)

geoimport: $(BUILD)/geoimport.p

# ---- Targets ----

PLUGINS = objswap objmeshswap fresnel pbr lensflare pngsaver pngloader normalmap motion toon geoimport

all: sdk $(PLUGINS)

# ---- FPU variants ----
# Each variant is a full `all` build with its own CPU flags, build directory
# and SDK library (build-<variant>/). 020fpu targets 68020/68030 + 68881/68882.
# 040fpu/060fpu emit fd*/fs* instructions that only the 68040/68060 on-chip
# FPU implements, so they must not be used with a 68881/68882.
FPU_VARIANTS    = 020fpu 040fpu 060fpu
CPUFLAGS_020fpu = -m68020 -m68881
CPUFLAGS_040fpu = -m68040 -m68881
CPUFLAGS_060fpu = -m68060 -m68881

fpu: $(FPU_VARIANTS)

$(FPU_VARIANTS):
	$(MAKE) all CPUFLAGS="$(CPUFLAGS_$@)" BUILD=build-$@ SDK_LIB=build-$@/sdklib

# ---- Release package ----
# LW-Plugins-<version>.lha: Installer script, the Generic build and every FPU
# variant, plus per-plugin docs converted to Latin-1 for Amiga text viewers
# (iconv fails on characters package/latin1.sed does not map).
# Level-1 LHA headers: 7-Zip mis-extracts level-0 paths written by LHa for UNIX.
PKG_ROOT  = LW-Plugins
PKG_DIR   = $(BUILD)/package
PKG_STAGE = $(PKG_DIR)/$(PKG_ROOT)
PKG_LHA   = $(BUILD)/$(PKG_ROOT)-$(PVER).lha

package: all fpu
	rm -rf $(PKG_DIR) $(PKG_LHA)
	mkdir -p $(PKG_STAGE)/Plugins/Generic $(PKG_STAGE)/Docs
	cp $(PLUGINS:%=$(BUILD)/%.p) $(PKG_STAGE)/Plugins/Generic/
	for v in $(FPU_VARIANTS); do \
		mkdir -p $(PKG_STAGE)/Plugins/$$v && \
		cp $(PLUGINS:%=build-$$v/%.p) $(PKG_STAGE)/Plugins/$$v/ || exit 1; \
	done
	for d in Generic $(FPU_VARIANTS); do cp package/icons/drawer.info $(PKG_STAGE)/Plugins/$$d.info || exit 1; done
	sed 's/@VERSION@/$(PVER)/' package/Install | tr -d '\r' > $(PKG_STAGE)/Install
	cp package/icons/Install.info $(PKG_STAGE)/Install.info
	cp package/icons/drawer.info $(PKG_STAGE)/Plugins.info
	cp package/icons/drawer.info $(PKG_STAGE)/Docs.info
	cp package/icons/drawer.info $(PKG_DIR)/$(PKG_ROOT).info
	for p in $(PLUGINS); do \
		tr -d '\r' < src/$$p/README.md | sed -f package/latin1.sed > $(PKG_DIR)/doc.tmp && \
		iconv -f UTF-8 -t ISO-8859-1 $(PKG_DIR)/doc.tmp > $(PKG_STAGE)/Docs/$$p.txt && \
		cp package/icons/doc.info $(PKG_STAGE)/Docs/$$p.txt.info || exit 1; \
	done
	tr -d '\r' < LICENSE > $(PKG_STAGE)/Docs/LICENSE.txt
	cp package/icons/doc.info $(PKG_STAGE)/Docs/LICENSE.txt.info
	rm -f $(PKG_DIR)/doc.tmp
	cd $(PKG_DIR) && lha c1o5 $(CURDIR)/$(PKG_LHA) $(PKG_ROOT) $(PKG_ROOT).info

clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.p $(SDK_LIB)/server.a $(SDK_LIB)/serv_gcc.o
	rm -rf $(addprefix build-,$(FPU_VARIANTS)) $(PKG_DIR) $(BUILD)/$(PKG_ROOT)-*.lha

.PHONY: all sdk $(PLUGINS) clean fpu $(FPU_VARIANTS) package
