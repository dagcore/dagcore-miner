# Installing and running DAGCore Miner

For the person who wants a machine mining, not a tour of the source.

- [Before you start](#before-you-start)
- [Installing](#installing)
- [Special cases](#special-cases)
- [After it is installed](#after-it-is-installed)
- [Upgrading, reconfiguring, removing](#upgrading-reconfiguring-removing)
- [Troubleshooting](#troubleshooting)
- [Security](#security)

## Before you start

You need a Linux machine on x86-64 with an NVIDIA card, the NVIDIA driver
installed, and a BlockDAG wallet address.

The installer will not install a graphics driver. That needs a reboot and can
leave a machine with no display if it goes wrong, so it stops and tells you the
command for your distribution instead.

## Installing

```sh
sudo ./install.sh
```

To see everything it would do first, without touching the machine:

```sh
./install.sh --dry-run
```

### What it asks

**Your wallet address (0x...)** — required, and checked: it must be `0x`
followed by 40 hexadecimal characters. A typo here means mining for someone
else, so there is no default.

**Pool address** and **Pool port** — default `stratum.dagcore.net` and `3334`.

**A name for this machine** — defaults to the hostname. The current DagCore pool
ignores worker names, so this is for your own records.

**CPU mining threads** — default `0`, graphics card only. CPU mining on this
algorithm contributes well under one percent of a GPU's hashrate while taking
cores the GPU needs to keep fed. `-1` auto-detects half the logical cores.

**Which graphics cards** — asked only when there is more than one. The default
is all of them; hashrate adds up and leaving one out simply earns less.

**Dashboard reachable only from this machine?** — default yes. Answering no
opens it to your whole local network; read [Security](#security) first.

**Create a service?** — default yes when the system has systemd. The miner then
starts at boot and restarts by itself.

**Start mining now?** — default yes.

### Unattended

For several machines:

```sh
sudo ./install.sh --wallet 0xYOURADDRESS --yes
```

`--yes` takes every default and asks nothing, so `--wallet` becomes mandatory.
Other options: `--pool`, `--port`, `--worker`, `--threads`, `--gpu-device`,
`--lan`, `--no-service`, `--start`, `--prefix`. `./install.sh --help` lists them.

## Special cases

### A container without systemd (RunPod, Vast.ai)

The installer detects containers and says so. There is no service manager, so
instead of a service it writes a start script:

```sh
sudo /opt/dagcore-miner/start.sh
```

That runs the miner in the foreground; Ctrl+C stops it. To keep it running after
you disconnect, use `screen`, `tmux`, or `nohup`.

### "No OpenCL platforms found" in a container

Container images normally ship the NVIDIA OpenCL library without the file that
registers it, so programs cannot find the driver. The miner then sees no card,
falls back to CPU mining and earns effectively nothing while looking like it is
running.

The installer detects exactly this — library present, registration missing — and
offers to create the file. Say yes. By hand it is:

```sh
sudo mkdir -p /etc/OpenCL/vendors
echo libnvidia-opencl.so.1 | sudo tee /etc/OpenCL/vendors/nvidia.icd
```

This is the normal state of RunPod, Vast.ai and the CUDA images, not a fault in
your setup.

### More than one card

The installer counts the cards and sets `GPU_DEVICE=all` when it finds more than
one. If a rig was set up before that behaviour existed, check:

```sh
grep GPU_DEVICE /etc/dagcore-miner/config.env
```

`GPU_DEVICE=0` means only the first card is mining. Change it to `all` and
restart. The dashboard's **GPUs active** figure tells you how many are in use.

On a multi-card rig the dashboard's tuning controls are switched off, with the
reason shown. Mining on every card works; only the tuning is held back, because
the miner numbers cards through OpenCL and NVIDIA numbers them differently and
nothing guarantees the two agree — so a power limit could land on the wrong card.

### No nvidia-smi

Mining works. These do not: power limit, core and memory clock locks, clock
offsets. The dashboard shows the reason instead of dead controls. `nvidia-smi`
usually comes with the driver, in a package named something like
`nvidia-utils`.

## After it is installed

**Dashboard** — http://localhost:8881/ , help page at
http://localhost:8881/help

**Control token** — needed to change anything from the dashboard. Reading works
without it.

```sh
sudo cat /etc/dagcore-miner/api-token
```

It is also printed once, the first time the miner runs, in the log. To force a
new one, delete the file and restart; every browser holding the old one will
need the new.

**Starting and stopping**

```sh
sudo systemctl status dagcore-miner     # is it running
sudo systemctl stop dagcore-miner
sudo systemctl start dagcore-miner
sudo systemctl restart dagcore-miner
```

Without a service, `sudo /opt/dagcore-miner/start.sh` and Ctrl+C.

**Logs**

```sh
sudo journalctl -u dagcore-miner -f          # live
sudo journalctl -u dagcore-miner -n 100      # last 100 lines
```

**Files**

| Path | What |
|------|------|
| `/opt/dagcore-miner/` | The program, the GPU kernel, the dashboard |
| `/etc/dagcore-miner/config.env` | Your configuration. Edit it by hand, or from Settings → Mining configuration, which keeps the previous version as `config.env.bak` |
| `/etc/dagcore-miner/api-token` | The dashboard's control token, root-only |
| `/var/lib/dagcore-miner/overrides.env` | Tuning written by the dashboard, loaded on top of config.env |

Two files on purpose: tuning never touches the file that defines the rig, and
throwing all tuning away is one deleted file. The dashboard writes
`config.env` only through Mining configuration — six settings, validated first,
every other line kept.

## Upgrading, reconfiguring, removing

Run the installer again. It notices the existing installation and offers:

1. **Upgrade** — rebuild and replace the program, keep every setting
2. **Reconfigure** — ask the questions again and rewrite the configuration
3. **Cancel**

`--yes` picks Upgrade.

To upgrade to a new version, fetch it first (`git pull` in the directory you
cloned), then run the installer. What changed is in
[CHANGELOG.md](../CHANGELOG.md). Settings added by a new version are not
written into an existing `config.env`; while they are missing, the miner uses
their defaults, and [CONFIG.md](CONFIG.md) lists them.

To remove it:

```sh
sudo ./uninstall.sh
```

The program goes without asking. Your configuration and your tuning are asked
about separately — they took effort and cannot be rebuilt from a package.
`--purge` removes everything without asking.

## Troubleshooting

### "No OpenCL platforms found", or the miner mines on CPU only

**Symptom** — the log says no OpenCL platform, the dashboard shows GPUs active 0,
hashrate is a few kH/s or zero.

**Cause** — no OpenCL registration file, so nothing can find the NVIDIA driver.
Usual in containers.

**Fix** — see [above](#no-opencl-platforms-found-in-a-container). Then restart
the miner.

### Hashrate 0 and "Threads: 0"

**Symptom** — the miner runs, connects to the pool, and produces nothing.

**Cause** — `THREADS=0` means no CPU mining, which is the right default; combined
with a GPU that failed to initialise it leaves nothing mining at all.

**Fix** — find out why the GPU did not come up. The log lines just after startup
say. Usually the OpenCL registration above.

### Rejected shares after tuning

**Symptom** — the rejected counter climbs after a clock or offset change.

**Cause** — the card computes wrong results past what its silicon holds. It does
not crash; the pool just refuses the work.

**Fix** — inside the ten-minute test the miner reverts by itself. If the value was
already saved, lower it one step and let the test confirm. Memory offset is the
usual culprit.

### The miner will not start

```sh
sudo systemctl status dagcore-miner
sudo journalctl -u dagcore-miner -n 50 --no-pager
```

A wrong option is now refused at startup with a clear message rather than being
ignored, so a typo in the configuration shows up here.

### The dashboard does not open

**From the rig itself** — check the miner is running, and that
`DASHBOARD_DIR` in `config.env` points at an existing directory.

**From another machine** — by default the dashboard listens on localhost only.
Either tunnel:

```sh
ssh -L 8881:127.0.0.1:8881 user@rig
```

then open http://127.0.0.1:8881/ locally, or set `METRICS_BIND` — after reading
the next section.

## Security

**The metrics endpoint has no authentication and it serves your full wallet
address.** Anyone who can reach `http://<rig>:8881/metrics` reads the wallet,
the pool, the worker name and the hashrate. There is no password on reading.

Changing settings needs the token, but that only protects writes, and the token
travels in plain HTTP — anyone who can watch the network can copy it and then
change your clocks. **The token can also change the wallet and the pool**
(Settings → Mining configuration): whoever holds it and can reach the port can
redirect your payouts. Treat it like a password.

What that means:

- `METRICS_BIND` defaults to `127.0.0.1`. Out of the box the dashboard is
  reachable only from the rig.
- To use it from your network, set `METRICS_BIND` to the rig's own LAN address
  rather than `0.0.0.0`, so it is not offered on every interface. The miner
  warns in its log whenever it binds beyond localhost.
- **Never expose this to the internet.** Not with a port forward, not through a
  router's DMZ. It is plain HTTP with a shared secret and your wallet on it.
- From outside the LAN, tunnel over SSH instead of forwarding a port.
