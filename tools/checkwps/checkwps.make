#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/

ENGLISH := english

ifdef WAVEFORM_BUILD
WAVEFORM_LANG_DIR ?= $(BUILDDIR)/generated/lang
LANG_GEN_H := $(WAVEFORM_LANG_DIR)/lang.h
LANG_GEN_C := $(WAVEFORM_LANG_DIR)/lang_core.c
LANG_ENUM_H := $(WAVEFORM_LANG_DIR)/lang_enum.h
else
LANG_GEN_H := $(BUILDDIR)/lang/lang.h
LANG_GEN_C := $(BUILDDIR)/lang/lang_core.c
LANG_ENUM_H := $(BUILDDIR)/lang_enum.h
endif

# Use global GCCOPTS
GCCOPTS += -D__PCTOOL__ -DCHECKWPS

CHECKWPS_SRC = $(call preprocess, $(TOOLSDIR)/checkwps/SOURCES)
CHECKWPS_OBJ = $(call c2obj,$(CHECKWPS_SRC)) $(BUILDDIR)/lang/lang_core.o

OTHER_SRC += $(CHECKWPS_SRC)

INCLUDES = -I$(ROOTDIR)/apps/gui \
           -I$(ROOTDIR)/apps/gui/skin_engine \
           -I$(FIRMDIR)/kernel/include \
           -I$(ROOTDIR)/firmware/export \
           -I$(ROOTDIR)/firmware/include \
           -I$(ROOTDIR)/firmware/target/hosted \
           -I$(ROOTDIR)/firmware/target/hosted/sdl \
           -I$(ROOTDIR)/apps \
           -I$(ROOTDIR)/apps/recorder \
           -I$(ROOTDIR)/apps/radio \
           -I$(ROOTDIR)/lib/rbcodec \
           -I$(ROOTDIR)/lib/rbcodec/metadata \
           -I$(ROOTDIR)/lib/rbcodec/dsp \
           -I$(APPSDIR) \
           -I$(BUILDDIR) \
           -I$(BUILDDIR)/lang \
           $(TARGET_INC)
ifdef WAVEFORM_BUILD
INCLUDES += -I$(WAVEFORM_LANG_DIR)
endif

.SECONDEXPANSION: # $$(OBJ) is not populated until after this

$(BUILDDIR)/$(BINARY): $$(CHECKWPS_OBJ) $$(CORE_LIBS)
	@echo LD $(BINARY)
	$(SILENT)$(HOSTCC) -o $@ $+ $(INCLUDE) $(GCCOPTS)  \
	-L$(BUILDDIR)/lib $(call a2lnk,$(CORE_LIBS))

$(BUILDDIR)/fontbundle.h: $(ROOTDIR)/fonts/*bdf
	@echo FONTBUNDLE
	$(SILENT)echo "static unsigned char* bundledfonts[] = {" > $@
	$(SILENT)ls $(ROOTDIR)/fonts/*bdf | perl -pne 's|.*/(\d+-.*)\.bdf|  "$$1",|;' >> $@
	$(SILENT)echo "  NULL, " >> $@
	$(SILENT)echo "};" >> $@

#### Everything below is hacked in from apps.make and lang.make

$(BUILDDIR)/apps/features: $(ROOTDIR)/apps/features.txt
	$(SILENT)mkdir -p $(BUILDDIR)/apps
	$(SILENT)mkdir -p $(BUILDDIR)/lang
	$(call PRINTS,PP $(<F))
	$(SILENT)$(CC) $(PPCFLAGS) \
		-E -P -imacros "config.h" -imacros "button.h" -x c $< | \
	grep -v "^#" | grep -v "^ *$$" > $(BUILDDIR)/apps/features; \

$(BUILDDIR)/apps/genlang-features:  $(BUILDDIR)/apps/features
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))tr \\n : < $< > $@

ifneq ($(WAVEFORM_BUILD),1)
$(BUILDDIR)/lang_enum.h: $(BUILDDIR)/lang/lang.h $(TOOLSDIR)/genlang

$(BUILDDIR)/lang/lang.h: $(ROOTDIR)/apps/lang/$(ENGLISH).lang $(BUILDDIR)/apps/features $(TOOLSDIR)/genlang $(BUILDDIR)/apps/genlang-features
	$(call PRINTS,GEN lang.h)
	$(SILENT)$(TOOLSDIR)/genlang -e=$(ROOTDIR)/apps/lang/$(ENGLISH).lang -p=$(BUILDDIR)/lang -t=$(MODELNAME):`cat $(BUILDDIR)/apps/genlang-features` $<

$(BUILDDIR)/lang/lang_core.c: $(BUILDDIR)/lang/lang.h $(TOOLSDIR)/genlang
else
$(LANG_GEN_H) $(LANG_GEN_C) $(LANG_ENUM_H):
	$(SILENT)echo "*** Missing Waveform-generated language artifacts."
	$(SILENT)echo "*** Run: python tools/waveform_build.py generate"
	$(SILENT)exit 1
endif

$(BUILDDIR)/lang/lang_core.o: $(LANG_GEN_H) $(LANG_GEN_C)
	$(call PRINTS,CC lang_core.c)$(CC) $(CFLAGS) -c $(LANG_GEN_C) -o $@

$(BUILDDIR)/lang/max_language_size.h: $(LANG_GEN_H)
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))
	$(SILENT)echo "#define MAX_LANGUAGE_SIZE 131072" > $@
