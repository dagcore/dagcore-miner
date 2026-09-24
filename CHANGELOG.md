# Changelog

All notable changes to DAGCore Miner are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `make windows` builds `dagcore-miner.exe` and `dagcore-miner-cpu.exe` with
  MinGW-w64 - cross-compiled on Linux, or natively on Windows with WinLibs -
  and `make check` builds them too. Run by hand on Windows 11 with an RTX 3080
  (driver 617.14), against the public pool: NVML readings, the portable
  layout, the dashboard over the LAN, CPU use and a clean stop all checked.
  There is still no service, no installer and no GPU tuning beyond the power
  limit and intensity; it is for testing, not for a rig left unattended.
  - Windows: the control API token comes from `BCryptGenRandom` (there is no
    `/dev/urandom`); without it the control API was always disabled.
  - Windows: `overrides.env` and `config.env` are replaced with `MoveFileEx`;
    `rename()` fails there when the file exists, so only the first dashboard
    change was kept - for `config.env`, every Mining configuration save after
    the first answered "cannot replace config.env: File exists". Five saves in
    a row now each rewrite the file.
  - Windows: everything is in the miner's own folder, next to the `.exe` -
    `config.env` (which a dashboard save creates if missing),
    `overrides.env`, the token, `autotune.json`, `dashboard\` and
    `dagcore_gpu.cl` - found from the `.exe`'s real path whatever the
    current directory, instead of `/etc`, `/var/lib` and
    `C:\dagtech-gpu-miner\`. `config.env`, `overrides.env` or `api-token` left
    by the first test builds in `%ProgramData%\DAGCore\` is copied next to
    the `.exe` at startup (same token, so the browser keeps working). A
    `DASHBOARD_DIR` that does not exist falls back to the bundled `dashboard`
    folder.
  - Windows: the dashboard server gives up on a silent client after 2 seconds,
    as on Linux, instead of hanging for every other client.
  - Windows: `nvidia-smi` is called with `2>NUL`; `cmd.exe` has no
    `/dev/null`, so temperature, power and the power limit were never read.
  - Windows: closing the console window, logging off or shutting down stops
    the miner cleanly (`SetConsoleCtrlHandler`), and its waits of several
    seconds end as soon as a stop is requested. Ctrl+C, Ctrl+Break and the
    window's close button each end it in under a second, exit code 0, after
    "Shutdown complete".
  - Windows: waits under 2 ms use a high-resolution waitable timer. `usleep()`
    there was `Sleep(x/1000)`, so the submit queue's 500 µs gap became
    `Sleep(0)` and spun a core, and `Sleep(1)` can last a 15.6 ms timer tick.
    Measured: every thread but the GPU one together uses 0.1% of a core while
    the queue sends shares.
  - `make windows-package` puts the kit in `dist/windows/`: exactly the folder
    to copy to a new machine - both `.exe` files, `dagcore_gpu.cl`,
    `dashboard\`, `config.env.example`, `readme.html` and a `SHA256SUMS` for
    them. The `.exe` files are built in `build/win/`, no longer in the
    source tree. The `config.env.example` is made for Windows from the Linux
    one: no `/etc`, `/var/lib` or XDG paths, and no `DASHBOARD_DIR`
    (commented out), so the miner no longer starts by saying the `/opt`
    dashboard is missing.
  - `readme.html`, a getting-started tutorial in the dashboard's style, sits
    next to the `.exe` in the Windows package: what you need, putting the
    miner in place, the wallet, starting, the dashboard, the control token,
    Mining configuration, tuning, checking that it works, upgrading and
    troubleshooting - Linux and Windows side by side where they differ. It
    opens from the folder with a double-click, offline, before the miner
    has run.
  - Windows: the dashboard leaves out what that build cannot do - the clock
    lock and offset rows, Adopt, and the text about clock tests - instead of
    an error note about offsets; `/metrics` gains `clock_controls_supported`,
    `false` there. The miner no longer warns at every start that NVML gave no
    clock range, which it cannot there.
  - Windows: GPU temperature, load, memory used, power and clocks are read
    from `nvml.dll`, which every NVIDIA driver installs, loaded at run time
    from System32 or the NVSMI folder only; `nvidia-smi` is the fallback.
    Without either the miner runs normally and the dashboard leaves those
    readings out. Writes (power limit, offsets, clock locks) are not done
    through it yet. On an RTX 3080 the readings match `nvidia-smi` exactly,
    and an `nvml.dll` dropped next to the `.exe` is not loaded.
  - README: how to build, check and run the Windows kit by hand.
- Every start names the control API token file in use, not only the start
  that creates it.

### Changed
- The dashboard leaves out a reading the machine cannot give - GPU
  temperature, load, power, memory, CPU temperature - instead of showing
  `n/a`, and the whole GPU & thermals card when there is none.
- The CPU hashrate card is left out when no CPU thread mines (`THREADS=0`,
  the default with a GPU), instead of showing 0 H/s.
- The CPU-only build (`make cpu`, `dagcore-miner-cpu.exe`) no longer accepts
  `--threads 0` / `THREADS=0`, the default, silently: with no GPU support that
  mined nothing and reported 0 H/s. It now warns at startup and uses
  auto-detect (half the logical cores) instead.
- The startup banner and `--help` show only the name and version, without
  the original author's line. The DagTech copyright and attribution stay
  where the licence puts them: `LICENSE`, the source headers and the README.

### Fixed
- **GPU intensity can be changed wherever a card is mining.** It was held
  back with the tuning controls, which need `nvidia-smi` and a single card,
  so a rig with several cards, or without `nvidia-smi` (a container, Windows),
  could not change it from the dashboard although it is the miner's own
  setting. It now needs only the token and a mining card.
- A control API error message longer than 296 characters was cut off before
  its closing `"}`, so the page got invalid JSON instead of the error. The
  reply now has room for the longest message `/api/config` can give (300).
  Found by GCC 16's `-Wformat-truncation`.

