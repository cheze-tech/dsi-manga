# make          -> dsimanga.nds      (standard edition, works in plain DS mode)
# make dsi      -> dsimanga_dsi.nds  (DSi Enhanced edition: page cache, hi-res, double pages, library)
# Needs devkitPro (devkitARM, libnds, libfat), Maxmod + libmad for DSi audio,
# and TJpgDec (tjpgd.c/.h/tjpgdcnf.h) in source/.
ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM, e.g. export DEVKITARM=/opt/devkitpro/devkitARM)
endif
DEVKITPRO ?= /opt/devkitpro
LIBNDS    := $(DEVKITPRO)/libnds
CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
NDSTOOL   := $(DEVKITPRO)/tools/bin/ndstool
SPECS     ?= ds_arm9.specs
AUDIO     ?= 1
ifeq ($(AUDIO),1)
AUDIO_CFLAGS := -DUSE_AUDIO
AUDIO_LIBS   := -lmm9 -lmad
else ifeq ($(AUDIO),0)
AUDIO_CFLAGS :=
AUDIO_LIBS   :=
else
$(error AUDIO must be 0 or 1)
endif
PORTLIBS  ?= $(wildcard $(DEVKITPRO)/portlibs/nds) $(wildcard $(DEVKITPRO)/portlibs/armv5te) $(wildcard $(DEVKITPRO)/portlibs/armv4t) $(wildcard $(DEVKITPRO)/portlibs/arm)
PORTINC   := $(foreach d,$(PORTLIBS),-I$(d)/include)
PORTLIB   := $(foreach d,$(PORTLIBS),-L$(d)/lib)
# DSi-enhanced gamecard. 00030004 is DSiWare and cannot be loaded as a card;
# 80040407 is ndstool's default "allow access to everything" ARM7 SCFG mask.
DSIFLAGS  ?= -u 00030000 -z 80040407
# ARM mode (not Thumb) is noticeably faster for the pixel and JPEG loops on ARM9.
ARCH      := -march=armv5te -mtune=arm946e-s -marm -mthumb-interwork
CFLAGS    := -O2 -Wall $(ARCH) -ffunction-sections -fdata-sections -DARM9 -D__NDS__ -I$(LIBNDS)/include -I$(DEVKITPRO)/calico/include -Isource $(AUDIO_CFLAGS) $(PORTINC)
LDFLAGS   := -specs=$(DEVKITPRO)/calico/share/ds9.specs -g $(ARCH) -Wl,--gc-sections -L$(LIBNDS)/lib -L$(DEVKITPRO)/calico/lib $(PORTLIB)
COMMON    := source/pdf.c source/tjpgd.c

all: dsimanga.nds
dsi: dsimanga_dsi.nds

dsimanga.elf: $(COMMON) source/main.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -lfat -lnds9 -lcalico_ds9 -o $@
dsimanga_dsi.elf: $(COMMON) source/enhanced.c source/audio.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(AUDIO_LIBS) -lfat -lnds9 -lcalico_ds9 -o $@

dsimanga.nds: dsimanga.elf
	$(NDSTOOL) -c $@ -9 $< -7 $(DEVKITPRO)/calico/bin/ds7_maine.elf
dsimanga_dsi.nds: dsimanga_dsi.elf
	$(NDSTOOL) -c $@ -9 $< -7 $(DEVKITPRO)/calico/bin/ds7_maine.elf $(DSIFLAGS)

clean:
	rm -f *.elf *.nds
