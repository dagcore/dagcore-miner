# Changelog

All notable changes to DAGCore Miner are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `make windows` cross-compiles `dagcore-miner.exe` and
  `dagcore-miner-cpu.exe` with MinGW-w64, and `make check` builds them too.
  The Windows build compiles and links but has not been run yet, and it has no
  service, installer or GPU tuning (NVML); it is not ready for use.
  - Windows: the control API token comes from `BCryptGenRandom` (there is no
    `/dev/urandom`); without it the control API was always disabled.
  - Windows: `overrides.env` is replaced with `MoveFileEx`; `rename()` fails
    there when the file exists, so only the first dashboard change was kept.
  - Windows: `config.env`, `overrides.env` and the token live in
    `%ProgramData%\DAGCore\` instead of `/etc` and `/var/lib`.
  - Windows: the dashboard server gives up on a silent client after 2 seconds,
    as on Linux, instead of hanging for every other client.
  - Windows: `nvidia-smi` is called with `2>NUL`; `cmd.exe` has no
    `/dev/null`, so temperature, power and the power limit were never read.

### Fixed
- A miner started by hand (not as the systemd service) died with SIGPIPE when
  a client closed the connection while the dashboard server was still sending
  its response. SIGPIPE is now ignored.

## [1.1.0] - 2026-09-23

### Added
- **Effective hashrate** on the dashboard, next to the raw figure: the work the
  pool accepted over the last 10 minutes, as a hashrate, and its percentage of
  raw over the same window. Each accepted share counts at the pool difficulty of
  the job it was found for. The help page explains why the two differ.
- **The 30-minute chart is kept by the miner.** A ring buffer of 360 samples,
  one every 5 seconds, in memory only, served as `GET /history`. A page reload
  or a second device now shows the whole half hour; a miner restart starts it
  empty. Nothing is written to disk.
- The chart draws the effective hashrate as a green line over the raw area, with
  a "now" value for each in the legend. It starts once the 10-minute window is
  full.
- `SUBMIT_MIN_INTERVAL_MS` in `config.env`: the minimum gap between two share
  submissions, previously fixed at 20 ms. Default 20, range 0–1000, `0` turns
  the limit off.
- A **Dropped shares** row under Advanced: shares dropped in the last 10
  minutes and their share of those found, the total since start, and the gap in
  use. The help page explains that the total comes mostly from the first
  minutes after a start and is not an ongoing loss.
- New `/metrics` fields, all appended after the existing ones:
  `effective_hashrate`, `effective_raw_hashrate`, `effective_pct`,
  `effective_window_s`, `effective_window_full`, `submit_min_interval_ms`,
  `dropped_window`, `submitted_window`.

### Changed
- Stat cards take the style of dagcore.net: each has its own accent for the
  border, the tint and the label — Total orange, Effective green, GPU light
  blue, CPU purple, Efficiency blue.

### Fixed
- The hashrate chart started empty after every page reload, because its samples
  lived only in the browser.
- The chart's y axis mixed units (kH/s next to MH/s) over an unrounded top, so
  its ticks looked uneven. It now uses a round linear step with every label in
  one unit.

## [1.0.0] - 2026-09-22

First release as DAGCore Miner, derived from DagTech Miner GPU-2026.0628.1.

### Added
- **Web dashboard** on the metrics port (http://localhost:8881/): hashrate,
  temperatures, power, shares, per-GPU hashrate and a 30-minute chart, in the
  dagcore.net visual identity. A help page at `/help` explains every number and
  control.
- **Control API**, token-protected, used by the dashboard: power limit,
  intensity, core and memory clock locks, and clock offsets through NVML
  (loaded with `dlopen`). Clock and offset changes run a 10-minute trial and
  revert on their own if shares are rejected; a test can be cancelled. Adopt
  current state takes over a card tuned with another tool. Basic and Advanced
  settings.
- Configuration in `/etc/dagcore-miner/config.env`, with `config.env.example`,
  and dashboard changes kept apart in `overrides.env`.
- `--metrics-bind`, defaulting to `127.0.0.1`.
- `--gpu-align <pow2|N>`, `pow2` by default, measured fastest on an RTX 3080.
- `dropped` in `/metrics`: shares discarded by the submission rate limiter,
  which used to vanish without a trace.
- Installer and uninstaller for users with no technical knowledge, with a
  systemd service; on rigs with several cards the default is
  `GPU_DEVICE=all`.
- README, installation guide, configuration reference and development notes.
  MIT licence, keeping the DagTech attribution.

### Changed
- Renamed to DAGCore: files, logs and banner. Default pool
  `stratum.dagcore.net:3334`. Versioning restarts at 1.0.0.
- `TCP_NODELAY` on the Stratum socket, and the submission rate limit lowered
  from 200 ms to 20 ms.
- The metrics server listens on `127.0.0.1` instead of every interface.
- Unknown options, and options missing their value, fail with a clear error.
- `THREADS=0` (GPU only) is the default and is documented as such.

### Fixed
- Linux paths: the autotune cache and the dashboard pointed at Windows paths
  and failed silently.
- Autotune errors are reported instead of looping silently.
- The CPU-only build compiles again.
- The stats loop notices a stop within a second, and shutdown no longer hangs
  on a silent pool.
- Builds clean with `-Wall`; a `malloc(0)` guard for GPU-only runs.
