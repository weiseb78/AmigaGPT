# MorphOS openssl3.library probe — build shared (default) and optional static control.
#
#   make
#   make static
#   make all
#   make clean

SDK ?= $(HOME)/development/morphos/AmigaSDK-gcc/morphos/sdk
OUTDIR ?= $(CURDIR)/bin
CC := ppc-morphos-gcc

INCLUDES = -I$(SDK)/os-include -I$(SDK)/includestd -I$(SDK)/include -I$(SDK)/ppc-morphos/include
LIBDIRS = -L$(SDK)/ppc-morphos/lib -L$(SDK)/lib -L$(SDK)/ppc-morphos/lib/libnix
GCCLIBDIR = $(SDK)/lib/gcc/ppc-morphos/11.2.0

CFLAGS = -noixemul -D__MORPHOS__ -Wall -O2 $(INCLUDES)
LDFLAGS_COMMON = $(LIBDIRS) -L$(GCCLIBDIR)

SHARED_OUT = $(OUTDIR)/openssl3_probe_shared
STATIC_OUT = $(OUTDIR)/openssl3_probe_static

.PHONY: all shared static clean

all: shared static

shared: $(SHARED_OUT)

static: $(STATIC_OUT)

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(SHARED_OUT): openssl3_probe.c | $(OUTDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_COMMON) -lssl_shared -lcrypto_shared -latomic
	@echo "built $@"
	@strings $@ | grep -E 'xopenssl3\.library|openssl3\.library' | head -5 || true

$(STATIC_OUT): openssl3_probe.c | $(OUTDIR)
	$(CC) $(CFLAGS) -DPROBE_STATIC $< -o $@ $(LDFLAGS_COMMON) -lssl -lcrypto -latomic
	@echo "built $@"
	@strings $@ | grep -E 'xopenssl3\.library|openssl3\.library' | head -5 || echo "(no openssl3.library string — static OK)"

clean:
	rm -rf $(OUTDIR)