## [1.2.1] - 2026-09-23

### Changed
- **`make install` installs into `/opt/dagcore-miner` by default**, where
  `install.sh` puts the miner and where its service looks, instead of
  `/usr/local`. A hand `make install` over an installed rig used to leave the
  service on the old binary and the old dashboard. *Possible impact:* if you
  install into `/usr/local` on purpose, pass `PREFIX=/usr/local` to
  `make install` and `make uninstall` from now on, or they work on
  `/opt/dagcore-miner`. `config.env.example` names the new dashboard path.

### Fixed
- **Mining configuration: its Save button is now hard to miss.** The only
  button of the block sat at its bottom with the same label as the GPU
  intensity one below it, so a changed CPU thread count was easily left
  unsaved. The block is now a box of its own; while a field is changed but not
  saved, the box is outlined, the button is filled in, and a line says what is
  pending ("Not saved yet: CPU threads 2 → 3"). The intensity button asks first
  when the box has unsaved changes, since it saves the intensity only.
- Primary buttons are 136px wide instead of 124px, so "Save & restart" is no
  longer cut to "Save & rest…"; Settings rows switch to the two-column layout
  below 780px instead of 560px, where the five columns no longer fit.
- `POST /api/config` compared the new values with the running miner only, so
  after a save without a restart, going back to the running value answered
  "nothing to save" and left config.env as it was. It now also compares with
  config.env itself.

## [1.2.0] - 2026-09-23

### Added
- Prebuilt Linux x86-64 binaries attached to the release, with the GPU kernel
  and `SHA256SUMS`; the README says how to check them.
- **The GPU reports every share of a batch, not just one.** The kernel kept a
  single nonce per batch (about 10 ms of work on an RTX 3080), so while the
  pool's difficulty is low — the first minutes of every connection — all but
  one share of each batch were lost uncounted. It now returns up to 64, each
  checked again on the CPU. `/metrics` gains `gpu_candidates_found`,
  `gpu_candidates_reported`, `gpu_candidates_extra` (beyond the first in their
  batch: what the old kernel lost) and `gpu_candidates_valid`, and Advanced
  shows a GPU candidates row.
