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

## Windows (experimental)

The Windows build is for testing only. It is cross-compiled on Linux, has not
been through a real test yet, and has no service, no installer and no GPU
tuning. The steps below run it by hand, CPU-only, on a machine without an
NVIDIA card.

**Build it** on a Linux machine with MinGW-w64 (`sudo apt install mingw-w64`):

```sh
make windows    # -> dagcore-miner-cpu.exe and dagcore-miner.exe
```

**Copy to the Windows machine**, all into one folder, for example
`C:\DAGCore`:

- `dagcore-miner-cpu.exe`
- the `dashboard` folder, whole

Nothing else is needed: the `.exe` has no DLLs of its own to bring along.
(`dagcore-miner.exe` is the GPU build; it also needs `dagcore_gpu.cl` next to it
and an OpenCL driver, and is not covered here.)

**Run it** from a Command Prompt or PowerShell opened in that folder:

```bat
cd C:\DAGCore
dagcore-miner-cpu.exe --wallet 0xYOURADDRESS --threads -1 --dashboard-dir dashboard
```

`--threads -1` uses half the logical cores; a number sets them exactly. Left
out, the CPU build warns that its default, `0` (GPU only), would mine nothing
and uses `-1` instead. Windows may ask whether to allow network access the
first time.

Open **http://localhost:8881/** for the dashboard. Stop the miner with Ctrl+C
or by closing the window; either way it shuts down cleanly.

**Files it creates** in `%ProgramData%\DAGCore\` (usually
`C:\ProgramData\DAGCore\`): `api-token`, and `overrides.env` once a setting
is changed from the dashboard. Settings can also go in a `config.env` there, in
the same format as [config.env.example](config.env.example) — with it, the
command line shrinks to `dagcore-miner-cpu.exe`. A `config.env` in the folder
you run it from is found too, and takes precedence.

**Limits of this build.** On a CPU-only machine the power, clock and intensity
controls say they are unavailable, which is correct. The token file is not
protected from other accounts on the same computer yet — do not test on a
shared machine. What else is missing is listed in
[DEVELOPMENT.md](docs/DEVELOPMENT.md#not-done-yet).

## The dashboard

Once the miner runs, open **http://localhost:8881/** — hashrate, temperatures,
shares, and controls for power limit, clocks and offsets. Next to the raw
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
