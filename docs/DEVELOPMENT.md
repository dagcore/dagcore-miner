# Working on the source

- [How it is put together](#how-it-is-put-together)
- [Building](#building)
- [Changes from DagTech Miner](#changes-from-dagtech-miner)
- [Traps](#traps)
- [Not done yet](#not-done-yet)

## How it is put together

| File | What |
|------|------|
| `dagcore_miner.c` | The whole host program. One translation unit. |
| `dagcore_gpu.cl` | The OpenCL kernel, read from disk and compiled at runtime. |
| `dagcore_sha256.h` | SHA-256, header-only. Used unless `USE_OPENSSL`. |
| `dashboard/` | `index.html`, `help.html`, shared `fonts.css` and `logo.webp`. |
| `install.sh`, `uninstall.sh` | Packaging. |

**One `.c` file.** Around 5500 lines, no build system beyond a Makefile, no
dependency except libc, pthreads and `libOpenCL`. Inherited from DagTech Miner
and kept: a miner is one long-running process with a handful of threads, and
splitting it would add build machinery without making anything clearer. Stratum
is spoken over raw sockets with hand-rolled JSON; there is no libcurl and no
JSON library.

**The kernel is not compiled at build time.** `dagcore_gpu.cl` is read at
startup from the directory the binary sits in — the path comes from `argv[0]` —
and handed to `clCreateProgramWithSource` with `-cl-std=CL1.2`. So the binary
and the `.cl` file must be deployed together; replacing only the binary gives
`Cannot open kernel` at startup.

**Why OpenCL and not CUDA.** OpenCL compiles the kernel for whatever device the
driver presents, at the moment it runs. There is no `sm_XX` to target, no fat
binary, no rebuild when a new architecture appears — the same binary ran on an
RTX 3080 and an RTX 4090 without being touched. It would also run on AMD, though
nobody has tried. CUDA would mean compiling per architecture and shipping a
matrix of binaries, for an algorithm that is memory-bound and would gain nothing
from CUDA's arithmetic tooling.

**NVML is opened with `dlopen`, never linked.** Clock offsets and locks go
through `libnvidia-ml.so.1` at runtime. The miner has to build and run on a
machine with no NVIDIA driver at all, and the CPU-only build must not grow a
dependency on it. `DAGCORE_NVML_LIB` redirects the library, which is how the
NVML paths are tested without a card.

## Building

```sh
make            # GPU build -> dagcore-miner
make cpu        # CPU-only  -> dagcore-miner-cpu
make check      # builds every variant, from scratch (Windows too, if MinGW is there)
make windows    # cross-compiled .exe pair, needs MinGW-w64
make warn       # -Wall -Wextra -Wshadow, syntax only
make install    # into /opt/dagcore-miner, as the installer does
```

**Always `make check` before committing.** The two builds take different paths
through `#ifdef DAGTECH_GPU`, and code reachable from one but defined in the
other links fine in the GPU build and fails only in the CPU one. That has
happened. The same goes for `#ifndef _WIN32`, which is why `make check` also
builds the Windows pair; without MinGW-w64 it says `SKIPPED` rather than
failing.

**Windows cross-build.** `make windows` builds `dagcore-miner.exe` and
`dagcore-miner-cpu.exe` with MinGW-w64 (`apt install mingw-w64`). It reuses
the OpenCL headers of the Linux build (`OPENCL_HEADERS`, default
`/usr/include/CL`) and generates an import library for `OpenCL.dll` from
`win/OpenCL.def`; a new `cl*` call has to be added there. Everything else is
linked statically, so the GPU `.exe` needs only system DLLs and `OpenCL.dll`,
which the NVIDIA driver installs. Nothing here can run it: there is no Wine
and no Windows machine on the build rig.

`make windows` also writes `build/win/config.env.example`, made from the Linux
`config.env.example` by `win/config.env.sed`: the same keys and text, with the
Linux paths (`/etc`, `/var/lib`, the XDG cache, the `/opt` `DASHBOARD_DIR`)
replaced by what the portable layout does. If a Linux path survives - someone
reworded a line the sed script matches - the build stops and names the line;
fix the script, not the output. `make windows-package` puts everything a user
unzips in `build/win/DAGCore/`.

| Variable | Default | Effect |
|----------|---------|--------|
| `NATIVE` | `0` | `1` adds `-march=native`. Only for a binary that stays on the machine that built it. |
| `DEBUG` | `0` | `1` gives `-O0 -g3` with address and UB sanitizers. |
| `USE_OPENSSL` | unset | `1` uses libcrypto's SHA-256 instead of the bundled one. |
| `PREFIX` | `/opt/dagcore-miner` | Install location, the installer's. `/usr/local` until 1.2.1. |
| `SYSCONFDIR` | `/etc/dagcore-miner` | Configuration, regardless of `PREFIX`. |
| `MINGW_CC` | `x86_64-w64-mingw32-gcc` | Compiler for `make windows`. |
| `OPENCL_HEADERS` | `/usr/include/CL` | OpenCL headers for `make windows`. |

**Installing by hand on a rig set up by `install.sh`.** The installer uses
`PREFIX=/opt/dagcore-miner`, and its service runs
`/opt/dagcore-miner/bin/dagcore-miner` with the dashboard from
`/opt/dagcore-miner/share/dagcore-miner/dashboard`. `make install` uses the same
prefix by default since 1.2.1:

```sh
make
sudo make install
sudo systemctl restart dagcore-miner
```

That replaces the binary, the kernel and the dashboard under the prefix, and
`config.env.example` in `SYSCONFDIR`; an existing `config.env` is never
touched. The dashboard's pages are read on every request, so a change to them
alone shows after a browser reload, without the restart. The installer's
Upgrade does the same from a fresh build.

Before 1.2.1 the default was `/usr/local`, which nothing on an installed rig
reads: the service kept the old binary or, when only the binary was copied
across, ran a new binary next to the old dashboard. A setup that installs into
`/usr/local` on purpose now needs `make install PREFIX=/usr/local` (and
`make uninstall PREFIX=/usr/local`).

`-Wall` is on. The build is warning-free; keep it that way.

## Changes from DagTech Miner

### Submission latency (#44)

`TCP_NODELAY` on the Stratum socket. Nagle held each short JSON line until the
previous segment was acknowledged, which on a high-RTT link turned accepted
shares into stale ones.

The submission rate limiter went from 200 ms to 20 ms. At 200 ms any share found
inside the window was dropped **silently** — not submitted, not counted as
stale, invisible. GPU burst rates exceed 5 shares/second easily, so real work was
being discarded with no way to notice. A `dropped` counter now records it.

### Linux paths

The autotune cache defaulted to `C:\dagtech-gpu-miner\autotune.json` and the
dashboard built `%s\index.html`, both of which fail silently on Linux. Now the
XDG cache directory and a forward slash. `mkdir_parents` also actually creates
parents, which it did not despite the name.

### gpu_align

The VRAM clamp originally rounded the work-item count down to a power of two.
A change removed that, and on an RTX 3080 it cost hashrate:

| Alignment | Work-items | Hashrate |
|-----------|-----------|----------|
| `pow2` | 16384 | **1.614 MH/s** |
| `align 256` | 19712 | 1.494 MH/s (−7.4%) |
| exact fit | 19754 | 1.472 MH/s (−8.8%) |

The obvious theory was that a non-power-of-two work size forces the driver into
a poor local work size. Measurement says otherwise: 19712 is cleanly divisible
and still 7.4% slower. At this scratchpad size the card is memory-bound and the
larger V buffer (2.4 GB against 2.0 GB) costs more than the extra parallelism
returns. `pow2` is the default on that evidence, and `--gpu-align N` exists so
another card can be measured rather than assumed.

### Control API and dashboard

A token-protected POST API on the metrics server for power limit, clocks,
offsets and intensity, and a single-file dashboard that uses it. A clock or
offset change is applied live but only saved after ten minutes without new
rejected shares; on failure the previous value is restored automatically. See
[CONFIG.md](CONFIG.md#control-api).

`/api/config` writes the rig's own settings (wallet, pool, port,
threads, cards) into `config.env`. That reverses the earlier rule that the
browser never writes that file, deliberately, so a rig can be configured
without a shell; validation, a `.bak` copy, keeping every other line and
refusing command-line-pinned settings are what replace the rule. `/api/pause`
ends the mining session the way a dropped pool connection does and makes the
reconnect loop wait; the web server is its own thread and is never paused.

### Smaller things

- Unknown CLI options are refused instead of silently ignored.
- `METRICS_BIND`, defaulting to `127.0.0.1`. It bound `INADDR_ANY` before while
  the log claimed localhost.
- `shutdown()` on the pool socket before joining the receive thread; a connected
  but silent pool used to hang shutdown until `SIGKILL`.
- The statistics loop checks `running` every second instead of every ten, so a
  flag set by another thread is noticed in 0.7 s rather than 9.7 s.
- `DT_PRIu64` everywhere for `uint64_t`; `%lu` with a cast truncated counters to
  32 bits on Windows.

## Traps

**`DT_VALUE_OPTS` must be kept in sync.** Every option that takes a value must
be listed there too. Forgetting costs nothing until someone passes that option
last on the line, and then the error says "unknown option" instead of "requires
a value".

**NVML cannot read back a locked clock.** `nvmlDeviceSetGpuLockedClocks` and
`nvmlDeviceResetGpuLockedClocks` exist; there is no `Get`. The
`ApplicationsClocksSetting` event bit does not cover them — checked on a card
with memory locked to 9851 MHz, the reason mask read `0x4` with bit `0x2` clear.
An earlier version inferred a lock from a clock holding still for 600 ms and was
wrong in both directions within three minutes of one session. Do not reintroduce
the guess; ask the operator.

**The OpenCL device index is not the NVML index.** The miner enumerates through
OpenCL, `nvidia-smi` and NVML through NVML, and nothing guarantees the orders
agree. This is why every tuning control is disabled on a multi-GPU rig.

**A memory offset displaces the clock table by half the offset.** Measured on an
RTX 3080: base table tops at 9501 MHz, with a +1200 offset the card reports a
10101 MHz ceiling (9501 + 600) and accepts a 9851 MHz lock (9251 + 600). So
`--query-supported-clocks` describes the table *before* the offset, while
`clocks.max.*` and `nvmlDeviceGetMaxClockInfo` account for it. Locks are stored
as table steps for this reason. The core offset is treated as 1:1, which is
conventional but unverified here — the core offset on the test card was 0.

**`nice()` returns the new priority**, so `-1` is a legitimate success. Clear
`errno` before calling it.

**A difficulty-1 share is ~65537 hashes, not 2^32.** A share passes when the top
64 bits of its hash are `<= 0xFFFF00000000 / difficulty`, so one share at
difficulty 1 takes 2^64 / 0xFFFF00000000 hashes on average. Bitcoin's 2^32
turns the effective hashrate into 10^11 H/s. Checked against 682 accepted
shares on the production rig: raw 1.65 MH/s, effective 1.56 MH/s with the
right constant (`HASHES_PER_DIFF1`).

**A full 10-minute window is seldom 600 seconds.** The effective window starts
at the oldest 5-second history sample inside it, so once full it spans 595–600s.
Testing `effective_window_s >= 600` fails on almost every poll; that is what
`effective_window_full` is for.

**`make install` creates `config.env` from the example when none exists.**
Anything that writes a configuration must record whether one existed *before*
it ran. `install.sh` did not, kept the freshly-dropped template, and shipped a
rig mining to `0x0000...0000`.

## Releases

Every functional change goes into [CHANGELOG.md](../CHANGELOG.md), under
`[Unreleased]`, in the same commit as the change. A release moves those entries
under a new version heading and bumps `DAGCORE_VERSION` in `dagcore_miner.c`,
following semantic versioning: new features raise the minor version, fixes
alone the patch.

## Not done yet

**PCI bus-ID mapping for multi-GPU.** Until the OpenCL device can be matched to
the NVML one by PCI bus ID, tuning is blocked whenever more than one card is
present. Mining across all cards works and scales linearly; only the controls
are held back. This is the main thing standing between the dashboard and a
multi-card rig.

**Windows.** The `#ifdef _WIN32` paths are inherited. They compile and link
(`make windows`) and can be run by hand (README, "Windows (experimental)"),
with the token from `BCryptGenRandom`, every file next to the `.exe` (the
portable layout below),
`overrides.env` replaced with `MoveFileEx`, the metrics receive timeout,
`2>NUL` for `nvidia-smi`, and a clean stop on console close. Still missing:
NVML (stubbed, so no offsets or clock locks), a Windows service and
installer, and an ACL on the token file (other local accounts can read it).
None of it has run on Windows yet; treat the build as unverified.

**Portable layout (`DT_PORTABLE_LAYOUT`).** On for Windows builds, off for
Linux. The miner is a folder someone unzips: `config.env`, `overrides.env`,
`api-token`, `autotune.json`, `dashboard\` and `dagcore_gpu.cl` are all read
from, and created in, the directory of the running `.exe` (from
`GetModuleFileName`, not `argv[0]`). Files an earlier test build left in
`%ProgramData%\DAGCore\` (and an autotune cache in `C:\dagtech-gpu-miner\`)
are used while there is none next to the `.exe`. A `DASHBOARD_DIR` without
an `index.html` falls back to the `dashboard` folder next to the `.exe`. To
test it on Linux, build with `-DDT_PORTABLE_LAYOUT` and set `ProgramData` in
the environment to stand in for the Windows one.

**Memory junction temperature.** Not exposed by the Linux driver to any tool, so
the dashboard cannot show the one temperature that matters most when tuning
memory. Hashrate sag is the only available proxy.