- **A submit queue for the rest of a batch's shares.** They come a fraction of
  a millisecond after the first, too close for the 5 ms gap between
  submissions; a thread of its own sends them one at a time,
  `SUBMIT_BURST_GAP_US` apart (500 µs), at most `SUBMIT_BURST_MAX` per batch
  (8), and only while fewer than `SUBMIT_MAX_INFLIGHT` submissions (8) await
  the pool's answer, so a batch at difficulty 0.05 cannot flood the pool. A
  share whose job changes while it waits is discarded. `/metrics` gains
  `submit_queue_queued`, `_sent`, `_over_burst`, `_full`, `_expired`,
  `_max_depth`, `submit_inflight` and the three settings; the GPU candidates
  row shows what the queue sent. `SUBMIT_BURST_MAX=0` restores one share per
  batch. Measured on an RTX 3080 against the public pool, 10 minutes after a
  start: shares dropped by the gap 960 → 3, effective hashrate at 30 s 48% →
  94% of raw, no rejects and no disconnects; the limit of 8 unanswered
  submissions was reached only in peaks shorter than a second, while the
  difficulty was still below 1.
- `/metrics` fields for a configuration form, appended at the end:
  `gpu_devices` (index and name of every card on the OpenCL platform),
  `gpu_device_sel`, `threads_config` (`-1` = auto), `threads_auto`,
  `config_path`, and `config_cli`, the settings given on the command line.
- `POST /api/config` (control token): wallet, pool, port, worker, CPU threads
  and GPU selection, written into `config.env`, applied by a restart as for
  intensity. Every field is validated first — wallet format, pool name that
  resolves, port range, thread count, cards that exist — so a save cannot leave
  a config the miner will not start with. Other lines and comments are kept,
  the previous file is kept as `config.env.bak`, and settings given on the
  command line are refused, since they would override the file anyway.
- Dashboard, Settings: a **Mining configuration** block for wallet, pool and
  port, worker, CPU threads (auto shows what it resolves to and the cores
  detected) and graphics cards (all, one, or several, by name). The same checks
  as the miner's run before a save, only changed fields are sent, a new wallet
  or pool asks for confirmation, and settings pinned by the command line are
  shown but locked. Saving restarts the miner as an intensity change does.
- Dashboard: **Pause mining / Resume mining** in the header. The status chip
  now shows what the miner is doing — mining, connecting, pausing, paused —
  rather than only that it answers, and a banner with a Resume button stays up
  while mining is paused.
- `POST /api/pause` (control token), `{"paused": true|false}`: stops the
  mining threads and disconnects from the pool, while the dashboard stays up
  to resume. Refused while a clock test runs. Not saved: a restarted miner
  mines. `/metrics` gains `paused` and `mining_state` (`connecting`,
  `mining`, `pausing`, `paused`).
- Help page, CONFIG.md and INSTALL.md: Mining configuration, Pause, the new
  endpoints and fields, and what the token now allows.

### Changed
- **`SUBMIT_MIN_INTERVAL_MS` defaults to 5 ms instead of 20.** A GPU batch
  takes about 10 ms on an RTX 3080 and yields at most one share, so while the
  pool's difficulty is low — the first minutes of every connection — 20 ms
  dropped every other share. Replayed on 6103 shares from production: 34.6%
  dropped and 84.9% of raw work credited at 20 ms, 0.5% dropped at 5 ms and
  92.5% credited with no limit at all. A `config.env` that still sets
  `SUBMIT_MIN_INTERVAL_MS=20` (older `config.env.example`) keeps 20 until it is
  changed.
- **No more worker name.** The pool has no worker names and ignores the field,
  so it is gone from the dashboard's Mining configuration, `/api/config`, the
  installer's questions and `config.env.example`. An existing `WORKER=` line or
  `--worker` flag is still accepted, so old configs and command lines keep
  working; `install.sh --worker` warns that it is ignored.
- Mining configuration shows, under CPU threads, what the CPU adds right now
  as a share of the total — on a GPU rig well under one percent, which is why
  that setting barely matters there.
- **The dashboard can now change the wallet and the pool.** Until now
  `config.env` was never written from the browser, so nothing there could
  redirect payouts. Anyone holding the control token — and, with
  `METRICS_BIND` open to the network, able to reach the port — can now change
  them. Keep the token private and the port on localhost unless you need it.

### Fixed
- The first total hashrate after every reconnection to the pool counted all
  the hashes since the miner started, divided by ten seconds: a spike on the
  Total card and in the chart, the larger the longer the miner had run.
- After a restart requested from the dashboard (a config or intensity save),
  a miner that could not reach the pool took up to 10 more seconds to exit:
  the waits between connection attempts now end as soon as a stop is asked.
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
