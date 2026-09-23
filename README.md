# DAGCore Miner

An OpenCL miner for [BlockDAG](https://dagcore.net) (chain 1404), with a built-in
web dashboard for monitoring and GPU tuning.

Derived from DagTech Miner (MIT). Linux only for now — see
[what is left to do](docs/DEVELOPMENT.md#not-done-yet).

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
dashboard.

Check the files before running them:

```sh
V=1.2.0
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
