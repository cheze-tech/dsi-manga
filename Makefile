# make          -> dsimanga.nds      (standard edition, works in plain DS mode)
# make dsi      -> dsimanga_dsi.nds  (DSi Enhanced edition: page cache, hi-res, double pages, library)
# Needs devkitPro (devkitARM, libnds, libfat) and TJpgDec (tjpgd.c/.h/tjpgdcnf.h) in source/.
ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM, e.g. export DEVKITARM=/opt/devkitpro/devkitARM)
endif
DEVKITPRO ?= /opt/devkitpro
LIBNDS    := $(DEVKITPRO)/libnds
CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
NDSTOOL   := $(DEVKITPRO)/tools/bin/ndstool
SPECS     ?= ds_arm9.specs
# DSi-mode header flags: from memory of other DSi-mode homebrew Makefiles, UNVERIFIED.
DSIFLAGS  ?= -u 00030004 -z 80040407
# ARM mode (not Thumb) is noticeably faster for the pixel and JPEG loops on ARM9.
ARCH      := -march=armv5te -mtune=arm946e-s -marm -mthumb-interwork
CFLAGS    := -O2 -Wall $(ARCH) -ffunction-sections -fdata-sections -DARM9 -D__NDS__ -I$(LIBNDS)/include -I$(DEVKITPRO)/calico/include -Isource
LDFLAGS   := -specs=$(DEVKITPRO)/calico/share/ds9.specs -g $(ARCH) -Wl,--gc-sections -L$(LIBNDS)/lib -L$(DEVKITPRO)/calico/lib
COMMON    := source/pdf.c source/tjpgd.c

all: dsimanga.nds
dsi: dsimanga_dsi.nds

dsimanga.elf: $(COMMON) source/main.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -lfat -lnds9 -lcalico_ds9 -o $@
dsimanga_dsi.elf: $(COMMON) source/enhanced.c source/audio.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -lfat -lnds9 -lcalico_ds9 -o $@

dsimanga.nds: dsimanga.elf
	$(NDSTOOL) -c $@ -9 $< -7 $(DEVKITPRO)/calico/bin/ds7_maine.elf
dsimanga_dsi.nds: dsimanga_dsi.elf
	$(NDSTOOL) -c $@ -9 $< -7 $(DEVKITPRO)/calico/bin/ds7_maine.elf $(DSIFLAGS)

clean:
	rm -f *.elf *.nds
