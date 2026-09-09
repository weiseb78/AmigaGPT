# MorphOS TCP connect probe — blocking vs FIONBIO+WaitSelect.
#
#   make -C tools/tcp-connect-probe

SDK ?= $(HOME)/development/morphos/AmigaSDK-gcc/morphos/sdk
OUTDIR ?= $(CURDIR)/bin
CC := ppc-morphos-gcc

INCLUDES = -I$(SDK)/os-include -I$(SDK)/includestd -I$(SDK)/include -I$(SDK)/ppc-morphos/include
LIBDIRS = -L$(SDK)/ppc-morphos/lib -L$(SDK)/lib -L$(SDK)/ppc-morphos/lib/libnix
GCCLIBDIR = $(SDK)/lib/gcc/ppc-morphos/11.2.0

CFLAGS = -noixemul -D__MORPHOS__ -Wall -O2 $(INCLUDES)
LDFLAGS = $(LIBDIRS) -L$(GCCLIBDIR)

OUT = $(OUTDIR)/tcp_connect_probe

.PHONY: all clean

all: $(OUT)

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OUT): tcp_connect_probe.c | $(OUTDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@echo "built $@"
	@ls -la $@

clean:
	rm -rf $(OUTDIR)
