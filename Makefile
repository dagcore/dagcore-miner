# Makefile - DagCore Miner
#
#   make                -> dagcore-miner      (GPU + CPU, via OpenCL)
#   make cpu            -> dagcore-miner-cpu  (fara OpenCL)
#   make warn           -> verificare sintaxa cu -Wall -Wextra
#   make install        -> binar + kernel in $(PREFIX)/bin
#
# Variabile: NATIVE=1 (build local, -march=native), DEBUG=1, USE_OPENSSL=1, PREFIX=...

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

# Implicit off: binarul trebuie sa ruleze si pe alte masini decat cea de build.
# NATIVE=1 doar pentru build local, pe rig-ul pe care mineaza.
ifeq ($(NATIVE),1)
  CFLAGS += -march=native -mtune=native
endif

# OpenCL: 1.2 e versiunea tinta reala (codul foloseste clCreateCommandQueue,
# deprecat dupa 1.2); fara define-ul asta header-ele 3.0 dau warning-uri.
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
DASHBOARD := dashboard/index.html

BIN_GPU := dagcore-miner
BIN_CPU := dagcore-miner-cpu

PREFIX   ?= /usr/local
BINDIR   := $(PREFIX)/bin
SHAREDIR := $(PREFIX)/share/dagcore-miner

.PHONY: all cpu check warn install uninstall clean help
all: $(BIN_GPU)

# $(KERNEL) e prerequisite doar pentru coerenta: nu se compileaza, e citit
# la runtime si dat la clCreateProgramWithSource.
$(BIN_GPU): $(SRC) $(HDR) $(KERNEL)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GPU_CPPFLAGS) $< -o $@ $(LDFLAGS) $(GPU_LDLIBS) $(LDLIBS)

cpu: $(BIN_CPU)
$(BIN_CPU): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# Compileaza ambele variante de la zero. Prinde ruperile care apar doar pe o
# cale - de exemplu cod din #ifdef DAGTECH_GPU apelat din afara guard-ului,
# care leaga bine cu GPU si esueaza la link fara el.
check:
	@$(MAKE) --no-print-directory -B $(BIN_GPU)
	@$(MAKE) --no-print-directory -B $(BIN_CPU)
	@echo "check: ambele variante (GPU + CPU) compileaza"

# Build zgomotos, pentru audit - nu e in calea implicita.
warn: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GPU_CPPFLAGS) -Wall -Wextra -Wshadow -fsyntax-only $<

# Kernelul e cautat langa binar (calea vine din argv[0]), deci se instaleaza
# in acelasi director, nu in share/. Dashboard-ul, in schimb, e dat explicit
# prin --dashboard-dir, deci sta in share/.
install: $(BIN_GPU)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN_GPU) $(DESTDIR)$(BINDIR)/
	install -m 0644 $(KERNEL)  $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(SHAREDIR)/dashboard
	install -m 0644 $(DASHBOARD) $(DESTDIR)$(SHAREDIR)/dashboard/
	@echo "dashboard instalat in $(SHAREDIR)/dashboard"
	@echo "porneste minerul cu: --dashboard-dir $(SHAREDIR)/dashboard"

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(BIN_GPU) $(DESTDIR)$(BINDIR)/$(KERNEL)
	$(RM) $(DESTDIR)$(SHAREDIR)/dashboard/index.html
	-rmdir $(DESTDIR)$(SHAREDIR)/dashboard $(DESTDIR)$(SHAREDIR) 2>/dev/null

clean:
	$(RM) $(BIN_GPU) $(BIN_CPU)

help:
	@printf '%s\n' 'tinte: all cpu check warn install uninstall clean' \
	                'vars:  NATIVE=1 DEBUG=1 USE_OPENSSL=1 PREFIX=...'
