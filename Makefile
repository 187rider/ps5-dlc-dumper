# ps5-dlc-dumper — build with the ps5-payload-dev SDK
#
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make
#   make test            # build + send to the PS5's elfldr
#
# Override the target console with:
#   make test PS5_HOST=192.168.1.6

PS5_HOST ?= 192.168.1.6
PS5_PORT ?= 9021

# The host-side 'sim' and 'clean' targets don't need the PS5 toolchain.
NEEDS_SDK := 1
ifneq (,$(filter sim clean,$(MAKECMDGOALS)))
    ifeq (,$(filter all test send $(ELF),$(MAKECMDGOALS)))
        NEEDS_SDK := 0
    endif
endif

ifeq ($(NEEDS_SDK),1)
    ifdef PS5_PAYLOAD_SDK
        include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
    else
        $(error PS5_PAYLOAD_SDK is undefined. e.g. export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)
    endif
endif

# Ensure Homebrew LLVM (llvm@18) is on PATH if installed
ifneq ($(wildcard /opt/homebrew/opt/llvm@18/bin),)
    export PATH := /opt/homebrew/opt/llvm@18/bin:$(PATH)
else ifneq ($(wildcard /opt/homebrew/opt/llvm/bin),)
    export PATH := /opt/homebrew/opt/llvm/bin:$(PATH)
endif

ELF    := dlc_dump.elf
CFLAGS := -Wall -Wextra -O2 -g

all: $(ELF) pkg

$(ELF): main.c web.c
	$(CC) $(CFLAGS) -o $@ main.c
	$(STRIP) $@

pkg: $(ELF)
	cp $(ELF) homebrew/DLC-Dumper/eboot.elf
	rm -f DLC-Dumper-homebrew.zip
	(cd homebrew && zip -q -r ../DLC-Dumper-homebrew.zip DLC-Dumper)

clean:
	rm -f $(ELF) sim

# Send the payload to the console.
test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

# Fallback deploy if PS5_DEPLOY misbehaves (macOS nc has no -q0, so use socat).
send: $(ELF)
	socat -t 99999999 - TCP:$(PS5_HOST):$(PS5_PORT) < $(ELF)

# Host-side build that exercises the copy logic against a fake tree in /tmp.
# Nothing to do with the PS5 — just proves the scan/filter/copy code is sane.
sim: main.c web.c
	cc -Wall -Wextra -O2 -DSIM=1 \
	  -DPFSMNT='"/tmp/ps5sim/pfsmnt"' \
	  -DSANDBOX='"/tmp/ps5sim/sandbox"' \
	  -DUSBBASE='"/tmp/ps5sim/usb"' \
	  -o sim main.c -lpthread

.PHONY: all clean test send pkg

