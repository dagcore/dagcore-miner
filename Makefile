# Makefile - DagCore Miner
#
#   make                -> dagcore-miner      (GPU + CPU, via OpenCL)
#   make cpu            -> dagcore-miner-cpu  (without OpenCL)
#   make warn           -> syntax check with -Wall -Wextra
#   make install        -> binary + kernel in $(PREFIX)/bin, the dashboard in
#                          $(PREFIX)/share/dagcore-miner, config.env.example
#                          in $(SYSCONFDIR). The installer uses
#                          PREFIX=/opt/dagcore-miner; so must a hand install
#                          over it, or the service keeps running the old files.
#
# Variables: NATIVE=1 (local build, -march=native), DEBUG=1, USE_OPENSSL=1, PREFIX=...

CC      ?= gcc
NATIVE  ?= 0
DEBUG   ?= 0

CFLAGS  := -std=gnu11 -pthread -funroll-loops -Wall
ifeq ($(DEBUG),1)
  CFLAGS  += -O0 -g3 -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
else
  CFLAGS  += -O3
endif

# Off by default: the binary has to run on machines other than the build host.
# NATIVE=1 only for a local build, on the rig that mines with it.
ifeq ($(NATIVE),1)
  CFLAGS += -march=native -mtune=native
endif

# OpenCL: 1.2 is the real target version (the code uses clCreateCommandQueue,
# deprecated after 1.2); without this define the 3.0 headers warn.
GPU_CPPFLAGS := -DDAGTECH_GPU -DCL_TARGET_OPENCL_VERSION=120
GPU_LDLIBS   := -lOpenCL

LDLIBS := -lm
ifeq ($(USE_OPENSSL),1)
  CPPFLAGS += -DUSE_OPENSSL
  LDLIBS   += -lcrypto
endif

SRC       := dagcore_miner.c
HDR       := dagcore_sha256.h
KERNEL    := dagcore_gpu.cl
# The page, the help page, and the assets both of them share.
DASHBOARD := dashboard/index.html dashboard/help.html \
             dashboard/fonts.css dashboard/logo.webp
# IBM Plex Mono is inlined in the page; OFL 1.1 requires the licence to travel with it.
DASH_OFL  := dashboard/OFL.txt
CONFIG_EX := config.env.example

BIN_GPU := dagcore-miner
BIN_CPU := dagcore-miner-cpu

PREFIX   ?= /usr/local
BINDIR   := $(PREFIX)/bin
SHAREDIR := $(PREFIX)/share/dagcore-miner
# The config lives in /etc regardless of PREFIX: it is an operator file, not an
# install payload. Overridable for packagers who want it elsewhere.
SYSCONFDIR ?= /etc/dagcore-miner

.PHONY: all cpu check warn install uninstall clean help
all: $(BIN_GPU)

# $(KERNEL) is a prerequisite only for consistency: it is not compiled, it is read
# at runtime and handed to clCreateProgramWithSource.
$(BIN_GPU): $(SRC) $(HDR) $(KERNEL)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GPU_CPPFLAGS) $< -o $@ $(LDFLAGS) $(GPU_LDLIBS) $(LDLIBS)

cpu: $(BIN_CPU)
$(BIN_CPU): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# Builds both variants from scratch. Catches breakage that shows up on one path
# only - for example code inside #ifdef DAGTECH_GPU called from outside the guard,
# which links fine with GPU and fails to link without it.
check:
	@$(MAKE) --no-print-directory -B $(BIN_GPU)
	@$(MAKE) --no-print-directory -B $(BIN_CPU)
	@echo "check: both variants (GPU + CPU) compile"

# Noisy build, for auditing - not on the default path.
warn: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GPU_CPPFLAGS) -Wall -Wextra -Wshadow -fsyntax-only $<

# The kernel is looked up next to the binary (the path comes from argv[0]), so it
# is installed in the same directory, not in share/. The dashboard, by contrast, is
# passed explicitly through --dashboard-dir, so it lives in share/.
install: $(BIN_GPU)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN_GPU) $(DESTDIR)$(BINDIR)/
	install -m 0644 $(KERNEL)  $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(SHAREDIR)/dashboard
	install -m 0644 $(DASHBOARD) $(DESTDIR)$(SHAREDIR)/dashboard/
	install -m 0644 $(DASH_OFL)  $(DESTDIR)$(SHAREDIR)/dashboard/
	install -d -m 0750 $(DESTDIR)$(SYSCONFDIR)
	install -m 0644 $(CONFIG_EX) $(DESTDIR)$(SYSCONFDIR)/config.env.example
	@if [ -f "$(DESTDIR)$(SYSCONFDIR)/config.env" ]; then \
	    echo "config.env already exists in $(SYSCONFDIR) - left untouched"; \
	else \
	    install -m 0640 $(CONFIG_EX) "$(DESTDIR)$(SYSCONFDIR)/config.env"; \
	    echo "config.env created in $(SYSCONFDIR) - EDIT WALLET before starting"; \
	fi
	@echo "dashboard installed in $(SHAREDIR)/dashboard"
	@echo "start the miner with: --config $(SYSCONFDIR)/config.env"

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(BIN_GPU) $(DESTDIR)$(BINDIR)/$(KERNEL)
	$(RM) $(DESTDIR)$(SHAREDIR)/dashboard/index.html \
	      $(DESTDIR)$(SHAREDIR)/dashboard/help.html \
	      $(DESTDIR)$(SHAREDIR)/dashboard/fonts.css \
	      $(DESTDIR)$(SHAREDIR)/dashboard/logo.webp \
	      $(DESTDIR)$(SHAREDIR)/dashboard/OFL.txt
	$(RM) $(DESTDIR)$(SYSCONFDIR)/config.env.example
	-rmdir $(DESTDIR)$(SHAREDIR)/dashboard $(DESTDIR)$(SHAREDIR) 2>/dev/null
	@# config.env stays: it is the operator's file, not ours.
	@[ -f "$(DESTDIR)$(SYSCONFDIR)/config.env" ] && \
	    echo "kept: $(SYSCONFDIR)/config.env (remove it by hand if you want it gone)" || \
	    rmdir "$(DESTDIR)$(SYSCONFDIR)" 2>/dev/null || true

clean:
	$(RM) $(BIN_GPU) $(BIN_CPU)

help:
	@printf '%s\n' 'targets: all cpu check warn install uninstall clean' \
	                'vars:  NATIVE=1 DEBUG=1 USE_OPENSSL=1 PREFIX=... SYSCONFDIR=...'
