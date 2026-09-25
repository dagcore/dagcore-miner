# Makefile - DagCore Miner
#
#   make                -> build/linux/dagcore-miner      (GPU + CPU, via OpenCL)
#   make cpu            -> build/linux/dagcore-miner-cpu  (without OpenCL)
#   make linux          -> build/linux: both of them
#   make linux-package  -> dist/linux: the Linux kit, with README.md and
#                          SHA256SUMS
#   make windows        -> build/win: dagcore-miner.exe + dagcore-miner-cpu.exe,
#                          cross-compiled with MinGW-w64 (apt install mingw-w64),
#                          and the Windows config.env.example
#   make windows-package -> dist/windows: the folder to copy to Windows, with
#                          SHA256SUMS
#   make release-linux  -> dist/dagcore-miner-<version>-linux-x64.tar.gz
#   make release-windows -> dist/dagcore-miner-<version>-windows-x64.zip
#   make release        -> both archives
#   make warn           -> syntax check with -Wall -Wextra
#   make install        -> binary + kernel in $(PREFIX)/bin, the dashboard in
#                          $(PREFIX)/share/dagcore-miner, config.env.example
#                          in $(SYSCONFDIR). PREFIX defaults to
#                          /opt/dagcore-miner, where install.sh puts it too.
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

# Built into build/linux, never into the source tree, as the Windows pair is
# into build/win. The binaries alone do not run from there - the GPU one looks
# for dagcore_gpu.cl next to itself - so dist/linux is the folder to run or
# ship, and make install takes them from here.
LINUX_BUILD := build/linux
BIN_GPU     := $(LINUX_BUILD)/dagcore-miner
BIN_CPU     := $(LINUX_BUILD)/dagcore-miner-cpu
# Exactly what goes into a Linux release, the counterpart of dist/windows.
LINUX_PKG   := dist/linux

# Windows cross-build. The OpenCL headers are the same ones the Linux build uses;
# they are exposed through a directory of their own because -I/usr/include
# would pull glibc headers into a Windows build. MinGW ships no import library
# for OpenCL.dll, so one is generated from win/OpenCL.def. Everything else is
# linked statically (winpthreads included), so the .exe needs no MinGW DLLs -
# only OpenCL.dll, which the GPU driver installs.
MINGW_CC       ?= x86_64-w64-mingw32-gcc
MINGW_DLLTOOL  ?= x86_64-w64-mingw32-dlltool
OPENCL_HEADERS ?= /usr/include/CL
WIN_BUILD      := build/win
WIN_OPENCL_DEF := win/OpenCL.def
WIN_OPENCL_LIB := $(WIN_BUILD)/libOpenCL.a
WIN_CFLAGS     := -std=gnu11 -pthread -O3 -funroll-loops -Wall
WIN_LDFLAGS    := -static
WIN_LDLIBS     := -lws2_32 -lbcrypt -lpthread -lm
# Built into build/win, never into the source tree; only the package copies them.
BIN_GPU_WIN    := $(WIN_BUILD)/dagcore-miner.exe
BIN_CPU_WIN    := $(WIN_BUILD)/dagcore-miner-cpu.exe
# The example config for Windows is made from the Linux one: same keys, same
# text, with the Linux paths replaced by win/config.env.sed. A Linux path that
# survives fails the build rather than ship a config pointing into /opt.
WIN_CONFIG_EX  := $(WIN_BUILD)/config.env.example
WIN_CONFIG_SED := win/config.env.sed
# Exactly what is unzipped on a new machine, and nothing else.
WIN_PKG        := dist/windows
# The getting-started page sits next to the .exe, where a user who has just
# unzipped the folder sees it first; it takes its style from dashboard/.
README_HTML    := readme.html
# Starts the miner as administrator, which the power limit needs on Windows.
WIN_START_BAT  := win/start.bat
# The Linux kit's getting-started guide, the counterpart of readme.html: plain
# text that reads in a terminal, where a Linux kit is usually unpacked. It
# goes into the kit as README.md.
LINUX_README   := linux/README.md

# Release archives: dist/linux and dist/windows, each in a folder named like
# the archive, so unpacking gives one folder rather than loose files. The
# version is the one the binary reports, read from the source.
VERSION     := $(shell sed -n 's/^\#define DAGCORE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' $(SRC))
REL_LINUX   := dagcore-miner-$(VERSION)-linux-x64
REL_WINDOWS := dagcore-miner-$(VERSION)-windows-x64
REL_STAGE   := dist/.release

