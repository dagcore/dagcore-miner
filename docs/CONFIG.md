# Configuration reference

Every setting, option, endpoint and metrics field.

- [Where settings come from](#where-settings-come-from)
- [config.env keys](#configenv-keys)
- [overrides.env](#overridesenv)
- [Command-line options](#command-line-options)
- [Control API](#control-api)
- [/metrics fields](#metrics-fields)
- [/history](#history)

## Where settings come from

Three sources, later ones winning:

```
config.env   <   overrides.env   <   command line
```

- **`/etc/dagcore-miner/config.env`** — yours. The dashboard's Mining
  configuration also writes six keys in it (`WALLET`, `POOL`, `PORT`, `WORKER`,
  `THREADS`, `GPU_DEVICE`), keeping the previous file as `config.env.bak`.
- **`/var/lib/dagcore-miner/overrides.env`** — written by the dashboard's control
  API. Only tuning keys, never identity.
- **Command line** — an explicit flag beats both.

Autotune keys can additionally be set through environment variables of the same
name, which win over `config.env` but not over the command line.

Without `--config`, the miner looks for `config.env` in `<exedir>/`, then
`<exedir>/../`, then the working directory, then
`$HOME/dagtech-gpu-miner/` (a legacy location kept for older installs). The
service passes `--config` explicitly.

## config.env keys

Format is `KEY=value`, one per line. Blank lines and lines starting with `#` are
ignored. Values are taken literally — no quotes, no shell expansion.

### Pool

| Key | Values | Default | What it does |
|-----|--------|---------|--------------|
| `WALLET` | `0x` + 40 hex | *(none)* | Your payout address. The only required key. Sent as the Stratum username. |
| `POOL` | hostname | `stratum.dagcore.net` | Pool to connect to. |
| `PORT` | 1–65535 | `3334` | Stratum port. |
| `WORKER` | text | `dagcore` | Machine name. Sent in the Stratum password field; the current DagCore pool ignores it, because that pool build has no worker names. |
| `PASSWORD` | text | *(empty)* | Pool password. Shares the Stratum password field with `WORKER`, and `WORKER` wins when both are set. |

### CPU mining

| Key | Values | Default | What it does |
|-----|--------|---------|--------------|
| `THREADS` | `0`, `-1`, or N | `0` | `0` = no CPU mining (GPU only). Negative = auto-detect, half the logical cores. N = exactly N threads. |
| `CPU_LIMIT` | 1–100 | `100` | Percent of CPU time each thread may use. |
| `LOW_PRIORITY` | `0`/`1` | `0` | Run at the lowest scheduling priority. |

### Share submission

| Key | Values | Default | What it does |
|-----|--------|---------|--------------|
| `SUBMIT_MARGIN` | ≥ 1.0 | `1.0` | Multiplier on the share threshold. Higher submits fewer, safer shares. |
| `AUTO_THRESHOLD` | `0`/`1` | `1` | Raise the margin automatically after low-difficulty rejects. |
| `SUBMIT_MIN_INTERVAL_MS` | 0–1000 | `20` | Minimum gap between two share submissions. A share found inside it is dropped, never sent (`dropped` since start and `dropped_window` for the last 10 minutes in `/metrics`, and under Advanced on the dashboard; the total comes mostly from the first minutes after a start). `0` = no limit. Config file only, no command-line flag. |

### GPU

| Key | Values | Default | What it does |
|-----|--------|---------|--------------|
| `GPU_ENABLED` | `-1`/`0`/`1` | `-1` | `-1` auto, `0` off, `1` on. |
| `GPU_PLATFORM` | index | `0` | OpenCL platform index. |
| `GPU_DEVICE` | `all`, N, or `0,1` | `0` | Which cards to mine on. **`all` is what a multi-card rig wants** — hashrate adds up. The installer sets it for you. |
| `GPU_INTENSITY` | 0–100, or per card `80,60` | `80` | Work-items per batch, as 2^14 … 2^20. Always clamped to what VRAM holds, so past a point higher values change nothing. |
| `GPU_THROTTLE` | 1–100 | `100` | GPU duty-cycle limit, percent. |
| `GPU_ALIGN` | `pow2` or N | `pow2` | How the VRAM-fitted work-item count is rounded down. `pow2` measured fastest (see [DEVELOPMENT.md](DEVELOPMENT.md#gpu_align)). N must be a multiple of 32. |

### GPU tuning (usually written by the dashboard)

| Key | Values | Default | What it does |
|-----|--------|---------|--------------|
| `GPU_POWER_LIMIT` | watts | `0` | Applied with `nvidia-smi` at startup. `0` leaves the card alone. Must be inside the card's own min/max. |
| `GPU_CORE_CLOCK_BASE` | MHz | `0` | Core lock, as a **step of the base clock table**, not the frequency the card runs. Effective = step + offset. `0` = leave to the driver. |
| `GPU_MEM_CLOCK_BASE` | MHz | `0` | Same for memory. Effective = step + offset/2. |
| `GPU_CORE_OFFSET` | MHz, signed | *(unset)* | VF-curve offset, applied through NVML. Range comes from the card. |
| `GPU_MEM_OFFSET` | MHz, signed | *(unset)* | Same for memory. The clock moves by **half** the offset. |
| `GPU_CORE_CLOCK` | MHz | `0` | **Retired.** Absolute core clock. Read once to migrate an old config to `..._BASE`, then written back as 0. Do not set by hand. |
| `GPU_MEM_CLOCK` | MHz | `0` | **Retired**, same as above. |

A lock is stored as a table step rather than a frequency so that changing an
offset moves the lock with the table instead of stranding it.

### Metrics and dashboard

| Key | Values | Default | What it does |
|-----|--------|---------|--------------|
| `METRICS_PORT` | port | `8881` | HTTP port for `/metrics` and the dashboard. |
| `METRICS_BIND` | IPv4 | `127.0.0.1` | Interface to bind. `0.0.0.0` exposes the endpoint — including the full wallet, unauthenticated — to the network. |
| `DASHBOARD_DIR` | path | *(empty)* | Directory holding `index.html`. Empty means no dashboard. |

### Autotune (off by default)

| Key | Values | Default | What it does |
|-----|--------|---------|--------------|
| `AUTOTUNE` | `0`/`1` | `0` | Sweep work sizes and kernel modes once, cache the winner. |
| `AUTOTUNE_FORCE` | `0`/`1` | `0` | Retune even when a valid cache exists. |
| `AUTOTUNE_TRIAL_SECONDS` | 5–600 | `60` | Seconds per candidate. Below ~60 the measurement gets noisy. |
| `AUTOTUNE_BATCHES` | comma list | `1024,2048,4096,8192` | Work sizes to try. |
| `AUTOTUNE_KERNEL_MODES` | comma list | `split,legacy` | Kernel modes to try. |
| `AUTOTUNE_CACHE` | path | *(XDG cache)* | Result file. Empty resolves to `$XDG_CACHE_HOME/dagcore-miner/autotune.json`, falling back to `$HOME/.cache/...`. |
| `TARGET_BATCH_MS` | 100–5000 | `1500` | Batch duration above which the score penalises latency. |

## overrides.env

`/var/lib/dagcore-miner/overrides.env` is written by the control API and loaded
on top of `config.env`. Only these keys are ever written to it:

```
GPU_POWER_LIMIT   GPU_INTENSITY
GPU_CORE_CLOCK_BASE   GPU_MEM_CLOCK_BASE
GPU_CORE_OFFSET   GPU_MEM_OFFSET
```

Anything else in the file is ignored by the API and left alone. The wallet,
the pool and the other rig settings never go through this file: they are
written into `config.env` by `/api/config`, below.

Writes are atomic — a temporary file is renamed into place — and lines the API
does not manage are copied through unchanged.

You can edit it by hand, but you do not need to: everything in it is reachable
from the dashboard, and the dashboard will overwrite your edits next time it
writes. To discard all tuning, delete the file and restart.

## Command-line options

Every `config.env` key has an equivalent where it makes sense; the command line
always wins.

| Option | Equivalent | Notes |
|--------|-----------|-------|
| `--config PATH` | — | Which configuration file to read. |
| `--wallet ADDR` | `WALLET` | |
| `--pool HOST` | `POOL` | |
| `--port N` | `PORT` | |
| `--worker NAME` | `WORKER` | |
| `--password PW` | `PASSWORD` | |
| `--threads N` | `THREADS` | |
| `--cpu-limit N` | `CPU_LIMIT` | |
| `--low-priority` | `LOW_PRIORITY=1` | |
| `--submit-margin F` | `SUBMIT_MARGIN` | |
| `--no-auto-threshold` | `AUTO_THRESHOLD=0` | |
| `--gpu` | `GPU_ENABLED=1` | |
| `--no-gpu` | `GPU_ENABLED=0` | |
| `--gpu-platform N` | `GPU_PLATFORM` | |
| `--gpu-device WHICH` | `GPU_DEVICE` | `all`, N, or `0,1` |
| `--gpu-intensity N` | `GPU_INTENSITY` | also `80,60` per card |
| `--gpu-throttle N` | `GPU_THROTTLE` | |
| `--gpu-align WHICH` | `GPU_ALIGN` | `pow2` or a multiple of 32 |
| `--metrics-port N` | `METRICS_PORT` | |
| `--metrics-bind IP` | `METRICS_BIND` | |
| `--dashboard-dir DIR` | `DASHBOARD_DIR` | |
| `--save-config` | — | Write current settings to the config file and exit. |
| `--help`, `-h` | — | |

An unrecognised option, or a known option with no value, stops the miner with a
message. It is not ignored — a typo in a service file used to sit unnoticed.

## Control API

HTTP POST on the metrics port. All endpoints need the token from
`/etc/dagcore-miner/api-token` in an `X-DagCore-Token` header.

Bodies and replies are JSON. Every reply carries `ok`.

| Endpoint | Body | Effect |
|----------|------|--------|
| `/api/power-limit` | `{"watts":280}` or `{"reset":true}` | Applies immediately and saves. `reset` restores the card's own default, not 0. Restarts any running trial. |
| `/api/core-clock` | `{"mhz":1800}` or `{"reset":true}` | Locks the core. Starts a 10-minute trial. `reset` hands the domain back to the driver and saves at once. |
| `/api/mem-clock` | `{"mhz":9851}` or `{"reset":true}` | Same for memory. |
| `/api/core-offset` | `{"mhz":150}` or `{"reset":true}` | VF offset through NVML. Starts a trial. Re-applies any clock lock at its step. |
| `/api/mem-offset` | `{"mhz":1200}` or `{"reset":true}` | Same for memory. |
| `/api/intensity` | `{"value":90}` | Saves and exits so the supervisor restarts the miner — intensity is fixed when buffers are allocated. Replies `restarting:false` when not supervised. |
| `/api/cancel-trial` | `{"domain":"mem-clock"}` | Ends a running trial and puts the previous value back. Domains: `core-clock`, `mem-clock`, `core-offset`, `mem-offset`. |
| `/api/config` | Any of `{"wallet","pool","port","worker","threads","gpu_device"}` | Validates every field, writes them into `config.env` (previous file kept as `config.env.bak`, other lines untouched) and exits for a restart like `/api/intensity`. Unchanged values answer `saved:false`. `threads` is `-1` (auto) to the core count, `0` only while a GPU mines; `gpu_device` is `all`, `N` or `N,M`. Works even when the tuning controls are unavailable. |
| `/api/pause` | `{"paused":true}` or `{"paused":false}` | Stops the mining threads and disconnects from the pool, or resumes. The web server keeps running. Not saved across restarts. Refused during a trial. Works even when the tuning controls are unavailable. |
| `/api/adopt` | `{}`, `{"core":true}`, `{"mem":true}` | Saves what the card runs now, with no trial. Offsets always; clock locks only when named, because NVML cannot report a lock. |

### Response codes

| Code | Means |
|------|-------|
| 200 | Done. |
| 400 | The body is missing a field, or a value is out of range or invalid (a wallet, a pool that does not resolve, a card that does not exist). |
| 401 | Missing or wrong token. |
| 409 | Controls unavailable — GPU off, multi-GPU, no `nvidia-smi`, no NVML, or no trial to cancel. For `/api/config`: the setting is given on the command line. For `/api/pause`: a trial is running. |
| 500 | The driver refused, or the settings file could not be written. The driver's message is passed through. |

### The trial

A clock or offset change is applied live but **not saved** until it survives ten
minutes without the rejected-share counter moving. If rejects appear, the
previous value is put back automatically and nothing is written. Until the trial
passes, nothing on disk mentions the new value — so any restart lands on the
last setting that proved itself.

Power limit and adopt have no trial: the first is reversible and harmless, the
second describes a state the card has been in all along.

## /metrics fields

`GET /metrics` returns one JSON object, 102 fields. No authentication. New
fields are only ever appended at the end, so the order of the existing ones
does not change.

### Identity
`version`, `pool`, `worker`, `wallet` (abbreviated), `wallet_full`,
`cpu_name`, `cpu_cores`, `threads`, `gpu_enabled`, `gpu_count`,
`gpu_intensity` (the value in force, after VRAM clamping)

### Hashrate
`hashrate`, `cpu_hashrate`, `gpu_hashrate` — H/s, averaged over the last ten
seconds. `gpu_hashrates` — array, one per card. `total_hashes` — since this
process started.

### Effective hashrate
Over a sliding window of the last 10 minutes — shorter during the first 10
minutes after a start:

| Field | Means |
|-------|-------|
| `effective_hashrate` | Accepted work as H/s: each accepted share counted at the pool difficulty of its job, × 65537 hashes per difficulty-1 share |
| `effective_raw_hashrate` | Raw hashrate averaged over the same window |
| `effective_pct` | The first as a percentage of the second |
| `effective_window_s` | The window's length in seconds; 595–600 once full |
| `effective_window_full` | `true` once the window no longer reaches back to the start. Use this, not `effective_window_s >= 600` |

The 65537 is not Bitcoin's 2^32: it comes from this miner's share target, see
[DEVELOPMENT.md](DEVELOPMENT.md#traps).

### Shares
`submitted`, `accepted`, `rejected`, `stale`, `dropped`, and the same five split
as `cpu_*` and `gpu_*`. `dropped` counts shares discarded by the submission rate
limiter, since start; most of it comes from the first minutes after a start,
while the pool's difficulty is still low. `dropped_window` and
`submitted_window` are the same two counts over the effective hashrate's
window. `submit_min_interval_ms` is the gap in force (`SUBMIT_MIN_INTERVAL_MS`).

### Health
`cpu_temp`, `gpu_temp`, `gpu_usage`, `gpu_memory`, `gpu_power` — `-1` when a
reading is unavailable. GDDR memory temperature is not among them: the Linux
driver does not expose it.

### Power limit
`gpu_power_limit` (current), `gpu_power_min`, `gpu_power_max`,
`gpu_power_default` — all watts, read from the card.

### Clocks
For each of `gpu_core_clock` and `gpu_mem_clock`:

| Suffix | Means |
|--------|-------|
| *(none)* | What the card runs right now |
| `_min`, `_max` | The range a lock may use, offset-aware |
| `_boost` | The ceiling the card advertises |
| `_lock` | The effective frequency of the saved lock, 0 for none |
| `_base` | The saved lock as a table step |
| `_shift` | How far the offset displaces the table |

### Offsets
`gpu_core_offset`, `gpu_mem_offset`, each with `_min` and `_max` from the card.
`offset_available` and `offset_reason` say whether NVML could be used.

### Trials
For each of `trial_core`, `trial_mem`, `trial_coreoff`, `trial_memoff`:

| Suffix | Means |
|--------|-------|
| `_status` | `none`, `running`, `saved`, `failed` |
| `_value` | The value under test |
| `_prev` | What a failure reverts to |
| `_remaining` | Seconds left |
| `_rejected` | Rejected shares since the trial began |

### Control state
`control_available`, `control_reason` — whether the tuning controls can work and
why not. `clock_lock_stale` — a saved step whose frequency now falls outside the
card's range.

### Session
`difficulty`, `uptime` (seconds, this process), `job_id`

### Configuration
What the dashboard's Mining configuration form shows:

| Field | Means |
|-------|-------|
| `gpu_devices` | `[{"index":0,"name":"NVIDIA GeForce RTX 3080"}, ...]`, every card on the OpenCL platform, empty in the CPU-only build |
| `gpu_device_sel` | `GPU_DEVICE` as it would be written: `all`, `0,1` or `0` |
| `threads_config` | `THREADS` as configured, `-1` for auto; `threads` is what it resolved to |
| `threads_auto` | What auto resolves to on this machine |
| `config_path` | The `config.env` this process loaded, and the one a save writes |
| `config_cli` | Settings given on the command line, which `/api/config` refuses: any of `wallet`, `pool`, `port`, `worker`, `threads`, `gpu_device` |

### Mining state
`paused` — whether a pause is requested. `mining_state` — `connecting`,
`mining`, `pausing` (requested, the session still winding down) or `paused`.

## /history

`GET /history` returns the last 30 minutes of hashrate, kept by the miner in
memory: one sample every 5 seconds, at most 360. It is empty after a miner
restart and never written to disk. No authentication.

```json
{"interval_s":5,"now":1790136380,"samples":[
  {"t":1790136372,"hashrate":1650420.00,"gpu_hashrate":1643315.20,
   "cpu_hashrate":7104.80,"effective_hashrate":1641003.50}, ...]}
```

Samples are oldest first. `t` and `now` are Unix seconds on the miner's clock;
a client should place samples by `now - t` rather than trust its own clock.
`effective_hashrate` is `null` until the effective window is full.
