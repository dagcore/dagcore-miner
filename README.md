# DAGCore Miner

An OpenCL miner for [BlockDAG](https://dagcore.net) (chain 1404), with a built-in
web dashboard for monitoring and GPU tuning.

Derived from DagTech Miner (MIT). Linux; a Windows build is in progress and
can be run by hand for testing — see [Windows (experimental)](#windows-experimental)
and [what is left to do](docs/DEVELOPMENT.md#not-done-yet).

## Requirements

- Linux on x86-64
- An NVIDIA graphics card
- The NVIDIA driver, with its OpenCL support

`nvidia-smi` is optional: mining works without it, but the power-limit and clock
controls in the dashboard do not.

## Install

```sh
git clone https://github.com/dagcore/dagcore-miner.git
cd dagcore-miner
sudo ./install.sh
```

The installer asks for a wallet address and takes sensible defaults for
everything else. It checks the machine, installs what it needs to build (after
asking), compiles, sets up a service, and tells you where the dashboard is.

To see what it would do without touching anything:

```sh
./install.sh --dry-run
```

For several machines, skip the questions:

```sh
sudo ./install.sh --wallet 0xYOURADDRESS --yes
```

[Full installation guide →](docs/INSTALL.md)

### Prebuilt binaries

From 1.2.0 on, each [release](https://github.com/dagcore/dagcore-miner/releases)
also carries Linux x86-64 binaries: `dagcore-miner` (GPU and CPU),
`dagcore-miner-cpu` (CPU only), the GPU kernel `dagcore_gpu.cl`, and
`SHA256SUMS`. They need glibc 2.34 or newer (Ubuntu 22.04, Debian 12 or later)
and, for the GPU build, the NVIDIA driver's OpenCL (`libOpenCL.so.1`). Keep
`dagcore_gpu.cl` in the same directory as `dagcore-miner`: the miner loads it
from there. The installer above is still what sets up the service and the
dashboard. The dashboard's pages are not among these files: replacing only the
binary of an installed rig leaves the old dashboard in place, so upgrade with
`git pull` and `sudo ./install.sh` instead.

To install a build of your own over an installed rig (`make install` uses the
installer's `/opt/dagcore-miner` since 1.2.1):

```sh
make && sudo make install && sudo systemctl restart dagcore-miner
```

Check the files before running them:

```sh
V=1.2.1
base=https://github.com/dagcore/dagcore-miner/releases/download/v$V
for f in dagcore-miner dagcore-miner-cpu dagcore_gpu.cl SHA256SUMS; do
    curl -fLO "$base/$f"
done
sha256sum -c SHA256SUMS
chmod +x dagcore-miner dagcore-miner-cpu
```

Every line must end in `OK`. `FAILED` means the file is damaged or not the one
released: delete it and download it again. The sums come from the same place as
the files, so they catch a broken or incomplete download, not a compromised
account; they are not a signature.

## Windows (experimental)

The Windows build is for testing only. It is cross-compiled on Linux, has not
been through a real test yet, and has no service, no installer and no GPU
tuning. The steps below run it by hand, CPU-only, on a machine without an
NVIDIA card.

**Build it** on a Linux machine with MinGW-w64 (`sudo apt install mingw-w64`):

```sh
make windows-package    # -> build/win/DAGCore/
```

**Copy the `DAGCore` folder to the Windows machine**, somewhere you can write
to — for example `C:\DAGCore`, not `Program Files`. It holds both `.exe` files,
`dagcore_gpu.cl`, the `dashboard` folder, `readme.html` — a step-by-step
getting-started page that opens with a double-click, offline, styled like the
dashboard — and a `config.env.example` written for Windows: rename it to `config.env` and fill in `WALLET=` (or give `--wallet` on
the command line instead). It needs no `DASHBOARD_DIR`; the line is there,
commented out, only for serving a copy kept elsewhere.

Nothing else is needed: the `.exe` has no DLLs of its own to bring along.
(`dagcore-miner.exe` is the GPU build; it also needs `dagcore_gpu.cl` in the same
folder and an OpenCL driver, and is not covered here.)

**Run it** by double-clicking `dagcore-miner-cpu.exe` when `config.env` has the
wallet, or from a Command Prompt or PowerShell:

```bat
C:\DAGCore\dagcore-miner-cpu.exe --wallet 0xYOURADDRESS --threads -1
```

It finds everything in its own folder, whichever folder it is started from.
`--threads -1` uses half the logical cores; a number sets them exactly. Left
out, the CPU build warns that its default, `0` (GPU only), would mine nothing
and uses `-1` instead. Windows may ask whether to allow network access the
first time.

Open **http://localhost:8881/** for the dashboard. Stop the miner with Ctrl+C
or by closing the window; either way it shuts down cleanly.

**Everything stays in that folder.** The miner reads `config.env`,
`dashboard\` and `dagcore_gpu.cl` from next to the `.exe`, and writes there
too: `api-token` on the first start, `config.env` when Mining configuration is
saved from the dashboard (created if it was not there), `overrides.env` once a
tuning setting is changed, and `autotune.json` for the GPU build. A
`DASHBOARD_DIR` that does not exist on this machine — the `/opt/...` one in
`config.env.example`, say — falls back to the `dashboard` folder next to the
`.exe`. The first test builds kept `config.env`, `overrides.env` and
`api-token` in `%ProgramData%\DAGCore\`; a file found only there is copied
next to the `.exe` at the next start, and the console says so. The token stays
the same, so a browser that has it keeps working, and that old folder can be
deleted. Every start prints which token file is in use (`Control API token:`).

**Limits of this build.** On a CPU-only machine the power, clock and intensity
controls say they are unavailable, which is correct. The token file is not
protected from other accounts on the same computer yet — do not test on a
shared machine. What else is missing is listed in
[DEVELOPMENT.md](docs/DEVELOPMENT.md#not-done-yet).

## The dashboard

Once the miner runs, open **http://localhost:8881/** — hashrate, temperatures,
shares, controls for power limit, clocks and offsets, the rig's own settings
(wallet, pool, threads, cards), and Pause/Resume. Next to the raw
hashrate it shows the **effective** one, the work the pool actually accepted,
and a 30-minute chart of both that the miner keeps, so it survives a page
reload. A help page explaining every number and setting is at **/help**.

By default it listens on localhost only. Read the
[security notes](docs/INSTALL.md#security) before opening it to your network.

## Measured hashrates

These are our own measurements on our own hardware, running this miner against
a live pool. **They are not a promise.** What a card does depends on its
silicon, its cooling, its power limit and the rest of the machine — two cards of
the same model can differ by several percent.

| Card | Hashrate | Notes |
|------|----------|-------|
| RTX 3080 | 1.61 MH/s | 300 W limit, memory offset +1200 |
| RTX 4090 | 2.06 MH/s | per card, stock |
| 2× RTX 4090 | 4.09 MH/s | 2.06 + 2.03, scaling is linear |

The algorithm is scrypt with N=1024, which needs 128 KB of scratchpad per
work-item. It is bound by memory bandwidth, not arithmetic — which is why memory
tuning moves the number and core clock mostly does not.

## Documentation

| Document | For |
|----------|-----|
| [docs/INSTALL.md](docs/INSTALL.md) | Installing, running, and fixing it when it misbehaves |
| [docs/CONFIG.md](docs/CONFIG.md) | Every setting, CLI option, API endpoint and metrics field |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Building on the source, and the traps in it |
| `/help` on the dashboard | What each number and control means, in the browser |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each version |

## Contributors

- [@dagcore](https://github.com/dagcore)

## Licence and attribution

MIT.

```
Copyright (c) 2024-2026 DagTech Ltd / Dawie Nel
Portions Copyright (c) 2026 DagCore Community
```

DAGCore Miner is derived from **DagTech Miner** by Dawie Nel / DagTech Ltd, which
remains the copyright holder of the original work — the scrypt engine, the
OpenCL kernel and the Stratum client are theirs. This project adds Linux
packaging, the control API, the dashboard and the fixes listed in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#changes-from-dagtech-miner).

The dashboard embeds **IBM Plex Mono**, © IBM Corp., under the SIL Open Font
License 1.1. The licence travels with it in
`dashboard/OFL.txt`.
