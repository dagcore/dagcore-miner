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

The Windows build is for testing. It has run by hand on Windows 11 with an
RTX 3080, GPU and CPU, against the public pool, but it has no service, no
installer, and no GPU tuning beyond the power limit and intensity. Do not
leave a rig on it unattended yet.

**Build the kit** on Linux with MinGW-w64 (`sudo apt install mingw-w64`):

```sh
make windows-package    # -> dist/windows/
```

It also builds on Windows itself, without administrator rights — see
[DEVELOPMENT.md](docs/DEVELOPMENT.md#building).

**`dist/windows` is the whole kit.** Copy that folder to the Windows machine,
somewhere you can write to — for example `C:\DAGCore`, not `Program Files`.
It holds:

| File | What |
|------|------|
| `dagcore-miner.exe` | GPU build (and CPU); needs an NVIDIA driver, which brings OpenCL |
| `dagcore-miner-cpu.exe` | CPU only, for a machine without an NVIDIA card |
| `dagcore_gpu.cl` | The GPU kernel, read at every start |
| `dashboard\` | The dashboard's pages |
| `config.env.example` | Settings, written for Windows |
| `readme.html` | A getting-started page; opens with a double-click, offline |
| `start.bat` | Starts `dagcore-miner.exe` as administrator, which the power limit needs |
| `SHA256SUMS` | Checksums of all of the above |

Nothing else is needed: the `.exe` files have no DLLs of their own to bring
along.

**Check the files** after copying, from PowerShell in that folder. Every line
must say `True`:

```powershell
Get-Content SHA256SUMS | ForEach-Object {
    $hash, $file = $_ -split '\s+\*?', 2
    '{0,-5} {1}' -f ((Get-FileHash $file -Algorithm SHA256).Hash -eq $hash), $file
}
```

`False` means the file is damaged or incomplete: copy it again. As with the
Linux binaries, the sums catch a broken copy, not a tampered kit.

**Set the wallet.** Rename `config.env.example` to `config.env` and fill in
`WALLET=`, or give `--wallet` on the command line instead. It needs no
`DASHBOARD_DIR`; the line is there, commented out, only for serving a copy
kept elsewhere.

**Run it** by double-clicking `dagcore-miner.exe` (or `dagcore-miner-cpu.exe`)
when `config.env` has the wallet, or from a Command Prompt or PowerShell:

```bat
C:\DAGCore\dagcore-miner.exe --wallet 0xYOURADDRESS
C:\DAGCore\dagcore-miner-cpu.exe --wallet 0xYOURADDRESS --threads -1
```

It finds everything in its own folder, whichever folder it is started from.
The GPU build mines on the card only (`THREADS=0`); for the CPU build,
`--threads -1` uses half the logical cores and a number sets them exactly.
Left out, the CPU build warns that its default, `0` (GPU only), would mine
nothing and uses `-1` instead. Windows may ask whether to allow network
access the first time.

To change the power limit from the dashboard, start it with **`start.bat`**
instead: it asks for administrator rights (the UAC prompt) and starts
`dagcore-miner.exe` in a window of its own, passing on any arguments
(`start.bat --threads 1`). Refused, it starts nothing and says how to mine
without them.

Open **http://localhost:8881/** for the dashboard. To reach it from another
computer, set `METRICS_BIND=0.0.0.0` and allow TCP 8881 in Windows Firewall
for the local network only; read the [security notes](docs/INSTALL.md#security)
first. Stop the miner with Ctrl+C or by closing the window; either way it
shuts down cleanly.

**Everything stays in that folder.** The miner reads `config.env`,
`dashboard\` and `dagcore_gpu.cl` from next to the `.exe`, and writes there
too: `api-token` on the first start, `config.env` when Mining configuration is
saved from the dashboard (created if it was not there), `overrides.env` once a
tuning setting is changed, and `autotune.json` for the GPU build. A
`DASHBOARD_DIR` that does not exist on this machine — the `/opt/...` one in
the Linux `config.env.example`, say — falls back to the `dashboard` folder
next to the `.exe`. The first test builds kept `config.env`, `overrides.env`
and `api-token` in `%ProgramData%\DAGCore\`; a file found only there is copied
next to the `.exe` at the next start, and the console says so. The token stays
the same, so a browser that has it keeps working, and that old folder can be
deleted. Every start prints which token file is in use (`Control API token:`).

**Limits of this build.** Temperature, load, power, memory and clocks are
read from the NVIDIA driver. Clock locks work, through NVML as on Linux.

**Clock offsets cannot be set from the miner on Windows.** The Windows
GeForce driver refuses them through NVML ("Not Supported"), so the dashboard
hides the offset rows and shows that reason. Tuning on Windows is yours to do
with **MSI Afterburner**, running next to the miner. The miner reads the
clocks that result and shows them as they are: with the memory raised in
Afterburner to 10277 MHz, the dashboard and `nvidia-smi` both showed 10277.
Two things it cannot know about. The lock range (`gpu_mem_clock_max`) stays at
the card's stock table (9501 MHz on an RTX 3080), so leave the memory lock
off while Afterburner raises the memory; the two were not tested together.
And the offset belongs to Afterburner: it holds until Reset or a reboot, and
comes back after a reboot only if Afterburner applies it at startup.

**Expect less hashrate on Windows without tuning.** This algorithm is
memory-bound, and without an offset the driver keeps the memory at its
compute clock, 9251 MHz on an RTX 3080. Measured on the same RTX 3080 at 300 W:

| | Memory clock | Hashrate |
|---|---|---|
| Windows, no tuning | 9251 MHz | 1.43–1.53 MH/s |
| Windows, memory raised in Afterburner | 10277 MHz | 1.63 MH/s |
| Linux, memory offset +1200 set from the miner | 9851 MHz | 1.61 MH/s |

The numbers do not mean the same thing in both tools. Afterburner's memory
offset moves the clock one to one (+355 raised it from 9251 to 9605 MHz); the
miner's offset on Linux moves it by half (+1200 gives +600). So +1200 on Linux
is about +600 in Afterburner.

The power limit and the clock controls work only when
the miner runs as administrator
(`start.bat`, or right-click `dagcore-miner.exe` → Run as administrator); otherwise the
dashboard says so and leaves it unavailable. There is no service: Save & restart
works because the miner starts itself again, in the same window, but nothing
brings it back after a crash or a reboot. The GPU build keeps one CPU
core busy while it mines — the NVIDIA driver waits for the card by spinning
(see [DEVELOPMENT.md](docs/DEVELOPMENT.md#not-done-yet)). The token file is
not protected from other accounts on the same computer yet — do not test on
a shared machine.

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