# The installer's location, so that a hand "make install" over an installed rig
# replaces what its service runs. Until 1.2.1 it was /usr/local, which nothing
# read on such a rig; PREFIX=/usr/local keeps that layout.
PREFIX   ?= /opt/dagcore-miner
BINDIR   := $(PREFIX)/bin
SHAREDIR := $(PREFIX)/share/dagcore-miner
# The config lives in /etc regardless of PREFIX: it is an operator file, not an
# install payload. Overridable for packagers who want it elsewhere.
SYSCONFDIR ?= /etc/dagcore-miner

.PHONY: all cpu linux linux-package windows windows-package release release-linux \
        release-windows release-version check check-windows warn install uninstall clean help
all: $(BIN_GPU)

# $(KERNEL) is a prerequisite only for consistency: it is not compiled, it is read
# at runtime and handed to clCreateProgramWithSource.
$(BIN_GPU): $(SRC) $(HDR) $(KERNEL)
	@mkdir -p $(LINUX_BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GPU_CPPFLAGS) $< -o $@ $(LDFLAGS) $(GPU_LDLIBS) $(LDLIBS)

cpu: $(BIN_CPU)
$(BIN_CPU): $(SRC) $(HDR)
	@mkdir -p $(LINUX_BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

linux: $(BIN_GPU) $(BIN_CPU)

# The Linux counterpart of windows-package: the same layout, the files the
# miner wants next to itself, and SHA256SUMS over the rest, run from inside
# the folder. The config example needs no rewriting here: its paths are the
# Linux ones, and README.md says which line to change for running from here.
linux-package: linux
	rm -rf $(LINUX_PKG)
	mkdir -p $(LINUX_PKG)/dashboard
	cp $(BIN_GPU) $(BIN_CPU) $(KERNEL) $(CONFIG_EX) $(LINUX_PKG)/
	cp $(LINUX_README) $(LINUX_PKG)/README.md
	cp $(DASHBOARD) $(DASH_OFL) $(LINUX_PKG)/dashboard/
	cd $(LINUX_PKG) && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | \
	    xargs sha256sum > SHA256SUMS
	@echo "linux-package: $(LINUX_PKG) - the Linux kit"

windows: $(BIN_GPU_WIN) $(BIN_CPU_WIN) $(WIN_CONFIG_EX)

$(WIN_CONFIG_EX): $(CONFIG_EX) $(WIN_CONFIG_SED)
	@mkdir -p $(WIN_BUILD)
	sed -f $(WIN_CONFIG_SED) $< > $@.tmp
	@if grep -nE '/etc/|/var/lib|/opt/|/usr/|XDG_|\$$HOME|\.cache/' $@.tmp; then \
	    echo "$@: Linux paths left (above) - update $(WIN_CONFIG_SED)"; \
	    rm -f $@.tmp; exit 1; \
	fi
	mv $@.tmp $@

# Everything a Windows user unzips, in one folder: the miner looks for all of
# it next to the .exe. SHA256SUMS covers every other file in it, with paths
# relative to the folder, so "sha256sum -c SHA256SUMS" runs from inside it.
windows-package: windows
	rm -rf $(WIN_PKG)
	mkdir -p $(WIN_PKG)/dashboard
	cp $(BIN_GPU_WIN) $(BIN_CPU_WIN) $(KERNEL) $(WIN_CONFIG_EX) $(README_HTML) \
	    $(WIN_START_BAT) $(WIN_PKG)/
	cp $(DASHBOARD) $(DASH_OFL) $(WIN_PKG)/dashboard/
	cd $(WIN_PKG) && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | \
	    xargs sha256sum > SHA256SUMS
	@echo "windows-package: $(WIN_PKG) - copy that folder to the Windows machine"

# What a GitHub release attaches. Each archive holds its kit, unchanged, in a
# folder of the archive's name; SHA256SUMS inside still checks from there.
release: release-linux release-windows

release-version:
	@if [ -z "$(VERSION)" ]; then \
	    echo "release: no DAGCORE_VERSION found in $(SRC)"; exit 1; \
	fi

# GNU tar, as on any Linux: numeric root ownership and sorted names, so the
# archive does not carry the build user's name or the directory's order.
release-linux: release-version linux-package
	rm -rf $(REL_STAGE) dist/$(REL_LINUX).tar.gz
	mkdir -p $(REL_STAGE)
	cp -rp $(LINUX_PKG) $(REL_STAGE)/$(REL_LINUX)
	cd $(REL_STAGE) && tar --sort=name --owner=0 --group=0 --numeric-owner \
	    -czf ../$(REL_LINUX).tar.gz $(REL_LINUX)
	rm -rf $(REL_STAGE)
	@echo "release-linux: dist/$(REL_LINUX).tar.gz"

# zip where it is installed (apt install zip), else bsdtar (libarchive-tools
# on Linux; Windows 10 and later ship it as System32\tar.exe, which Git Bash
# hides behind its own GNU tar, so it is called by its full path).
release-windows: release-version windows-package
	rm -rf $(REL_STAGE) dist/$(REL_WINDOWS).zip
	mkdir -p $(REL_STAGE)
	cp -rp $(WIN_PKG) $(REL_STAGE)/$(REL_WINDOWS)
	cd $(REL_STAGE) && \
	if command -v zip >/dev/null 2>&1; then \
	    zip -qrX ../$(REL_WINDOWS).zip $(REL_WINDOWS); \
	elif command -v bsdtar >/dev/null 2>&1; then \
	    bsdtar -a -cf ../$(REL_WINDOWS).zip $(REL_WINDOWS); \
	elif [ -n "$$SYSTEMROOT" ] && command -v cygpath >/dev/null 2>&1 && \
	     [ -x "$$(cygpath -u "$$SYSTEMROOT")/System32/tar.exe" ]; then \
	    "$$(cygpath -u "$$SYSTEMROOT")/System32/tar.exe" -a -cf ../$(REL_WINDOWS).zip $(REL_WINDOWS); \
	else \
	    echo "release-windows: no zip or bsdtar found (apt install zip)"; exit 1; \
	fi
	rm -rf $(REL_STAGE)
	@echo "release-windows: dist/$(REL_WINDOWS).zip"

# Removed first: where ln -s copies instead of linking (Git Bash on Windows
# without symlink rights), a second run - make check forces one - would copy
# the headers into the copy, as include/CL/CL. On Linux it removes the link only.
$(WIN_BUILD)/include/CL:
	@mkdir -p $(WIN_BUILD)/include
	rm -rf $@
	ln -s $(OPENCL_HEADERS) $@

$(WIN_OPENCL_LIB): $(WIN_OPENCL_DEF)
	@mkdir -p $(WIN_BUILD)
	$(MINGW_DLLTOOL) -d $< -l $@ -D OpenCL.dll

$(BIN_GPU_WIN): $(SRC) $(HDR) $(KERNEL) $(WIN_OPENCL_LIB) | $(WIN_BUILD)/include/CL
	$(MINGW_CC) $(WIN_CFLAGS) $(CPPFLAGS) $(GPU_CPPFLAGS) -I$(WIN_BUILD)/include $< -o $@ \
	    $(WIN_LDFLAGS) -L$(WIN_BUILD) -lOpenCL $(WIN_LDLIBS)

$(BIN_CPU_WIN): $(SRC) $(HDR)
	@mkdir -p $(WIN_BUILD)
	$(MINGW_CC) $(WIN_CFLAGS) $(CPPFLAGS) $< -o $@ $(WIN_LDFLAGS) $(WIN_LDLIBS)

# Builds every variant from scratch. Catches breakage that shows up on one path
# only - for example code inside #ifdef DAGTECH_GPU called from outside the guard,
# which links fine with GPU and fails to link without it, or code inside
# #ifndef _WIN32 called from outside it. The Windows pair needs MinGW-w64; without
# it that half is skipped, loudly, rather than failing the Linux check.
check:
	@$(MAKE) --no-print-directory -B $(BIN_GPU)
	@$(MAKE) --no-print-directory -B $(BIN_CPU)
	@echo "check: both Linux variants (GPU + CPU) compile"
	@$(MAKE) --no-print-directory check-windows

check-windows:
	@if command -v $(MINGW_CC) >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory -B $(BIN_GPU_WIN) $(BIN_CPU_WIN) $(WIN_CONFIG_EX) && \
	    echo "check: both Windows variants (GPU + CPU) compile and link"; \
	else \
	    echo "check: SKIPPED Windows variants - $(MINGW_CC) not found (apt install mingw-w64)"; \
	fi

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
	$(RM) $(DESTDIR)$(BINDIR)/$(notdir $(BIN_GPU)) $(DESTDIR)$(BINDIR)/$(KERNEL)
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

# The two root binaries are what builds before build/linux left behind; they
# go too, so an old one is never mistaken for the current build.
clean:
	$(RM) -r $(LINUX_BUILD) $(LINUX_PKG)
	$(RM) -r $(WIN_BUILD) $(WIN_PKG)
	$(RM) -r $(REL_STAGE) dist/dagcore-miner-*.tar.gz dist/dagcore-miner-*.zip
	$(RM) dagcore-miner dagcore-miner-cpu
	-rmdir build dist 2>/dev/null || true

help:
	@printf '%s\n' 'targets: all cpu linux linux-package windows windows-package release release-linux release-windows check warn install uninstall clean' \
	                'vars:  NATIVE=1 DEBUG=1 USE_OPENSSL=1 PREFIX=... SYSCONFDIR=...' \
	                '       MINGW_CC=... OPENCL_HEADERS=... (windows)'
