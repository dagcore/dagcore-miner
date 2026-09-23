/*
 * DagTech GPU Miner - High Performance CPU+GPU Mining Engine
 * Copyright (c) 2024-2026 DagTech Ltd / Dawie Nel
 * Portions Copyright (c) 2026 DagCore Community
 * https://dagtech.network
 *
 * Licensed under the MIT License.
 * Custom implementation of Modified Scrypt (N=1024, r=1, p=1)
 * with proprietary post-ROMix transformation.
 *
 * GPU support via OpenCL (compile with -DDAGTECH_GPU -lOpenCL).
 * Without -DDAGTECH_GPU, builds as a pure CPU miner identical to
 * the original dagtech-miner.
 *
 * Stratum protocol compatible with standard mining pools.
 *
 * Author:  Dawie Nel <dawie@dagtech.network>
 * Project: DagTech Mining Suite
 * Version: DagCore 1.1.0 (derived from DagTech GPU-2026.0628.1)
 */

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    typedef int ssize_t;
  #endif
  #define close closesocket
  #define usleep(x) Sleep((x)/1000)
  #define sleep(x) Sleep((x)*1000)
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>   /* TCP_NODELAY (#44) */
  #include <sys/socket.h>
  #include <unistd.h>
  #ifdef __APPLE__
    #include <sys/sysctl.h>
  #endif
#endif

#ifdef USE_OPENSSL
  #include <openssl/sha.h>
  #define DT_SHA256(data, len, out)       SHA256(data, len, out)
  #define DT_SHA256_CTX                   SHA256_CTX
  #define DT_SHA256_Init(ctx)             SHA256_Init(ctx)
  #define DT_SHA256_Update(ctx, d, l)     SHA256_Update(ctx, d, l)
  #define DT_SHA256_Final(out, ctx)       SHA256_Final(out, ctx)
#else
  #include "dagcore_sha256.h"
  #define DT_SHA256(data, len, out)       dagtech_sha256(data, len, out)
  #define DT_SHA256_CTX                   DAGTECH_SHA256_CTX
  #define DT_SHA256_Init(ctx)             dagtech_sha256_init(ctx)
  #define DT_SHA256_Update(ctx, d, l)     dagtech_sha256_update(ctx, d, l)
  #define DT_SHA256_Final(out, ctx)       dagtech_sha256_final(ctx, out)
#endif

#ifdef DAGTECH_GPU
  #ifdef __APPLE__
    #include <OpenCL/opencl.h>
  #else
    #include <CL/cl.h>
  #endif
#endif

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#ifndef _WIN32
  #include <dlfcn.h>
#endif
#include <time.h>
#include <errno.h>
#include <math.h>
#ifndef _WIN32
  #include <sys/stat.h>
#endif

/* CPU brand-string detection (CPUID on x86; sysctl on macOS). */
#if defined(_MSC_VER)
  #include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  #include <cpuid.h>
#endif

#ifdef _WIN32
  #define DT_PRIu64 "I64u"
#else
  #define DT_PRIu64 "llu"
#endif

/* =========================================================================
 * DagTech GPU Miner Configuration
 * ========================================================================= */
/* DagCore versioning restarts at 1.0.0; derived from DagTech GPU-2026.0628.1. */
#define DAGTECH_VERSION       "1.1.0"
#define DAGTECH_BANNER        "DagCore Miner v" DAGTECH_VERSION " - dagcore.net"
#define DAGTECH_AUTHOR        "Dawie Nel / DagTech Ltd"
#define DAGTECH_DEFAULT_POOL  "stratum.dagcore.net"
#define DAGTECH_DEFAULT_PORT  3334

/* Scrypt parameters - fixed for this algorithm */
#define SCRYPT_N  1024
#define SCRYPT_R  1
#define SCRYPT_P  1

/* =========================================================================
 * Runtime State
 * ========================================================================= */
static char pool_host[256]     = DAGTECH_DEFAULT_POOL;
static int  pool_port          = DAGTECH_DEFAULT_PORT;
static char wallet[128]        = "";
static char worker_name[64]    = "dagcore";
static char password[32]       = "";
static int  num_threads        = 0;  /* CPU mining threads: 0 = none (GPU-only, the
                                        default), <0 = auto-detect, N = exactly N */
static int  cpu_priority       = 0;  /* 0=normal, 1=low */
static int  cpu_limit          = 100; /* 1-100: % of CPU time to use per thread */
static int  gpu_throttle       = 100; /* 1-100: % of GPU time to use (duty-cycle throttle) */
static volatile int running    = 1;
static volatile int keep_alive = 1;  /* 0 = clean program exit; stays 1 across reconnects */
static int  metrics_port       = 8881;  /* built-in metrics/dashboard endpoint */
/* METRICS_BIND / --metrics-bind: interface the metrics + dashboard server binds.
 * Loopback by default. Until this existed the server bound INADDR_ANY, so the
 * endpoint - which serves wallet_full, pool, worker and hashrate with no
 * authentication - was reachable from the whole LAN. Pass 0.0.0.0 to get the
 * old behaviour back deliberately. */
static char metrics_bind[64]   = "127.0.0.1";

/* Validate a --metrics-bind / METRICS_BIND value: a dotted-quad IPv4 address on
 * this host. Returns 0 and stores it, or -1 if inet_addr() refuses it. */
static int metrics_parse_bind(const char *val) {
    if (!val || !val[0]) return -1;
    /* INADDR_NONE doubles as the error return, so 255.255.255.255 needs the
     * explicit pass - binding it is useless, but rejecting it as "malformed"
     * would be a lie. */
    if (inet_addr(val) == INADDR_NONE && strcmp(val, "255.255.255.255") != 0) return -1;
    strncpy(metrics_bind, val, sizeof(metrics_bind) - 1);
    metrics_bind[sizeof(metrics_bind) - 1] = '\0';
    return 0;
}

static void metrics_bind_reject(const char *val, const char *origin) {
    fprintf(stderr,
            "[DagCore] ERROR: invalid %s value \"%s\".\n"
            "          Use an IPv4 address: 127.0.0.1 (default), 0.0.0.0 for the\n"
            "          whole LAN, or the address of one interface.\n",
            origin, val ? val : "");
    exit(1);
}
static char dashboard_dir[512] = "";

/* ---- Control API (#46) --------------------------------------------------
 * A token-protected POST endpoint on the metrics server that can change the
 * GPU power limit and the mining intensity from the dashboard.
 *
 * Two files, deliberately separate:
 *   /etc/dagcore-miner/config.env       operator's, the miner only reads it
 *   /var/lib/dagcore-miner/overrides.env written by this API, loaded on top
 * so a dashboard change can never corrupt the wallet or the pool, and an
 * operator editing config.env never fights the API over the same file.
 *
 * Both paths, and the token file, can be redirected with environment
 * variables - needed to test without root, useful for containers. */
#define DT_TOKEN_PATH_DEFAULT     "/etc/dagcore-miner/api-token"
#define DT_OVERRIDES_PATH_DEFAULT "/var/lib/dagcore-miner/overrides.env"

static int  gpu_power_limit = 0;      /* GPU_POWER_LIMIT, watts; 0 = leave alone */
/* A lock is stored as a step of the card's BASE clock table, not as the MHz
 * the card ends up running. An offset displaces the whole table, so an
 * absolute value goes stale the moment the offset changes - which is how
 * adjusting Memory offset used to raise a warning on the Memory clock row
 * nobody had touched. The step survives; the effective frequency is derived
 * from it as step + shift, and the lock is re-applied automatically.
 * GPU_CORE_CLOCK / GPU_MEM_CLOCK are the retired absolute keys, still read so
 * an existing config can be migrated once. */
static int  gpu_core_clock_base = 0;  /* GPU_CORE_CLOCK_BASE, MHz in the base table */
static int  gpu_mem_clock_base  = 0;  /* GPU_MEM_CLOCK_BASE */
static int  gpu_core_clock  = 0;      /* GPU_CORE_CLOCK, legacy absolute MHz */
/* Clock offsets are signed and 0 is a meaningful value, so "not configured"
 * needs its own sentinel rather than 0. */
#define DT_OFF_UNSET (-1000000)
static int  gpu_core_offset = DT_OFF_UNSET;   /* GPU_CORE_OFFSET, MHz */
static int  gpu_mem_offset  = DT_OFF_UNSET;   /* GPU_MEM_OFFSET, MHz */
static int  gpu_mem_clock   = 0;      /* GPU_MEM_CLOCK, legacy absolute MHz */
/* Last-known clock envelope, refreshed from NVML on every control request.
 * It is NOT static: a memory offset displaces the whole table, so the ceiling
 * moves without a restart.
 *
 * An earlier version read this from --query-supported-clocks and treated
 * clocks.max.* as a mere boost figure. That was backwards. The supported table
 * is the BASE one, before any offset; clocks.max.* (and NVML's
 * GetMaxClockInfo) already account for the offset. On rig1, with a +1200
 * memory offset, the base table said 9501 while the card was happily running a
 * 9851 MHz lock. */
static int  g_core_lo = -1, g_core_hi = -1, g_core_boost = -1;
static int  g_mem_lo  = -1, g_mem_hi  = -1, g_mem_boost  = -1;
static char g_api_token[80] = "";     /* empty = control API disabled */
static int  g_control_ok    = 0;      /* 1 = controls usable right now */
static char g_control_reason[128] = "not initialised";
/* Power-limit envelope as nvidia-smi reports it; -1 until queried. */
static double g_pl_min = -1, g_pl_max = -1, g_pl_default = -1, g_pl_current = -1;

static const char *dt_token_path(void) {
    const char *e = getenv("DAGCORE_TOKEN_FILE");
    return (e && e[0]) ? e : DT_TOKEN_PATH_DEFAULT;
}
static const char *dt_overrides_path(void) {
    const char *e = getenv("DAGCORE_OVERRIDES_FILE");
    return (e && e[0]) ? e : DT_OVERRIDES_PATH_DEFAULT;
}

/* Detected host CPU, shown at startup and in the metrics JSON. */
static char g_cpu_brand[96]    = "";
static int  g_cpu_cores        = 0;   /* total logical processors found */

/* GPU configuration */
static int gpu_enabled   = -1;  /* -1=auto, 0=disabled, 1=enabled */
static int gpu_intensity = 80;  /* 0-100 */
/* GPU_ALIGN / --gpu-align: how the VRAM-fitted work-item count is rounded down.
 * 0 = "pow2" (the default); N > 0 rounds to a multiple of N instead, which
 * keeps more work-items than pow2 would.
 *
 * pow2 is the default on measurement, not on tradition. On an RTX 3080
 * (2.4 GB budget -> 19754 work-items exact) at intensity 80:
 *
 *     pow2      16384 work-items   1.614 MH/s
 *     align 256 19712 work-items   1.494 MH/s   (-7.4%)
 *     exact     19754 work-items   1.472 MH/s   (-8.8%)
 *
 * So the extra ~20% of work-items cost throughput rather than adding it -
 * at this scratchpad size the card is memory-bound and the bigger V buffer
 * (2.4 GB vs 2.0 GB) hurts more than the added parallelism helps. Alignment
 * alone recovers only ~1.5% of that (19712 vs 19754), so a well-shaped
 * non-power-of-two does not rescue it either. Keep pow2 unless a different
 * card measures otherwise - and measure, don't assume.
 *
 * N must be a multiple of 32 so the result is a whole number of
 * warps/wavefronts, and so coop mode - which enqueues global_size * 4 with a
 * local size of 32, and therefore needs global_size % 8 == 0 - stays legal. */
static int gpu_align     = 0;

/* Parse a --gpu-align / GPU_ALIGN value: "pow2", or a positive multiple of 32.
 * Sets gpu_align and returns 0; returns -1 on a value we refuse. */
static int gpu_parse_align(const char *val) {
    if (!val || !val[0]) return -1;
    if (strcmp(val, "pow2") == 0) { gpu_align = 0; return 0; }
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (end == val || *end != '\0') return -1;
    if (n < 32 || n > (1 << 20) || (n % 32) != 0) return -1;
    gpu_align = (int)n;
    return 0;
}

/* Bad --gpu-align / GPU_ALIGN: fail loudly at startup rather than mine with a
 * work size the user did not ask for. */
static void gpu_align_reject(const char *val, const char *origin) {
    fprintf(stderr,
            "[DagCore] ERROR: invalid %s value \"%s\".\n"
            "          Use \"pow2\" (default) or a multiple of 32 (e.g. 256, 1024).\n",
            origin, val ? val : "");
    exit(1);
}

static int gpu_platform  = 0;
static int gpu_device    = 0;   /* single-GPU fallback */

/* Multi-GPU: GPU_DEVICE=0,1 or GPU_DEVICE=all selects devices from gpu_platform */
#define MAX_GPUS 8
static int gpu_device_list[MAX_GPUS];
static int gpu_device_count    = 0;  /* 0 = use gpu_device (single) */
static int gpu_use_all         = 0;  /* 1 = use every GPU on the platform */
static int g_num_gpus          = 0;  /* GPUs successfully initialised */

/* What the dashboard's configuration form needs to know, beyond the values
 * themselves: the cards on gpu_platform by name, the thread count "auto"
 * resolves to, the config file a save would write, and which settings came
 * from the command line - those win over config.env on every start, so the
 * form must not pretend to change them. */
static char g_gpu_names[MAX_GPUS][128];
static int  g_gpu_detected = 0;      /* GPUs on gpu_platform (gpu_list_devices) */
static int  g_threads_cfg  = 0;      /* THREADS / --threads before -1 is resolved */
static int  g_threads_auto = 0;      /* what -1 resolves to on this machine */
static const char *g_config_path = "";
#define CLI_WALLET     0x01
#define CLI_POOL       0x02
#define CLI_PORT       0x04
#define CLI_WORKER     0x08
#define CLI_THREADS    0x10
#define CLI_GPU_DEVICE 0x20
static int  g_cli_set = 0;

/* Per-GPU intensity: GPU_INTENSITY=80,60 overrides the single global per card */
static int gpu_intensity_list[MAX_GPUS];
static int gpu_intensity_count = 0;  /* 0 = use gpu_intensity for all GPUs */

/* #42 GPU work-size autotune (config in config.env or env-var override).
 * When AUTOTUNE=1, the GPU thread runs short trials at startup across the
 * Cartesian product of AUTOTUNE_BATCHES x AUTOTUNE_KERNEL_MODES, scores each
 * (reference miner's formula: useful_hashrate * (1 - penalties)), and picks
 * the winner. Result is cached at AUTOTUNE_CACHE and re-used on next start
 * unless AUTOTUNE_FORCE=1 or the cache key no longer matches this GPU/config. */
static int  gpu_autotune                = 0;     /* AUTOTUNE: 0=off (default), 1=on */
static int  gpu_autotune_force          = 0;     /* AUTOTUNE_FORCE: 0=use cache if valid, 1=always retune */
static int  gpu_autotune_trial_seconds  = 60;    /* AUTOTUNE_TRIAL_SECONDS: per-candidate trial duration (reference miner uses 60 minimum for noise reduction) */
static int  gpu_target_batch_ms         = 1500;  /* TARGET_BATCH_MS: latency penalty threshold for scoring */
static char gpu_autotune_batches[256]   = "1024,2048,4096,8192";   /* AUTOTUNE_BATCHES: comma list */
static char gpu_autotune_modes[64]      = "split,legacy";          /* AUTOTUNE_KERNEL_MODES: comma list */
static char gpu_autotune_cache[512]     = "";   /* AUTOTUNE_CACHE: file path; empty = resolved
                                                   at startup, see dagtech_default_autotune_cache() */

/* Stratum connection */
static int sockfd = -1;
static pthread_mutex_t sock_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t job_mtx   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stats_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Mining statistics */
static uint64_t total_hashes    = 0;
static uint64_t total_submitted = 0;
static uint64_t total_accepted  = 0;
static uint64_t total_rejected  = 0;
static uint64_t total_stale     = 0;
static uint64_t cpu_submitted   = 0;
static uint64_t gpu_submitted   = 0;
static uint64_t cpu_accepted    = 0;
static uint64_t gpu_accepted    = 0;
static uint64_t cpu_rejected    = 0;
static uint64_t gpu_rejected    = 0;
static uint64_t cpu_stale       = 0;
static uint64_t gpu_stale       = 0;

/* Pending submission ring buffer: maps submission id -> source (CPU=0, GPU=1)
   so that pool accept/reject responses can be attributed to the right source. */
#define PENDING_SUB_MAX 64
static struct { uint64_t id; int is_gpu; double diff; } pending_subs[PENDING_SUB_MAX];
static int pending_head  = 0;
static int pending_count = 0;
static pthread_mutex_t pending_mtx = PTHREAD_MUTEX_INITIALIZER;
static double   current_hashrate = 0.0;
static double   cpu_hashrate     = 0.0;
static double   gpu_hashrate     = 0.0;
static time_t   start_time;

/* Hashrate history for the dashboard chart, served at /history. The page used
 * to keep these samples itself and lost them on every reload. One sample every
 * 5s for 30 minutes: 360 x 32 bytes = 11.5 KB, fixed, in .bss. Memory only -
 * a miner restart starts an empty chart, which is the intent. */
#define HIST_INTERVAL_S 5
#define HIST_LEN        360
typedef struct {
    int64_t  t;
    double   total, gpu, cpu;      /* served at /history */
    double   effective;            /* ditto; < 0 = window not full yet */
    uint64_t hashes;               /* counters then - not served; they */
    double   accepted_work;        /* feed the 10-minute window below */
    uint64_t submitted, dropped;
} hist_sample_t;
static hist_sample_t   hist_buf[HIST_LEN];
static int             hist_head  = 0;   /* next slot to write */
static int             hist_count = 0;
static pthread_mutex_t hist_mtx   = PTHREAD_MUTEX_INITIALIZER;

/* Effective hashrate: the work the pool accepted, as hashes. A share passes
 * when the top 64 bits of its hash are <= 0xFFFF00000000 / difficulty (see
 * dagtech_check_target), so one share at difficulty 1 takes 2^64/0xFFFF00000000
 * = ~65537 hashes on average - not the 2^32 of Bitcoin's difficulty 1. Checked
 * against 682 accepted shares in the production log: raw 1.65 MH/s, effective
 * 1.56 MH/s (94%); with 2^32 it would have read 1e11 H/s. Each share counts at
 * the pool difficulty of the job it was found for, without the submit margin -
 * that is what the pool credits. */
#define HASHES_PER_DIFF1 (18446744073709551616.0 / (double)0x0000FFFF00000000ULL)
#define EFFECTIVE_WINDOW_S 600
static double accepted_work = 0.0;         /* cumulative, under stats_mtx */

/* The counters above, over the last EFFECTIVE_WINDOW_S (see stats_window). */
typedef struct {
    double   eff, raw;             /* H/s */
    long     span_s;
    int      full;                 /* the whole window is past start_time */
    uint64_t submitted, dropped;
} window_stats_t;

/* Per-session hash counters for hashrate tracking */
static uint64_t cpu_hashes_session = 0;
static uint64_t gpu_hashes_session = 0;
static pthread_mutex_t cpu_stats_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gpu_stats_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int      valid;
    uint64_t seq;
    char     job_id[128];
    char     prevhash[256];
    char     version[16];
    char     bits[16];
    char     ntime[16];
    char     extranonce1[16];
    double   difficulty;
} DagTechJob;

static DagTechJob current_job = {0};
static char extranonce1_global[16] = "";
static double current_difficulty = 0.01;

/* Adaptive submit margin. margin >= 1.0 tightens the share threshold so we
 * submit fewer, higher-quality shares. Default 1.0 = identical to old behaviour;
 * with auto_threshold on it only rises if the pool actually rejects shares as
 * "low difficulty". Mirrors the reference miner's SUBMIT_MARGIN/AUTO_THRESHOLD. */
static double submit_margin  = 1.0;  /* base, from SUBMIT_MARGIN / --submit-margin */
static double active_margin  = 1.0;  /* dynamic; rises after low-difficulty rejects */
static int    auto_threshold = 1;    /* 1 = raise active_margin on lowdiff rejects */

static double dagtech_effective_diff(double base) {
    double m = active_margin;
    if (m < 1.0)    m = 1.0;
    if (base <= 0.0) base = 1.0;
    return base * m;
}

/* =========================================================================
 * Utility Functions - DagTech Implementation
 * ========================================================================= */
#define DAGTECH_SWAB32(x) (((x)>>24)|(((x)>>8)&0xff00)|(((x)<<8)&0xff0000)|((x)<<24))

static void hex_to_bytes(const char *hex, uint8_t *out, int len) {
    for (int i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(hex + 2 * i, "%2x", &byte);
        out[i] = (uint8_t)byte;
    }
}

static void bytes_to_hex(const uint8_t *data, int len, char *out) {
    for (int i = 0; i < len; i++)
        sprintf(out + 2 * i, "%02x", data[i]);
    out[2 * len] = 0;
}

/* Millisecond wall-clock timer for CPU throttle */
static long long dagtech_tick_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* High-resolution millisecond clock (QueryPerformanceCounter on Windows).
 * Forward-declared here; defined near the share submitter below. Used by the
 * GPU loop to sanity-check batch duration. */
static uint64_t dagtech_now_ms(void);

static void sha256d(const uint8_t *data, int len, uint8_t *out) {
    uint8_t h1[32];
    DT_SHA256(data, len, h1);
    DT_SHA256(h1, 32, out);
}

/* =========================================================================
 * DagTech Scrypt Engine (N=1024, r=1, p=1)
 * cpuminer-compatible scrypt_1024_1_1_256 implementation
 * ========================================================================= */

static const uint32_t dagtech_sha256_iv[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

/* cpuminer PBKDF2 padding constants */
static const uint32_t scrypt_keypad[12]  = {
    0x80000000,0,0,0,0,0,0,0,0,0,0,0x00000280
};
static const uint32_t scrypt_innerpad[11] = {
    0x80000000,0,0,0,0,0,0,0,0,0,0x000004a0
};
static const uint32_t scrypt_outerpad[8] = {
    0x80000000,0,0,0,0,0,0,0x00000300
};
static const uint32_t scrypt_finalblk[16] = {
    0x00000001,0x80000000,0,0,0,0,0,0,0,0,0,0,0,0,0,0x00000620
};

/* SHA256 compression on uint32 words; swap=0: W[i]=block[i] (LE), swap=1: W[i]=bswap(block[i]) (BE) */
static void dagtech_sha256_xform(uint32_t state[8], const uint32_t block[16], int swap) {
    uint32_t W[64];
    if (swap)
        for (int i = 0; i < 16; i++) W[i] = DAGTECH_SWAB32(block[i]);
    else
        for (int i = 0; i < 16; i++) W[i] = block[i];
    for (int i = 16; i < 64; i++)
        W[i] = DAGTECH_SIG1(W[i-2]) + W[i-7] + DAGTECH_SIG0(W[i-15]) + W[i-16];
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
    uint32_t e=state[4],f=state[5],g=state[6],h=state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + DAGTECH_EP1(e) + DAGTECH_CH(e,f,g) + dagtech_sha256_k[i] + W[i];
        uint32_t t2 = DAGTECH_EP0(a) + DAGTECH_MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

/* HMAC-SHA256 init for 80-byte key: computes tstate (inner) and ostate (outer) */
static void dagtech_hmac80_init(const uint32_t *key, uint32_t *tstate, uint32_t *ostate) {
    uint32_t ihash[8], pad[16];
    int i;
    /* Finish inner hash: tstate already has midstate (SHA256 of key[0..63]); process key[64..79] + keypad */
    memcpy(pad, key + 16, 16);
    memcpy(pad + 4, scrypt_keypad, 48);
    dagtech_sha256_xform(tstate, pad, 0);
    memcpy(ihash, tstate, 32);
    /* ostate = SHA256_IV XOR'd with (ihash XOR opad_const) */
    memcpy(ostate, dagtech_sha256_iv, 32);
    for (i = 0; i < 8; i++) pad[i] = ihash[i] ^ 0x5c5c5c5c;
    for (; i < 16; i++) pad[i] = 0x5c5c5c5c;
    dagtech_sha256_xform(ostate, pad, 0);
    /* tstate = SHA256_IV XOR'd with (ihash XOR ipad_const) */
    memcpy(tstate, dagtech_sha256_iv, 32);
    for (i = 0; i < 8; i++) pad[i] = ihash[i] ^ 0x36363636;
    for (; i < 16; i++) pad[i] = 0x36363636;
    dagtech_sha256_xform(tstate, pad, 0);
}

/* PBKDF2 phase 1: 80-byte password/salt -> 128-byte X */
static void dagtech_pbkdf2_80_128(const uint32_t *tstate, const uint32_t *ostate,
                                   const uint32_t *salt, uint32_t *output) {
    uint32_t istate[8], ostate2[8], ibuf[16], obuf[16];
    memcpy(istate, tstate, 32);
    dagtech_sha256_xform(istate, salt, 0);
    memcpy(ibuf, salt + 16, 16);
    memcpy(ibuf + 5, scrypt_innerpad, 44);
    memcpy(obuf + 8, scrypt_outerpad, 32);
    for (int i = 0; i < 4; i++) {
        memcpy(obuf, istate, 32);
        ibuf[4] = i + 1;
        dagtech_sha256_xform(obuf, ibuf, 0);
        memcpy(ostate2, ostate, 32);
        dagtech_sha256_xform(ostate2, obuf, 0);
        for (int j = 0; j < 8; j++)
            output[8*i + j] = DAGTECH_SWAB32(ostate2[j]);
    }
}

/* PBKDF2 phase 2: 128-byte X -> 32-byte output */
static void dagtech_pbkdf2_128_32(uint32_t *tstate, uint32_t *ostate,
                                   const uint32_t *salt, uint32_t *output) {
    uint32_t buf[16];
    dagtech_sha256_xform(tstate, salt,      1);
    dagtech_sha256_xform(tstate, salt + 16, 1);
    dagtech_sha256_xform(tstate, scrypt_finalblk, 0);
    memcpy(buf, tstate, 32);
    memcpy(buf + 8, scrypt_outerpad, 32);
    dagtech_sha256_xform(ostate, buf, 0);
    for (int i = 0; i < 8; i++)
        output[i] = DAGTECH_SWAB32(ostate[i]);
}

static void dagtech_xor_salsa8(uint32_t B[16], const uint32_t Bx[16]) {
    uint32_t x00=(B[0]^=Bx[0]),  x01=(B[1]^=Bx[1]),  x02=(B[2]^=Bx[2]),  x03=(B[3]^=Bx[3]);
    uint32_t x04=(B[4]^=Bx[4]),  x05=(B[5]^=Bx[5]),  x06=(B[6]^=Bx[6]),  x07=(B[7]^=Bx[7]);
    uint32_t x08=(B[8]^=Bx[8]),  x09=(B[9]^=Bx[9]),  x10=(B[10]^=Bx[10]), x11=(B[11]^=Bx[11]);
    uint32_t x12=(B[12]^=Bx[12]), x13=(B[13]^=Bx[13]), x14=(B[14]^=Bx[14]), x15=(B[15]^=Bx[15]);
    #define ROTL(a,c) (((a)<<(c)) | ((a)>>(32-(c))))
    for (int i = 0; i < 8; i += 2) {
        x04^=ROTL(x00+x12,7);  x09^=ROTL(x05+x01,7);
        x14^=ROTL(x10+x06,7);  x03^=ROTL(x15+x11,7);
        x08^=ROTL(x04+x00,9);  x13^=ROTL(x09+x05,9);
        x02^=ROTL(x14+x10,9);  x07^=ROTL(x03+x15,9);
        x12^=ROTL(x08+x04,13); x01^=ROTL(x13+x09,13);
        x06^=ROTL(x02+x14,13); x11^=ROTL(x07+x03,13);
        x00^=ROTL(x12+x08,18); x05^=ROTL(x01+x13,18);
        x10^=ROTL(x06+x02,18); x15^=ROTL(x11+x07,18);
        x01^=ROTL(x00+x03,7);  x06^=ROTL(x05+x04,7);
        x11^=ROTL(x10+x09,7);  x12^=ROTL(x15+x14,7);
        x02^=ROTL(x01+x00,9);  x07^=ROTL(x06+x05,9);
        x08^=ROTL(x11+x10,9);  x13^=ROTL(x12+x15,9);
        x03^=ROTL(x02+x01,13); x04^=ROTL(x07+x06,13);
        x09^=ROTL(x08+x11,13); x14^=ROTL(x13+x12,13);
        x00^=ROTL(x03+x02,18); x05^=ROTL(x04+x07,18);
        x10^=ROTL(x09+x08,18); x15^=ROTL(x14+x13,18);
    }
    #undef ROTL
    B[0]+=x00;  B[1]+=x01;  B[2]+=x02;  B[3]+=x03;
    B[4]+=x04;  B[5]+=x05;  B[6]+=x06;  B[7]+=x07;
    B[8]+=x08;  B[9]+=x09;  B[10]+=x10; B[11]+=x11;
    B[12]+=x12; B[13]+=x13; B[14]+=x14; B[15]+=x15;
}

static void dagtech_scrypt_romix(uint32_t *X, uint32_t *V, int N) {
    for (int i = 0; i < N; i++) {
        memcpy(&V[i * 32], X, 128);
        dagtech_xor_salsa8(&X[0], &X[16]);
        dagtech_xor_salsa8(&X[16], &X[0]);
    }
    for (int i = 0; i < N; i++) {
        int j = X[16] & (N - 1);
        for (int k = 0; k < 32; k++) X[k] ^= V[j * 32 + k];
        dagtech_xor_salsa8(&X[0], &X[16]);
        dagtech_xor_salsa8(&X[16], &X[0]);
    }
}

/*
 * DagTech Full Hash Function — scrypt_1024_1_1_256 (cpuminer-compatible)
 * V is a caller-supplied scratch buffer of SCRYPT_N * 128 bytes
 */
static void dagtech_hash(const uint8_t *input, uint8_t *output, uint32_t *V) {
    uint32_t tstate[8], ostate[8], X[32];
    const uint32_t *in32 = (const uint32_t *)input;
    /* Midstate: SHA256 of first 64 bytes of input with LE word loading */
    memcpy(tstate, dagtech_sha256_iv, 32);
    dagtech_sha256_xform(tstate, in32, 0);
    /* HMAC init */
    dagtech_hmac80_init(in32, tstate, ostate);
    /* PBKDF2 phase 1: header -> 128-byte X */
    dagtech_pbkdf2_80_128(tstate, ostate, in32, X);
    /* ROMix */
    dagtech_scrypt_romix(X, V, SCRYPT_N);
    /* Pool post-ROMix X[0] modification: add 0xe0 to lower 15 bits of bswap(X[0]) */
    {
        uint32_t B = DAGTECH_SWAB32(X[0]);
        uint32_t M = (B & 0xffff8000) | ((B + 0xe0) & 0x7fff);
        X[0] = DAGTECH_SWAB32(M);
    }
    /* PBKDF2 phase 2: X -> 32-byte hash */
    dagtech_pbkdf2_128_32(tstate, ostate, X, (uint32_t *)output);
}

/* =========================================================================
 * OpenCL GPU Worker
 * ========================================================================= */
#ifdef DAGTECH_GPU

/* Per-GPU OpenCL state — one entry per active device */
typedef struct {
    cl_device_id     device;
    cl_context       ctx;
    cl_command_queue queue;
    cl_program       program;
    cl_kernel        kernel;          /* legacy single-kernel: dagtech_search */
    /* GPU-2026.0607.4 (#40 increment 2): split-kernel handles. NULL when
       kernel_mode == 0 (legacy). Created opportunistically; on any failure
       we fall back to legacy without failing init. */
    cl_kernel        kernel_pre;
    cl_kernel        kernel_romix;
    cl_kernel        kernel_post;
    /* GPU-2026.0607.5 (#40 increment 3): cooperative 4-threads-per-hash ROMix.
       Replaces kernel_romix when kernel_mode == 2 (coop). NULL otherwise. */
    cl_kernel        kernel_romix_coop;
    size_t           coop_local_size;     /* WG size for romix_coop (multiple of 4) */
    size_t           coop_local_bytes;    /* __local L arg size: (lws/4) * 32 * sizeof(cl_uint) */
    cl_mem           V_buf;
    cl_mem           X_buf;           /* intermediate X[32]/work-item, split-mode only */
    cl_mem           output_buf;
    size_t           global_size;
    int              platform_idx;
    int              device_idx;
    int              gpu_index;    /* 0-based position among active GPUs */
    volatile int     ready;
    char             name[256];
    int              intensity;        /* actual intensity used for this GPU */
    int              kernel_mode;      /* 0 = legacy (dagtech_search)
                                          1 = split (pre + romix + post)
                                          2 = coop  (pre + romix_coop + post, 4 threads/hash) */
    uint64_t         hashes_session;   /* per-GPU hash counter */
    double           hashrate;         /* per-GPU H/s updated by stats loop */
    /* Mining-loop failure reporting. Every failure path used to retry silently,
     * so a GPU that stopped producing work looked identical to an idle one. */
    uint64_t         loop_errors;      /* failed iterations since start */
    uint64_t         loop_err_last_ms; /* last time we logged one */
} GpuCtx;

static GpuCtx g_gpus[MAX_GPUS];

/* Report a failed mining-loop iteration. Rate-limited to one line per second
 * per GPU so a persistent fault does not flood the log, but never silent: the
 * loop retries at 5 Hz and would otherwise spin forever producing nothing.
 * `what` names the step, `code` is the OpenCL error (or 0 where there is none). */
static void gpu_loop_error(GpuCtx *ctx, const char *what, int code);

static void gpu_loop_error(GpuCtx *ctx, const char *what, int code) {
    uint64_t now = dagtech_now_ms();
    ctx->loop_errors++;
    if (ctx->loop_err_last_ms != 0 && now - ctx->loop_err_last_ms < 1000) return;
    ctx->loop_err_last_ms = now;
    if (code)
        fprintf(stderr, "[DagCore GPU] GPU %d: %s failed (err %d); "
                        "%" DT_PRIu64 " failed iteration(s) so far, retrying.\n",
                ctx->gpu_index, what, code, (unsigned long long)ctx->loop_errors);
    else
        fprintf(stderr, "[DagCore GPU] GPU %d: %s; "
                        "%" DT_PRIu64 " failed iteration(s) so far, retrying.\n",
                ctx->gpu_index, what, (unsigned long long)ctx->loop_errors);
}

/* Compute global_size from intensity (0-100 -> 2^14 .. 2^20) */
static size_t gpu_intensity_to_global_size(int intensity) {
    if (intensity <= 0)   return (size_t)1 << 14;
    if (intensity >= 100) return (size_t)1 << 20;
    /* linear interpolation across exponent 14..20 */
    double exp_val = 14.0 + (intensity / 100.0) * 6.0;
    return (size_t)1 << (int)(exp_val + 0.5);
}

/* Largest power of two <= n (n must be >= 1) */
static size_t gpu_floor_pow2(size_t n) {
    size_t p = 1;
    while ((p << 1) != 0 && (p << 1) <= n) p <<= 1;
    return p;
}

/* Clamp the requested work-item count so the per-work-item scrypt scratchpad
 * (V buffer) actually fits this device.  scrypt N=1024 needs 128 KB of scratch
 * per work-item, AND a single OpenCL allocation cannot exceed
 * CL_DEVICE_MAX_MEM_ALLOC_SIZE (commonly ~1/4 of VRAM on NVIDIA/AMD).  Without
 * this clamp the default intensity (80 -> 2^19 work-items -> ~64 GB) fails to
 * allocate on every real GPU and the miner silently drops to CPU-only.
 * Returns a work-item count guaranteed to allocate, rounded down per
 * gpu_align: a power of two by default, else a multiple of gpu_align. */
static size_t gpu_fit_global_size(cl_device_id dev, size_t desired, int gpu_index) {
    const size_t per_item = 1024u * 32u * sizeof(cl_uint);  /* 128 KB / work-item */
    cl_ulong global_mem = 0, max_alloc = 0;
    clGetDeviceInfo(dev, CL_DEVICE_GLOBAL_MEM_SIZE,    sizeof(global_mem), &global_mem, NULL);
    clGetDeviceInfo(dev, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(max_alloc),  &max_alloc,  NULL);

    /* Budget = the smaller of (max single allocation) and 80% of total VRAM. */
    cl_ulong budget = max_alloc;
    if (global_mem > 0) {
        cl_ulong soft = (cl_ulong)((double)global_mem * 0.80);
        if (budget == 0 || soft < budget) budget = soft;
    }
    if (budget == 0) return desired;  /* couldn't query limits — trust caller */

    size_t max_items = (size_t)(budget / (cl_ulong)per_item);
    if (max_items < 1) max_items = 1;
    if (desired <= max_items) return desired;

    size_t fitted;
    char   how[32];
    if (gpu_align > 0) {
        fitted = max_items - (max_items % (size_t)gpu_align);
        if (fitted == 0) {
            /* Budget smaller than one alignment unit. Don't silently ignore the
             * request - say so and use what actually fits. */
            fprintf(stderr, "[DagCore GPU] GPU %d: VRAM budget fits only %zu work-items, "
                            "below the requested alignment of %d - using %zu unaligned.\n",
                    gpu_index, max_items, gpu_align, max_items);
            fitted = max_items;
            snprintf(how, sizeof(how), "unaligned");
        } else {
            snprintf(how, sizeof(how), "align %d", gpu_align);
        }
    } else {
        fitted = gpu_floor_pow2(max_items);
        snprintf(how, sizeof(how), "pow2");
    }
    printf("[DagCore GPU] GPU %d: requested %zu work-items needs %.1f GB; device caps at "
           "%.1f GB (max-alloc %.1f GB) -> clamping to %zu (%s).\n",
           gpu_index, desired, (double)desired * per_item / (1024.0*1024.0*1024.0),
           (double)budget / (1024.0*1024.0*1024.0),
           (double)max_alloc / (1024.0*1024.0*1024.0), fitted, how);
    return fitted;
}

/* Load the kernel source from the same directory as argv[0] */
static char *gpu_load_kernel_source(const char *exe_path, size_t *src_len) {
    char cl_path[1024];

    /* Build path: replace binary name with dagcore_gpu.cl */
    strncpy(cl_path, exe_path, sizeof(cl_path) - 1);
    cl_path[sizeof(cl_path) - 1] = '\0';

    /* Find last path separator */
    char *sep = strrchr(cl_path, '/');
#ifdef _WIN32
    {
        char *sep2 = strrchr(cl_path, '\\');
        if (sep2 > sep) sep = sep2;
    }
#endif
    if (sep) {
        *(sep + 1) = '\0';
        strncat(cl_path, "dagcore_gpu.cl", sizeof(cl_path) - strlen(cl_path) - 1);
    } else {
        strncpy(cl_path, "dagcore_gpu.cl", sizeof(cl_path) - 1);
    }

    FILE *f = fopen(cl_path, "rb");
    if (!f) {
        fprintf(stderr, "[DagCore GPU] ERROR: Cannot open kernel: %s\n", cl_path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = (char *)malloc(fsize + 1);
    if (!src) { fclose(f); return NULL; }
    size_t got = fread(src, 1, (size_t)fsize, f);
    if (got != (size_t)fsize) {
        fprintf(stderr, "[DagCore GPU] ERROR: short read on kernel %s (%zu of %ld bytes)\n",
                cl_path, got, fsize);
        free(src);
        fclose(f);
        return NULL;
    }
    src[fsize] = '\0';
    fclose(f);
    if (src_len) *src_len = (size_t)fsize;
    return src;
}

/* Lists all OpenCL GPUs to stdout */
static void gpu_list_devices(void) {
    cl_uint num_platforms = 0;
    clGetPlatformIDs(0, NULL, &num_platforms);
    if (num_platforms == 0) {
        printf("[DagCore GPU] No OpenCL platforms found.\n");
        return;
    }
    cl_platform_id *platforms = (cl_platform_id *)malloc(num_platforms * sizeof(cl_platform_id));
    clGetPlatformIDs(num_platforms, platforms, NULL);
    printf("[DagCore GPU] Detected OpenCL devices:\n");
    for (cl_uint p = 0; p < num_platforms; p++) {
        cl_uint num_devices = 0;
        clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
        for (cl_uint d = 0; d < num_devices; d++) {
            cl_device_id dev;
            clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, d + 1, &dev, NULL);
            char name[256] = {0};
            clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(name), name, NULL);
            printf("[DagCore GPU]   Platform %u Device %u: %s\n", p, d, name);
            if ((int)p == gpu_platform && g_gpu_detected < MAX_GPUS) {
                snprintf(g_gpu_names[g_gpu_detected], sizeof(g_gpu_names[0]), "%s", name);
                g_gpu_detected++;
            }
        }
    }
    free(platforms);
}

/* Initialise a single GPU into ctx using a pre-loaded kernel source string */
static int gpu_init_one(GpuCtx *ctx, cl_platform_id platform, int platform_idx,
                        int device_idx, int gpu_index,
                        const char *src, size_t src_len) {
    cl_int err;

    /* Select device */
    cl_uint num_devices = 0;
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
    if (num_devices == 0 || (cl_uint)device_idx >= num_devices) {
        fprintf(stderr, "[DagCore GPU] Device %d not available on platform %d (only %u found).\n",
                device_idx, platform_idx, num_devices);
        return -1;
    }
    cl_device_id *devices = (cl_device_id *)malloc(num_devices * sizeof(cl_device_id));
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, num_devices, devices, NULL);
    ctx->device = devices[device_idx];
    free(devices);

    ctx->platform_idx = platform_idx;
    ctx->device_idx   = device_idx;
    ctx->gpu_index    = gpu_index;
    ctx->ready        = 0;

    clGetDeviceInfo(ctx->device, CL_DEVICE_NAME, sizeof(ctx->name), ctx->name, NULL);
    printf("[DagCore GPU] GPU %d: %s (platform %d, device %d)\n",
           gpu_index, ctx->name, platform_idx, device_idx);

    /* Per-GPU context and command queue */
    ctx->ctx = clCreateContext(NULL, 1, &ctx->device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[DagCore GPU] clCreateContext failed (GPU %d): %d\n", gpu_index, err);
        return -1;
    }
    ctx->queue = clCreateCommandQueue(ctx->ctx, ctx->device, 0, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[DagCore GPU] clCreateCommandQueue failed (GPU %d): %d\n", gpu_index, err);
        clReleaseContext(ctx->ctx); ctx->ctx = NULL;
        return -1;
    }

    /* Compile kernel for this device */
    ctx->program = clCreateProgramWithSource(ctx->ctx, 1, &src, &src_len, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[DagCore GPU] clCreateProgramWithSource failed (GPU %d): %d\n", gpu_index, err);
        clReleaseCommandQueue(ctx->queue); ctx->queue = NULL;
        clReleaseContext(ctx->ctx);        ctx->ctx   = NULL;
        return -1;
    }
    err = clBuildProgram(ctx->program, 1, &ctx->device, "-cl-std=CL1.2", NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t log_size = 0;
        clGetProgramBuildInfo(ctx->program, ctx->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char *log = (char *)malloc(log_size + 1);
        if (log) {
            clGetProgramBuildInfo(ctx->program, ctx->device, CL_PROGRAM_BUILD_LOG,
                                  log_size, log, NULL);
            log[log_size] = '\0';
            fprintf(stderr, "[DagCore GPU] Kernel build error (GPU %d):\n%s\n", gpu_index, log);
            free(log);
        }
        clReleaseProgram(ctx->program);    ctx->program = NULL;
        clReleaseCommandQueue(ctx->queue); ctx->queue   = NULL;
        clReleaseContext(ctx->ctx);        ctx->ctx     = NULL;
        return -1;
    }
    ctx->kernel = clCreateKernel(ctx->program, "dagtech_search", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[DagCore GPU] clCreateKernel failed (GPU %d): %d\n", gpu_index, err);
        clReleaseProgram(ctx->program);    ctx->program = NULL;
        clReleaseCommandQueue(ctx->queue); ctx->queue   = NULL;
        clReleaseContext(ctx->ctx);        ctx->ctx     = NULL;
        return -1;
    }

    /* Work size and V buffer — use per-GPU intensity if provided */
    ctx->intensity   = (gpu_intensity_count > gpu_index) ?
                        gpu_intensity_list[gpu_index] : gpu_intensity;
    ctx->global_size = gpu_intensity_to_global_size(ctx->intensity);
    /* Clamp to what this device can actually allocate (VRAM + max-alloc cap). */
    ctx->global_size = gpu_fit_global_size(ctx->device, ctx->global_size, gpu_index);
    printf("[DagCore GPU] GPU %d: global work size %zu (intensity %d)\n",
           gpu_index, ctx->global_size, ctx->intensity);

    size_t v_bytes = ctx->global_size * 1024 * 32 * sizeof(cl_uint);
    printf("[DagCore GPU] GPU %d: allocating V buffer %.1f MB\n",
           gpu_index, v_bytes / (1024.0 * 1024.0));
    ctx->V_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_WRITE, v_bytes, NULL, &err);
    /* Belt-and-suspenders: if the driver still refuses (VRAM fragmented or in
     * use by the display/other apps), halve the work size and retry a few times
     * before giving up.  Halving preserves both pow2 and multiple-of-N shapes;
     * nonce tiling only needs a consistent stride, not a power of two. */
    {
        int v_retries = 0;
        while (err != CL_SUCCESS && ctx->global_size > ((size_t)1 << 12) && v_retries < 6) {
            ctx->global_size >>= 1;
            v_bytes = ctx->global_size * 1024 * 32 * sizeof(cl_uint);
            printf("[DagCore GPU] GPU %d: V buffer alloc failed (err %d); retrying at "
                   "work size %zu (%.1f MB)\n",
                   gpu_index, err, ctx->global_size, v_bytes / (1024.0 * 1024.0));
            ctx->V_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_WRITE, v_bytes, NULL, &err);
            v_retries++;
        }
    }
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[DagCore GPU] V buffer failed (GPU %d, %zu bytes): %d\n"
                        "[DagCore GPU] Try reducing --gpu-intensity.\n", gpu_index, v_bytes, err);
        clReleaseKernel(ctx->kernel);      ctx->kernel  = NULL;
        clReleaseProgram(ctx->program);    ctx->program = NULL;
        clReleaseCommandQueue(ctx->queue); ctx->queue   = NULL;
        clReleaseContext(ctx->ctx);        ctx->ctx     = NULL;
        return -1;
    }

    /* Output buffer: [0]=best_nonce, [1]=found_count */
    ctx->output_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_WRITE, 2 * sizeof(cl_uint), NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[DagCore GPU] Output buffer failed (GPU %d): %d\n", gpu_index, err);
        clReleaseMemObject(ctx->V_buf);    ctx->V_buf   = NULL;
        clReleaseKernel(ctx->kernel);      ctx->kernel  = NULL;
        clReleaseProgram(ctx->program);    ctx->program = NULL;
        clReleaseCommandQueue(ctx->queue); ctx->queue   = NULL;
        clReleaseContext(ctx->ctx);        ctx->ctx     = NULL;
        return -1;
    }

    /* GPU-2026.0607.4 (#40 inc 2) / GPU-2026.0607.5 (#40 inc 3):
     * Try to set up split-kernel mode (pre + romix + post), then optionally
     * promote to coop mode (pre + romix_coop + post, 4 threads/hash). On any
     * failure at any level, fall back to the next tier without failing init:
     *   coop create fails  -> stay in split
     *   split create fails -> fall back to legacy dagtech_search
     * GPU_KERNEL_MODE env var caps the maximum mode: "legacy"|"0" forces 0,
     * "split"|"1" caps at 1, "coop"|"2"|unset/empty allows promotion to 2. */
    ctx->kernel_pre = ctx->kernel_romix = ctx->kernel_post = NULL;
    ctx->kernel_romix_coop = NULL;
    ctx->coop_local_size = 0;
    ctx->coop_local_bytes = 0;
    ctx->X_buf = NULL;
    ctx->kernel_mode = 0;
    {
        /* Default is "split" (mode 1, +112% vs baseline on RTX 4060 Laptop).
         * "coop" (mode 2) is an opt-in path: it adds 4-thread-per-hash
         * cooperative Salsa20/8 via __local + barriers, but the barrier
         * latency on portable OpenCL exceeds the parallelism savings on
         * the tested RTX 4060 Laptop (≈80 KH/s vs split's ≈272 KH/s).
         * The kernel is bit-identical (shares accept), so it's preserved
         * for future tuning or for hardware where the trade-off flips
         * (AMD with native wavefront cooperation, OpenCL 2.0 sub-groups,
         * etc.). Use GPU_KERNEL_MODE=coop to try it. */
        int max_mode = 1;
        const char *km = getenv("GPU_KERNEL_MODE");
        if (km) {
            if      (strcmp(km, "legacy") == 0 || strcmp(km, "0") == 0) max_mode = 0;
            else if (strcmp(km, "split")  == 0 || strcmp(km, "1") == 0) max_mode = 1;
            else if (strcmp(km, "coop")   == 0 || strcmp(km, "2") == 0) max_mode = 2;
        }

        if (max_mode >= 1) {
            cl_int e1 = 0, e2 = 0, e3 = 0;
            ctx->kernel_pre   = clCreateKernel(ctx->program, "dagtech_pre",   &e1);
            ctx->kernel_romix = clCreateKernel(ctx->program, "dagtech_romix", &e2);
            ctx->kernel_post  = clCreateKernel(ctx->program, "dagtech_post",  &e3);
            if (e1 != CL_SUCCESS || e2 != CL_SUCCESS || e3 != CL_SUCCESS) {
                fprintf(stderr, "[DagCore GPU] GPU %d: split-kernel create failed "
                                "(pre=%d romix=%d post=%d) — falling back to legacy.\n",
                                gpu_index, e1, e2, e3);
                if (ctx->kernel_pre)   { clReleaseKernel(ctx->kernel_pre);   ctx->kernel_pre   = NULL; }
                if (ctx->kernel_romix) { clReleaseKernel(ctx->kernel_romix); ctx->kernel_romix = NULL; }
                if (ctx->kernel_post)  { clReleaseKernel(ctx->kernel_post);  ctx->kernel_post  = NULL; }
            } else {
                size_t x_bytes = ctx->global_size * 32 * sizeof(cl_uint);
                cl_int xerr = 0;
                ctx->X_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_WRITE, x_bytes, NULL, &xerr);
                if (xerr != CL_SUCCESS) {
                    fprintf(stderr, "[DagCore GPU] GPU %d: X_buf alloc failed (err %d) "
                                    "— falling back to legacy.\n", gpu_index, xerr);
                    clReleaseKernel(ctx->kernel_pre);   ctx->kernel_pre   = NULL;
                    clReleaseKernel(ctx->kernel_romix); ctx->kernel_romix = NULL;
                    clReleaseKernel(ctx->kernel_post);  ctx->kernel_post  = NULL;
                } else {
                    ctx->kernel_mode = 1;
                    printf("[DagCore GPU] GPU %d: split-kernel mode (X_buf %.2f MB)\n",
                           gpu_index, x_bytes / (1024.0 * 1024.0));
                }
            }
        } else {
            printf("[DagCore GPU] GPU %d: legacy single-kernel mode (GPU_KERNEL_MODE=legacy)\n",
                   gpu_index);
        }

        /* Try to promote to coop mode if split succeeded and env allows. */
        if (ctx->kernel_mode == 1 && max_mode >= 2) {
            cl_int ce = 0;
            ctx->kernel_romix_coop = clCreateKernel(ctx->program, "dagtech_romix_coop", &ce);
            if (ce != CL_SUCCESS || ctx->kernel_romix_coop == NULL) {
                fprintf(stderr, "[DagCore GPU] GPU %d: coop kernel create failed (err %d) "
                                "— staying in split mode.\n", gpu_index, ce);
                ctx->kernel_romix_coop = NULL;
            } else {
                /* Local-work-size for romix_coop: 32 = 1 NVIDIA warp = 8 hashes
                 * per WG, all in lockstep so barrier(CLK_LOCAL_MEM_FENCE) is
                 * essentially free (intra-warp sync, no inter-warp wait).
                 * lws=64 (1 AMD wavefront) tested 3.2x slower on RTX 4060 due
                 * to the 2-warp barrier cost. AMD users get a slightly higher
                 * barrier cost but a correct execution; perf-tune later if
                 * needed (could read CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT or
                 * sub-group size to pick optimally per device). Local memory:
                 * 32 uints per hash * (lws/4) hashes per WG = lws*32 bytes
                 * = 1024 bytes at lws=32. */
                ctx->coop_local_size  = 32;
                ctx->coop_local_bytes = (ctx->coop_local_size / 4) * 32 * sizeof(cl_uint);
                ctx->kernel_mode = 2;
                printf("[DagCore GPU] GPU %d: coop-kernel mode (4 threads/hash, lws=%zu, "
                       "local=%.1f KB/WG)\n",
                       gpu_index, ctx->coop_local_size,
                       ctx->coop_local_bytes / 1024.0);
            }
        }
    }

    ctx->ready = 1;
    return 0;
}

/* Discover and initialise all requested GPUs on gpu_platform */
static int gpu_init_all(const char *exe_path) {
    /* Load kernel source once — reused for every GPU's compile */
    size_t src_len = 0;
    char *src = gpu_load_kernel_source(exe_path, &src_len);
    if (!src) return -1;

    /* Select platform */
    cl_uint num_platforms = 0;
    clGetPlatformIDs(0, NULL, &num_platforms);
    if (num_platforms == 0) {
        fprintf(stderr, "[DagCore GPU] No OpenCL platforms found.\n");
        free(src); return -1;
    }
    if ((cl_uint)gpu_platform >= num_platforms) {
        fprintf(stderr, "[DagCore GPU] Platform %d not available (only %u found).\n",
                gpu_platform, num_platforms);
        free(src); return -1;
    }
    cl_platform_id *platforms = (cl_platform_id *)malloc(num_platforms * sizeof(cl_platform_id));
    clGetPlatformIDs(num_platforms, platforms, NULL);
    cl_platform_id plat = platforms[gpu_platform];
    free(platforms);

    /* Count available GPU devices on this platform */
    cl_uint num_devices = 0;
    clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
    if (num_devices == 0) {
        fprintf(stderr, "[DagCore GPU] No GPU devices on platform %d.\n", gpu_platform);
        free(src); return -1;
    }

    /* Build list of device indices to initialise */
    int dev_list[MAX_GPUS];
    int dev_count = 0;

    if (gpu_use_all) {
        for (cl_uint d = 0; d < num_devices && dev_count < MAX_GPUS; d++)
            dev_list[dev_count++] = (int)d;
    } else if (gpu_device_count > 0) {
        for (int i = 0; i < gpu_device_count && i < MAX_GPUS; i++)
            dev_list[dev_count++] = gpu_device_list[i];
    } else {
        /* Backward-compatible single-device mode */
        dev_list[0] = gpu_device;
        dev_count   = 1;
    }

    /* Initialise each selected device */
    g_num_gpus = 0;
    memset(g_gpus, 0, sizeof(g_gpus));
    for (int i = 0; i < dev_count; i++) {
        if (g_num_gpus >= MAX_GPUS) break;
        if (gpu_init_one(&g_gpus[g_num_gpus], plat, gpu_platform,
                         dev_list[i], g_num_gpus, src, src_len) == 0) {
            g_num_gpus++;
        } else {
            fprintf(stderr, "[DagCore GPU] Skipping device %d (init failed).\n", dev_list[i]);
        }
    }

    free(src);
    if (g_num_gpus == 0) return -1;
    printf("[DagCore GPU] Initialised %d GPU(s) successfully.\n", g_num_gpus);
    return 0;
}

static void gpu_cleanup(void) {
    for (int i = 0; i < g_num_gpus; i++) {
        GpuCtx *ctx = &g_gpus[i];
        if (ctx->output_buf) { clReleaseMemObject(ctx->output_buf); ctx->output_buf = NULL; }
        if (ctx->V_buf)      { clReleaseMemObject(ctx->V_buf);      ctx->V_buf      = NULL; }
        if (ctx->kernel)     { clReleaseKernel(ctx->kernel);        ctx->kernel     = NULL; }
        if (ctx->program)    { clReleaseProgram(ctx->program);      ctx->program    = NULL; }
        if (ctx->queue)      { clReleaseCommandQueue(ctx->queue);   ctx->queue      = NULL; }
        if (ctx->ctx)        { clReleaseContext(ctx->ctx);          ctx->ctx        = NULL; }
        ctx->ready = 0;
    }
    g_num_gpus = 0;
}

/* ============================================================================
 * #42 GPU work-size autotune
 *
 * At GPU-thread startup (when AUTOTUNE=1), sweep the Cartesian product of
 * AUTOTUNE_BATCHES x AUTOTUNE_KERNEL_MODES, score each trial using the
 * reference miner's formula (useful hashrate - stale - lowdiff - error -
 * latency penalties, each capped at 0.75 of base), pick the highest-scoring
 * candidate, and persist the choice in AUTOTUNE_CACHE keyed by GPU+config.
 *
 * The trial harness duplicates the main loop's dispatch logic (clear comment
 * marker below) rather than refactoring gpu_thread - keeps the working main
 * path completely untouched while we prove the autotune machinery.
 * ============================================================================ */

#define AUTOTUNE_MAX_BATCHES 16
#define AUTOTUNE_MAX_MODES   4
#define AUTOTUNE_MAX_TRIALS  (AUTOTUNE_MAX_BATCHES * AUTOTUNE_MAX_MODES)

typedef struct {
    int      requested_batchsize;
    int      actual_batchsize;  /* after gpu_fit_global_size clamping */
    int      mode;
    int      valid;
    uint64_t hashes;
    uint64_t submitted;
    uint64_t accepted;
    uint64_t rejected;
    uint64_t stale;
    double   elapsed_s;
    double   hashrate_hps;
    double   avg_batch_ms;
    double   max_batch_ms;
    double   accepted_factor;
    double   base_score;
    double   stale_penalty;
    double   lowdiff_penalty;
    double   latency_penalty;
    double   final_score;
    char     reason[64];
} AutotuneTrial;

/* Forward declarations of share-submission helpers used by the trial dispatch. */
void dagtech_submit_share_ext(const DagTechJob *j, uint32_t nonce);
static void dagtech_mkdir_parents(const char *filepath);

static int at_parse_int_list(const char *s, int *out, int max_n) {
    int n = 0;
    char buf[256];
    strncpy(buf, s, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, ",");
    while (tok && n < max_n) {
        while (*tok == ' ' || *tok == '\t') tok++;
        int v = atoi(tok);
        if (v > 0) out[n++] = v;
        tok = strtok(NULL, ",");
    }
    return n;
}

static int at_mode_name_to_int(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "legacy", 6) == 0 || s[0] == '0') return 0;
    if (strncmp(s, "split",  5) == 0 || s[0] == '1') return 1;
    if (strncmp(s, "coop",   4) == 0 || s[0] == '2') return 2;
    return -1;
}

static const char *at_mode_int_to_name(int m) {
    switch (m) {
        case 0: return "legacy";
        case 1: return "split";
        case 2: return "coop";
        default: return "unknown";
    }
}

static int at_parse_mode_list(const char *s, int *out, int max_n) {
    int n = 0;
    char buf[128];
    strncpy(buf, s, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, ",");
    while (tok && n < max_n) {
        int m = at_mode_name_to_int(tok);
        if (m >= 0) out[n++] = m;
        tok = strtok(NULL, ",");
    }
    return n;
}

static void at_compute_cache_key(GpuCtx *ctx, char *out, size_t out_size) {
    cl_ulong vram = 0;
    char driver_ver[128] = "";
    clGetDeviceInfo(ctx->device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(vram),       &vram,       NULL);
    clGetDeviceInfo(ctx->device, CL_DRIVER_VERSION,         sizeof(driver_ver), driver_ver,  NULL);
    snprintf(out, out_size,
        "autotune-v1|gpu=%s|vram=%llu|drv=%s|batches=%s|modes=%s|trial=%d",
        ctx->name, (unsigned long long)vram, driver_ver,
        gpu_autotune_batches, gpu_autotune_modes, gpu_autotune_trial_seconds);
}

/* Cache I/O: simple key=value lines, fits plain C with no JSON library. */
static int autotune_load_cache(const char *path, const char *key,
                                int *out_batchsize, int *out_mode) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1280];
    char cached_key[1280] = "";
    int cached_batchsize = 0, cached_mode = -1, cached_valid = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if      (strcmp(k, "key") == 0) {
            strncpy(cached_key, v, sizeof(cached_key) - 1);
            cached_key[sizeof(cached_key) - 1] = '\0';
        }
        else if (strcmp(k, "selected_batchsize") == 0) cached_batchsize = atoi(v);
        else if (strcmp(k, "selected_mode")      == 0) cached_mode = at_mode_name_to_int(v);
        else if (strcmp(k, "valid")              == 0) cached_valid = atoi(v);
    }
    fclose(f);
    if (!cached_valid)                       return 0;
    if (strcmp(cached_key, key) != 0)         return 0;
    if (cached_batchsize <= 0)                return 0;
    if (cached_mode < 0)                      return 0;
    *out_batchsize = cached_batchsize;
    *out_mode      = cached_mode;
    return 1;
}

static int autotune_save_cache(const char *path, const char *key,
                                const AutotuneTrial *winner, int valid_trials) {
    dagtech_mkdir_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[Autotune] WARNING: cannot write cache %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    time_t now = time(NULL);
    fprintf(f, "# DagTech GPU miner autotune cache (#42)\n");
    fprintf(f, "# Generated %s", ctime(&now));
    fprintf(f, "valid=1\n");
    fprintf(f, "key=%s\n", key);
    fprintf(f, "selected_batchsize=%d\n",  winner->actual_batchsize);
    fprintf(f, "requested_batchsize=%d\n", winner->requested_batchsize);
    fprintf(f, "selected_mode=%s\n",       at_mode_int_to_name(winner->mode));
    fprintf(f, "final_score=%.2f\n",       winner->final_score);
    fprintf(f, "hashrate_hps=%.0f\n",      winner->hashrate_hps);
    fprintf(f, "submitted=%llu\n",         (unsigned long long)winner->submitted);
    fprintf(f, "accepted=%llu\n",          (unsigned long long)winner->accepted);
    fprintf(f, "elapsed_s=%.1f\n",         winner->elapsed_s);
    fprintf(f, "num_valid_trials=%d\n",    valid_trials);
    fclose(f);
    return 0;
}

/* Reallocate V_buf + X_buf for a new (batchsize, mode). Frees existing first.
 * Returns 0 on success, -1 if any alloc fails (caller must handle: in trial
 * harness mark the trial invalid; in apply-winner fall through to safe defaults). */
static int gpu_realloc_buffers(GpuCtx *ctx, size_t new_batchsize, int new_mode) {
    if (ctx->V_buf) { clReleaseMemObject(ctx->V_buf); ctx->V_buf = NULL; }
    if (ctx->X_buf) { clReleaseMemObject(ctx->X_buf); ctx->X_buf = NULL; }

    ctx->global_size = new_batchsize;
    ctx->kernel_mode = new_mode;

    cl_int err = 0;
    size_t v_bytes = ctx->global_size * 1024 * 32 * sizeof(cl_uint);
    ctx->V_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_WRITE, v_bytes, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[Autotune] V_buf alloc failed at batchsize=%zu (err=%d, %.1f MB)\n",
                ctx->global_size, err, v_bytes / (1024.0 * 1024.0));
        return -1;
    }
    if (new_mode >= 1) {
        size_t x_bytes = ctx->global_size * 32 * sizeof(cl_uint);
        cl_int xe = 0;
        ctx->X_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_WRITE, x_bytes, NULL, &xe);
        if (xe != CL_SUCCESS) {
            fprintf(stderr, "[Autotune] X_buf alloc failed at batchsize=%zu (err=%d)\n",
                    ctx->global_size, xe);
            clReleaseMemObject(ctx->V_buf); ctx->V_buf = NULL;
            return -1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------------------
 * autotune_run_trial: mine for `seconds` seconds at the given (batchsize, mode),
 * capture stats deltas, score using the reference miner's formula.
 *
 * IMPORTANT: the dispatch block below is a deliberate DUPLICATE of the main
 * mining loop in dagtech_gpu_thread (kept isolated so this experimental code
 * cannot regress the proven main path). If you change the main dispatch,
 * mirror the change here. Marker: "AUTOTUNE TRIAL DISPATCH".
 * ---------------------------------------------------------------------------- */
static AutotuneTrial autotune_run_trial(GpuCtx *ctx, int requested_batchsize,
                                         int mode, int seconds, int gpu_idx) {
    AutotuneTrial t;
    memset(&t, 0, sizeof(t));
    t.requested_batchsize = requested_batchsize;
    t.mode = mode;
    strncpy(t.reason, "unknown", sizeof(t.reason) - 1);

    /* Clamp to what device can allocate; mark invalid if even minimum doesn't fit */
    size_t fitted = gpu_fit_global_size(ctx->device, (size_t)requested_batchsize, gpu_idx);
    t.actual_batchsize = (int)fitted;
    if (fitted < (size_t)1 << 10) {
        strncpy(t.reason, "batchsize_too_small_after_fit", sizeof(t.reason) - 1);
        return t;
    }

    /* Allocate buffers for this candidate (frees prior allocation) */
    if (gpu_realloc_buffers(ctx, fitted, mode) != 0) {
        strncpy(t.reason, "alloc_failed", sizeof(t.reason) - 1);
        return t;
    }

    /* CPU verify scratch (same as main loop) */
    uint32_t *V_cpu = (uint32_t *)malloc(SCRYPT_N * 128);
    if (!V_cpu) {
        strncpy(t.reason, "verify_buf_alloc_failed", sizeof(t.reason) - 1);
        return t;
    }

    /* Snapshot global stats before */
    pthread_mutex_lock(&gpu_stats_mtx);
    uint64_t hashes_before = gpu_hashes_session;
    pthread_mutex_unlock(&gpu_stats_mtx);
    pthread_mutex_lock(&stats_mtx);
    uint64_t sub_before   = gpu_submitted;
    uint64_t acc_before   = gpu_accepted;
    uint64_t rej_before   = gpu_rejected;
    uint64_t stale_before = gpu_stale;
    pthread_mutex_unlock(&stats_mtx);

    uint64_t trial_t0    = dagtech_now_ms();
    uint64_t trial_end   = trial_t0 + (uint64_t)seconds * 1000ULL;
    uint64_t batch_count = 0;
    uint64_t batch_ms_total = 0;
    long long max_batch_ms = 0;

    uint32_t nonce_base   = 0x80000000u + (uint32_t)gpu_idx * (uint32_t)ctx->global_size;
    uint32_t nonce_stride = (uint32_t)g_num_gpus * (uint32_t)ctx->global_size;
    if (nonce_stride == 0) nonce_stride = (uint32_t)ctx->global_size;

    cl_mem header_buf = NULL;
    uint64_t job_seq_local = 0;
    cl_uint target32 = 0;
    uint8_t header80[80];

    while (running && dagtech_now_ms() < trial_end) {
        /* Get current job */
        DagTechJob j;
        pthread_mutex_lock(&job_mtx);
        j = current_job;
        pthread_mutex_unlock(&job_mtx);
        if (!j.valid) { usleep(100000); continue; }

        if (j.seq != job_seq_local) {
            /* New job: rebuild header_buf + target32 (mirrors main loop) */
            if (header_buf) { clReleaseMemObject(header_buf); header_buf = NULL; }

            uint8_t version_b[4], prevhash[32], ntime_b[4], bits_b[4];
            uint8_t en1[4], en2[4], en_combined[8], merkle[32];
            if (strlen(j.version) != 8 || strlen(j.prevhash) < 64 ||
                strlen(j.ntime) != 8 || strlen(j.bits) != 8 ||
                strlen(j.extranonce1) != 8) { usleep(100000); continue; }
            hex_to_bytes(j.version,    version_b, 4);
            hex_to_bytes(j.prevhash,   prevhash, 32);
            hex_to_bytes(j.ntime,      ntime_b,  4);
            hex_to_bytes(j.bits,       bits_b,   4);
            hex_to_bytes(j.extranonce1, en1,     4);
            memset(en2, 0, 4);
            memcpy(en_combined, en1, 4);
            memcpy(en_combined + 4, en2, 4);
            sha256d(en_combined, 8, merkle);
            memcpy(header80,      version_b, 4);
            memcpy(header80 + 4,  prevhash, 32);
            memcpy(header80 + 36, merkle,   32);
            memcpy(header80 + 68, ntime_b,  4);
            memcpy(header80 + 72, bits_b,   4);
            memset(header80 + 76, 0, 4);

            cl_uint header_words[20];
            memcpy(header_words, header80, 80);
            double diff = dagtech_effective_diff(j.difficulty);
            double thresh_d = (double)0x0000FFFF00000000ULL / diff;
            uint64_t thresh64 = (thresh_d >= 18446744073709551615.0) ? 0xFFFFFFFFFFFFFFFFULL : (uint64_t)thresh_d;
            target32 = (cl_uint)(thresh64 >> 32);

            cl_int herr = 0;
            header_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         20 * sizeof(cl_uint), header_words, &herr);
            if (!header_buf || herr != CL_SUCCESS) { usleep(100000); continue; }
            job_seq_local = j.seq;
        }

        /* ====== BEGIN AUTOTUNE TRIAL DISPATCH (mirrors gpu_thread main loop) ====== */
        cl_uint output_init[2] = { 0xFFFFFFFFu, 0 };
        clEnqueueWriteBuffer(ctx->queue, ctx->output_buf, CL_TRUE, 0,
                             2 * sizeof(cl_uint), output_init, 0, NULL, NULL);

        uint64_t gpu_t0 = dagtech_now_ms();
        cl_event ev = NULL;
        cl_int err;
        if (ctx->kernel_mode >= 1) {
            clSetKernelArg(ctx->kernel_pre, 0, sizeof(cl_mem),  &header_buf);
            clSetKernelArg(ctx->kernel_pre, 1, sizeof(cl_mem),  &ctx->X_buf);
            clSetKernelArg(ctx->kernel_pre, 2, sizeof(cl_uint), &nonce_base);
            err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_pre, 1, NULL,
                                         &ctx->global_size, NULL, 0, NULL, NULL);
            if (err == CL_SUCCESS) {
                if (ctx->kernel_mode == 2) {
                    size_t coop_gws = ctx->global_size * 4;
                    clSetKernelArg(ctx->kernel_romix_coop, 0, sizeof(cl_mem), &ctx->X_buf);
                    clSetKernelArg(ctx->kernel_romix_coop, 1, sizeof(cl_mem), &ctx->V_buf);
                    clSetKernelArg(ctx->kernel_romix_coop, 2, ctx->coop_local_bytes, NULL);
                    err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_romix_coop, 1, NULL,
                                                 &coop_gws, &ctx->coop_local_size, 0, NULL, NULL);
                } else {
                    clSetKernelArg(ctx->kernel_romix, 0, sizeof(cl_mem), &ctx->X_buf);
                    clSetKernelArg(ctx->kernel_romix, 1, sizeof(cl_mem), &ctx->V_buf);
                    err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_romix, 1, NULL,
                                                 &ctx->global_size, NULL, 0, NULL, NULL);
                }
            }
            if (err == CL_SUCCESS) {
                clSetKernelArg(ctx->kernel_post, 0, sizeof(cl_mem),  &ctx->X_buf);
                clSetKernelArg(ctx->kernel_post, 1, sizeof(cl_mem),  &header_buf);
                clSetKernelArg(ctx->kernel_post, 2, sizeof(cl_mem),  &ctx->output_buf);
                clSetKernelArg(ctx->kernel_post, 3, sizeof(cl_uint), &target32);
                clSetKernelArg(ctx->kernel_post, 4, sizeof(cl_uint), &nonce_base);
                err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_post, 1, NULL,
                                             &ctx->global_size, NULL, 0, NULL, &ev);
            }
        } else {
            clSetKernelArg(ctx->kernel, 0, sizeof(cl_mem),  &header_buf);
            clSetKernelArg(ctx->kernel, 1, sizeof(cl_mem),  &ctx->output_buf);
            clSetKernelArg(ctx->kernel, 2, sizeof(cl_mem),  &ctx->V_buf);
            clSetKernelArg(ctx->kernel, 3, sizeof(cl_uint), &target32);
            clSetKernelArg(ctx->kernel, 4, sizeof(cl_uint), &nonce_base);
            err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel, 1, NULL,
                                         &ctx->global_size, NULL, 0, NULL, &ev);
        }
        if (err != CL_SUCCESS) {
            gpu_loop_error(ctx, "kernel enqueue", err);
            usleep(200000); continue;
        }

        cl_int werr = clWaitForEvents(1, &ev);
        clReleaseEvent(ev);
        long long batch_ms = (long long)(dagtech_now_ms() - gpu_t0);
        if (werr != CL_SUCCESS) {
            gpu_loop_error(ctx, "clWaitForEvents", werr);
            usleep(200000); continue;
        }
        /* Implausibly-fast guard: a batch that returns in under 2 ms did no real
         * work (queue flushed, context lost). Same treatment - reported, not
         * silently swallowed. */
        if (batch_ms < 2) {
            gpu_loop_error(ctx, "batch returned implausibly fast (<2 ms)", 0);
            usleep(200000); continue;
        }

        cl_uint output_result[2] = { 0xFFFFFFFFu, 0 };
        cl_int rerr = clEnqueueReadBuffer(ctx->queue, ctx->output_buf, CL_TRUE, 0,
                                          2 * sizeof(cl_uint), output_result, 0, NULL, NULL);
        if (rerr != CL_SUCCESS) {
            gpu_loop_error(ctx, "clEnqueueReadBuffer", rerr);
            usleep(200000); continue;
        }

        /* Account for hashes (global counters - same as main loop) */
        pthread_mutex_lock(&gpu_stats_mtx);
        ctx->hashes_session += ctx->global_size;
        gpu_hashes_session  += ctx->global_size;
        pthread_mutex_unlock(&gpu_stats_mtx);
        pthread_mutex_lock(&stats_mtx);
        total_hashes += ctx->global_size;
        pthread_mutex_unlock(&stats_mtx);

        batch_count++;
        batch_ms_total += (uint64_t)batch_ms;
        if (batch_ms > max_batch_ms) max_batch_ms = batch_ms;

        /* Apply GPU_THROTTLE duty-cycle sleep — mirrors the main loop. Without
         * this, trials run at 100% duty cycle while the live miner runs at
         * gpu_throttle% — measured hashrate would be inflated by 1/throttle. */
        if (gpu_throttle < 100 && batch_ms > 0) {
            long long sleep_ms = batch_ms * (100 - gpu_throttle) / gpu_throttle;
            long long slept = 0;
            while (slept < sleep_ms && running) {
                long long chunk = sleep_ms - slept;
                if (chunk > 100) chunk = 100;
                usleep((unsigned int)(chunk * 1000));
                slept += chunk;
                /* No job-changed check here - trial outer loop handles it */
                if (dagtech_now_ms() >= trial_end) break;
            }
        }

        /* If candidate found, CPU re-verify before submitting (same as main loop) */
        if (output_result[1] > 0 && output_result[0] != 0xFFFFFFFFu) {
            uint32_t cand_nonce = output_result[0];
            uint8_t verify_hdr[80];
            memcpy(verify_hdr, header80, 80);
            verify_hdr[76] = cand_nonce & 0xff;
            verify_hdr[77] = (cand_nonce >> 8) & 0xff;
            verify_hdr[78] = (cand_nonce >> 16) & 0xff;
            verify_hdr[79] = (cand_nonce >> 24) & 0xff;
            uint8_t hash[32];
            dagtech_hash(verify_hdr, hash, V_cpu);
            uint64_t hash_top64 =
                ((uint64_t)hash[31] << 56) | ((uint64_t)hash[30] << 48) |
                ((uint64_t)hash[29] << 40) | ((uint64_t)hash[28] << 32) |
                ((uint64_t)hash[27] << 24) | ((uint64_t)hash[26] << 16) |
                ((uint64_t)hash[25] <<  8) |  (uint64_t)hash[24];
            double threshold_d = (double)0x0000FFFF00000000ULL / dagtech_effective_diff(j.difficulty);
            uint64_t threshold64 = (threshold_d >= 18446744073709551615.0) ?
                                    0xFFFFFFFFFFFFFFFFULL : (uint64_t)threshold_d;
            if (hash_top64 <= threshold64) {
                DagTechJob jcur;
                pthread_mutex_lock(&job_mtx);
                jcur = current_job;
                pthread_mutex_unlock(&job_mtx);
                if (jcur.seq == job_seq_local && jcur.valid) {
                    dagtech_submit_share_ext(&jcur, cand_nonce);
                }
            }
        }

        nonce_base += nonce_stride;
        if (nonce_base < 0x80000000u)
            nonce_base = 0x80000000u + (uint32_t)gpu_idx * (uint32_t)ctx->global_size;
        /* ====== END AUTOTUNE TRIAL DISPATCH ====== */
    }

    if (header_buf) clReleaseMemObject(header_buf);
    free(V_cpu);

    /* Settle: wait for in-flight pool replies to update accepted/rejected counters */
    usleep(750000);

    /* Snapshot stats after, compute deltas */
    pthread_mutex_lock(&gpu_stats_mtx);
    uint64_t hashes_after = gpu_hashes_session;
    pthread_mutex_unlock(&gpu_stats_mtx);
    pthread_mutex_lock(&stats_mtx);
    uint64_t sub_after   = gpu_submitted;
    uint64_t acc_after   = gpu_accepted;
    uint64_t rej_after   = gpu_rejected;
    uint64_t stale_after = gpu_stale;
    pthread_mutex_unlock(&stats_mtx);

    t.hashes    = hashes_after - hashes_before;
    t.submitted = sub_after - sub_before;
    t.accepted  = acc_after - acc_before;
    t.rejected  = rej_after - rej_before;
    t.stale     = stale_after - stale_before;
    t.elapsed_s = (double)(dagtech_now_ms() - trial_t0) / 1000.0;

    /* Validity gate: must run for at least 50% of intended duration */
    if (t.elapsed_s < (double)seconds * 0.5) {
        strncpy(t.reason, "too_short", sizeof(t.reason) - 1);
        return t;
    }

    t.hashrate_hps = t.elapsed_s > 0 ? (double)t.hashes / t.elapsed_s : 0.0;
    t.avg_batch_ms = batch_count > 0 ? ((double)batch_ms_total / (double)batch_count) : 0.0;
    t.max_batch_ms = (double)max_batch_ms;

    /* Scoring (reference miner's formula, simplified - we map lowdiff/error
     * to our rejected counter since the pool doesn't distinguish these for us). */
    double accepted_factor = t.submitted > 0
        ? ((double)t.accepted / (double)t.submitted)
        : 0.85;
    if (accepted_factor < 0.15) accepted_factor = 0.15;
    double base       = t.hashrate_hps * accepted_factor;
    double stale_rate = (t.submitted + t.stale) > 0 ? (double)t.stale / (double)(t.submitted + t.stale) : 0.0;
    double rej_rate   = t.submitted > 0 ? (double)t.rejected / (double)t.submitted : 0.0;
    if (stale_rate > 0.75) stale_rate = 0.75;
    if (rej_rate   > 0.75) rej_rate   = 0.75;
    double stale_pen   = base * stale_rate;
    double lowdiff_pen = base * rej_rate;
    double useful = base - stale_pen - lowdiff_pen;
    if (useful < 0) useful = 0;
    double latency_pen = 0.0;
    if (t.avg_batch_ms > (double)gpu_target_batch_ms && t.avg_batch_ms > 0.0) {
        double rate = 1.0 - ((double)gpu_target_batch_ms / t.avg_batch_ms);
        if (rate > 0.75) rate = 0.75;
        if (rate < 0.0)  rate = 0.0;
        latency_pen = useful * rate;
    }
    double final = useful - latency_pen;
    if (final < 0) final = 0;

    t.accepted_factor   = accepted_factor;
    t.base_score        = base;
    t.stale_penalty     = stale_pen;
    t.lowdiff_penalty   = lowdiff_pen;
    t.latency_penalty   = latency_pen;
    t.final_score       = final;
    t.valid             = 1;
    strncpy(t.reason, "ok", sizeof(t.reason) - 1);
    return t;
}

/* Autotune progress marker. Written next to the cache as "<cache>.running"
 * while a sweep is in progress and removed when it ends, so the control server /
 * dashboard can show an "Autotune in progress - trial X/Y" banner reliably -
 * independent of trial length or how much the miner logs (the old log-tail
 * detection missed long 60s trials whose markers scrolled out of the window). */
static void autotune_marker_path(char *out, size_t out_size) {
    snprintf(out, out_size, "%s.running", gpu_autotune_cache);
}
static void autotune_write_marker(int trial, int total) {
    char path[600];
    autotune_marker_path(path, sizeof(path));
    dagtech_mkdir_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[Autotune] WARNING: cannot write progress marker %s: %s\n",
                path, strerror(errno));
        return;
    }
    fprintf(f, "trial=%d\ntotal=%d\n", trial, total);
    fclose(f);
}
static void autotune_clear_marker(void) {
    char path[600];
    autotune_marker_path(path, sizeof(path));
    remove(path);
}

/* Apply a previously-saved autotune result WITHOUT running any trials. Used when
 * AUTOTUNE=0 so a machine that was tuned once keeps its tuned work size + kernel
 * mode ("tune once, toggle off, keep the speed"). No-op (keeps the gpu_init
 * defaults) when no valid cache exists for this GPU/driver/candidate set. Does
 * not need a pool job — applying the cache is just a buffer realloc.
 * Returns 0 on success or graceful no-op, -1 only if buffers end up unusable. */
static int autotune_apply_cache_if_any(GpuCtx *ctx, int gpu_idx) {
    char key[1280];
    at_compute_cache_key(ctx, key, sizeof(key));
    int cached_bs = 0, cached_mode = -1;
    if (!autotune_load_cache(gpu_autotune_cache, key, &cached_bs, &cached_mode))
        return 0;  /* no saved tuning for this GPU/config — keep gpu_init defaults */

    printf("[Autotune] GPU %d: AUTOTUNE off; applying saved tuning (batchsize=%d, mode=%s).\n",
           gpu_idx, cached_bs, at_mode_int_to_name(cached_mode));
    size_t prev_bs = ctx->global_size;
    int    prev_mode = ctx->kernel_mode;
    if (gpu_realloc_buffers(ctx, (size_t)cached_bs, cached_mode) == 0) {
        printf("[Autotune] GPU %d: applied saved tuning.\n", gpu_idx);
        return 0;
    }
    /* Cached size didn't fit right now (rare: VRAM contention). gpu_realloc_buffers
     * frees first, so restore the previous default allocation to stay mineable. */
    fprintf(stderr, "[Autotune] GPU %d: saved tuning didn't fit; restoring default work size.\n", gpu_idx);
    if (gpu_realloc_buffers(ctx, prev_bs, prev_mode) != 0) {
        fprintf(stderr, "[Autotune] GPU %d: failed to restore buffers after cache apply.\n", gpu_idx);
        return -1;
    }
    return 0;
}

/* Top-level autotune: load cache or sweep all candidates, apply winner.
 * Returns 0 on success (ctx->global_size and ctx->kernel_mode now reflect winner;
 * V_buf and X_buf allocated for the winner). Returns -1 on hard failure (no valid
 * candidate AND can't allocate fallback). */
static int autotune_run(GpuCtx *ctx, int gpu_idx) {
    if (!gpu_autotune) return 0;

    char key[1280];
    at_compute_cache_key(ctx, key, sizeof(key));

    /* Cache hit path */
    if (!gpu_autotune_force) {
        int cached_bs = 0, cached_mode = -1;
        if (autotune_load_cache(gpu_autotune_cache, key, &cached_bs, &cached_mode)) {
            printf("[Autotune] GPU %d: cache hit (batchsize=%d, mode=%s).\n",
                   gpu_idx, cached_bs, at_mode_int_to_name(cached_mode));
            if (gpu_realloc_buffers(ctx, (size_t)cached_bs, cached_mode) == 0) {
                printf("[Autotune] GPU %d: applied cached selection.\n", gpu_idx);
                return 0;
            }
            fprintf(stderr, "[Autotune] GPU %d: cache hit but realloc failed; retuning.\n", gpu_idx);
        } else {
            printf("[Autotune] GPU %d: no valid cache; running trials.\n", gpu_idx);
        }
    } else {
        printf("[Autotune] GPU %d: AUTOTUNE_FORCE=1; running trials.\n", gpu_idx);
    }

    /* Parse candidate lists */
    int batches[AUTOTUNE_MAX_BATCHES];
    int modes[AUTOTUNE_MAX_MODES];
    int n_batches = at_parse_int_list(gpu_autotune_batches, batches, AUTOTUNE_MAX_BATCHES);
    int n_modes   = at_parse_mode_list(gpu_autotune_modes, modes, AUTOTUNE_MAX_MODES);
    if (n_batches == 0 || n_modes == 0) {
        fprintf(stderr, "[Autotune] GPU %d: empty candidate list; skipping (batches=%d modes=%d).\n",
                gpu_idx, n_batches, n_modes);
        return -1;
    }

    int total = n_batches * n_modes;
    printf("[Autotune] GPU %d: %d batches x %d modes = %d trials at %ds each (~%dm total).\n",
           gpu_idx, n_batches, n_modes, total,
           gpu_autotune_trial_seconds, (total * gpu_autotune_trial_seconds + 30) / 60);

    autotune_write_marker(0, total);  /* banner shows as soon as the sweep starts */
    AutotuneTrial trials[AUTOTUNE_MAX_TRIALS];
    int n_trials = 0;
    int idx = 1;
    /* Iterate modes outer, batches inner: groups same-mode trials together
     * (avoids unnecessary kernel-mode switches and X_buf re-alloc churn) */
    for (int i = 0; i < n_modes; i++) {
        for (int b = 0; b < n_batches; b++) {
            if (!running) break;
            printf("[Autotune] GPU %d: trial %d/%d batchsize=%d mode=%s ...\n",
                   gpu_idx, idx, total, batches[b], at_mode_int_to_name(modes[i]));
            autotune_write_marker(idx, total);
            AutotuneTrial t = autotune_run_trial(ctx, batches[b], modes[i],
                                                  gpu_autotune_trial_seconds, gpu_idx);
            if (t.valid) {
                printf("[Autotune] GPU %d: trial %d done: hps=%.0f sub=%llu acc=%llu rej=%llu stale=%llu avg_ms=%.1f score=%.0f\n",
                       gpu_idx, idx,
                       t.hashrate_hps,
                       (unsigned long long)t.submitted, (unsigned long long)t.accepted,
                       (unsigned long long)t.rejected,  (unsigned long long)t.stale,
                       t.avg_batch_ms, t.final_score);
            } else {
                printf("[Autotune] GPU %d: trial %d INVALID (reason=%s, elapsed=%.1fs)\n",
                       gpu_idx, idx, t.reason, t.elapsed_s);
            }
            trials[n_trials++] = t;
            idx++;
        }
    }

    /* Pick winner */
    int best = -1, valid_count = 0;
    double best_score = -1.0;
    for (int i = 0; i < n_trials; i++) {
        if (!trials[i].valid) continue;
        valid_count++;
        if (trials[i].final_score > best_score) {
            best_score = trials[i].final_score;
            best = i;
        }
    }

    if (best < 0) {
        autotune_clear_marker();
        fprintf(stderr, "[Autotune] GPU %d: NO valid trials; falling back to original config.\n", gpu_idx);
        size_t desired = gpu_intensity_to_global_size(ctx->intensity);
        size_t fitted  = gpu_fit_global_size(ctx->device, desired, gpu_idx);
        int fallback_mode = ctx->kernel_mode;  /* whatever it was before autotune started */
        if (gpu_realloc_buffers(ctx, fitted, fallback_mode) != 0) {
            fprintf(stderr, "[Autotune] GPU %d: fallback realloc also failed.\n", gpu_idx);
            return -1;
        }
        return 0;
    }

    AutotuneTrial *w = &trials[best];
    printf("[Autotune] GPU %d: WINNER batchsize=%d mode=%s score=%.0f (out of %d valid trials)\n",
           gpu_idx, w->actual_batchsize, at_mode_int_to_name(w->mode), w->final_score, valid_count);
    autotune_clear_marker();

    if (gpu_realloc_buffers(ctx, (size_t)w->actual_batchsize, w->mode) != 0) {
        fprintf(stderr, "[Autotune] GPU %d: applying winner failed (realloc).\n", gpu_idx);
        return -1;
    }
    autotune_save_cache(gpu_autotune_cache, key, w, valid_count);
    printf("[Autotune] GPU %d: cache saved to %s\n", gpu_idx, gpu_autotune_cache);
    return 0;
}

/* GPU mining thread: covers a non-overlapping partition of 0x80000000-0xFFFFFFFF.
 * GPU i starts at 0x80000000 + i*global_size and strides by N*global_size,
 * ensuring N parallel GPUs tile the full range without overlap. */
static void *dagtech_gpu_thread(void *arg) {
    GpuCtx *ctx = (GpuCtx *)arg;
    int gpu_idx = ctx->gpu_index;

    if (!ctx->ready) {
        fprintf(stderr, "[DagCore GPU] GPU %d not ready, thread exiting.\n", gpu_idx);
        return NULL;
    }

    /* #42 autotune: runs once at GPU-thread startup if AUTOTUNE=1. Waits for
     * a valid job (trials mine real shares against the live pool), then sweeps
     * candidates and applies the winner. On cache hit it's near-instant. On
     * miss the user sees ~N_candidates * AUTOTUNE_TRIAL_SECONDS of trials in
     * the log before normal mining begins. The autotune_run() leaves
     * ctx->global_size and ctx->kernel_mode set to the winner, with V_buf and
     * X_buf already reallocated to match. */
    autotune_clear_marker();  /* drop any stale marker from a previously-crashed sweep */
    if (gpu_autotune) {
        int waits = 0;
        for (;;) {
            if (!running) return NULL;
            pthread_mutex_lock(&job_mtx);
            int have_job = (current_job.valid != 0);
            pthread_mutex_unlock(&job_mtx);
            if (have_job) break;
            if (waits == 0)
                printf("[Autotune] GPU %d: waiting for first valid pool job before tuning...\n", gpu_idx);
            usleep(200000);
            if (++waits > 5 * 60) {  /* 60s timeout */
                fprintf(stderr, "[Autotune] GPU %d: no job after 60s; skipping autotune.\n", gpu_idx);
                gpu_autotune = 0;
                break;
            }
        }
        if (gpu_autotune) {
            autotune_run(ctx, gpu_idx);
        }
    } else {
        /* AUTOTUNE off: apply a previously-saved tuning if one exists for this
         * GPU, so "tune once, toggle off, keep the speed" works. No trials and
         * no job wait — applying the cache is just a buffer realloc. */
        autotune_apply_cache_if_any(ctx, gpu_idx);
    }

    uint32_t nonce_base   = 0x80000000u + (uint32_t)gpu_idx * (uint32_t)ctx->global_size;
    uint32_t nonce_stride = (uint32_t)g_num_gpus  * (uint32_t)ctx->global_size;

    printf("[DagCore GPU] Worker %d started: %s (nonce base 0x%08x, stride 0x%x)\n",
           gpu_idx, ctx->name, nonce_base, nonce_stride);

    /* Per-thread scratch buffer for CPU re-verification */
    uint32_t *V_cpu = (uint32_t *)malloc(SCRYPT_N * 128);
    if (!V_cpu) {
        fprintf(stderr, "[DagCore GPU] Out of memory for CPU verify buffer (GPU %d).\n", gpu_idx);
        return NULL;
    }

    /* Counts kernel batches that failed to execute (launch/wait/read errors,
     * e.g. driver watchdog/TDR or lost context). Used to rate-limit error
     * logging and to keep failures visible — failed batches are NOT counted as
     * hashes, so a broken kernel reads ~0 H/s instead of a fake high rate. */
    unsigned long long kernel_errors = 0;

    while (running) {
        DagTechJob j;
        pthread_mutex_lock(&job_mtx);
        j = current_job;
        pthread_mutex_unlock(&job_mtx);

        if (!j.valid) { usleep(100000); continue; }

        uint64_t job_seq = j.seq;

        /* Build 80-byte header with nonce=0 (placeholder) */
        uint8_t header80[80];
        {
            uint8_t version[4], prevhash[32], ntime_b[4], bits_b[4];
            uint8_t en1[4], en2[4], en_combined[8], merkle[32];
            if (strlen(j.version) != 8 || strlen(j.prevhash) < 64 ||
                strlen(j.ntime) != 8 || strlen(j.bits) != 8 ||
                strlen(j.extranonce1) != 8) {
                usleep(100000);
                continue;
            }
            hex_to_bytes(j.version,    version,  4);
            hex_to_bytes(j.prevhash,   prevhash, 32);
            hex_to_bytes(j.ntime,      ntime_b,  4);
            hex_to_bytes(j.bits,       bits_b,   4);
            hex_to_bytes(j.extranonce1, en1,     4);
            memset(en2, 0, 4);
            memcpy(en_combined, en1, 4);
            memcpy(en_combined + 4, en2, 4);
            sha256d(en_combined, 8, merkle);
            memcpy(header80,      version,  4);
            memcpy(header80 + 4,  prevhash, 32);
            memcpy(header80 + 36, merkle,   32);
            memcpy(header80 + 68, ntime_b,  4);
            memcpy(header80 + 72, bits_b,   4);
            memset(header80 + 76, 0, 4);  /* nonce placeholder */
        }

        /* Convert header to uint32 array for kernel */
        cl_uint header_words[20];
        memcpy(header_words, header80, 80);

        /* Compute 32-bit difficulty target matching the CPU's 64-bit check.
         * CPU checks: hash_top64 <= 0x0000FFFF00000000 / difficulty
         * hash_top64 upper 32 bits == hash[7] (BSWAP(ostate[7])), so the GPU
         * pre-filter target is the upper 32 bits of that threshold.
         * Using 0xFFFFFFFF/diff instead is ~1000x too lenient: atomic_min
         * always picks a nonce near nonce_base that rarely passes the CPU check. */
        double diff = dagtech_effective_diff(j.difficulty);
        double thresh_d = (double)0x0000FFFF00000000ULL / diff;
        uint64_t thresh64 = (thresh_d >= 18446744073709551615.0) ? 0xFFFFFFFFFFFFFFFFULL : (uint64_t)thresh_d;
        cl_uint target32 = (cl_uint)(thresh64 >> 32);

        /* Upload header to this GPU's context */
        cl_mem header_buf = clCreateBuffer(ctx->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                           20 * sizeof(cl_uint), header_words, NULL);
        if (!header_buf) { usleep(50000); continue; }

        /* Launch batches until job changes */
        while (running) {
            pthread_mutex_lock(&job_mtx);
            int job_changed = (current_job.seq != job_seq);
            pthread_mutex_unlock(&job_mtx);
            if (job_changed) break;

            /* Reset output buffer */
            cl_uint output_init[2] = { 0xFFFFFFFFu, 0 };
            clEnqueueWriteBuffer(ctx->queue, ctx->output_buf, CL_TRUE, 0,
                                 2 * sizeof(cl_uint), output_init, 0, NULL, NULL);

            /* Launch (legacy single-kernel or split pre/romix/post) */
            uint64_t gpu_t0 = dagtech_now_ms();
            cl_event ev = NULL;
            cl_int err;
            if (ctx->kernel_mode >= 1) {
                /* Split-kernel mode (#40 increment 2) or coop mode (#40 inc 3).
                 * pre and post are identical between modes; only the middle
                 * romix kernel and its launch shape differ.
                 * Queue is in-order, so intermediate enqueues don't need events;
                 * only the final dagtech_post produces the event we wait on -
                 * which can't complete until pre and (romix|romix_coop) have
                 * finished. The <2ms "implausibly fast" guard below measures
                 * from gpu_t0 to event completion, still catches no-op cases. */
                clSetKernelArg(ctx->kernel_pre, 0, sizeof(cl_mem),  &header_buf);
                clSetKernelArg(ctx->kernel_pre, 1, sizeof(cl_mem),  &ctx->X_buf);
                clSetKernelArg(ctx->kernel_pre, 2, sizeof(cl_uint), &nonce_base);
                err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_pre, 1, NULL,
                                             &ctx->global_size, NULL, 0, NULL, NULL);
                if (err == CL_SUCCESS) {
                    if (ctx->kernel_mode == 2) {
                        /* Coop romix: 4 threads/hash. global_size_romix = 4 *
                         * hashes, lws fixed at coop_local_size (64), __local
                         * L sized by host (coop_local_bytes). */
                        size_t coop_gws = ctx->global_size * 4;
                        clSetKernelArg(ctx->kernel_romix_coop, 0, sizeof(cl_mem), &ctx->X_buf);
                        clSetKernelArg(ctx->kernel_romix_coop, 1, sizeof(cl_mem), &ctx->V_buf);
                        clSetKernelArg(ctx->kernel_romix_coop, 2, ctx->coop_local_bytes, NULL);
                        err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_romix_coop, 1, NULL,
                                                     &coop_gws, &ctx->coop_local_size,
                                                     0, NULL, NULL);
                    } else {
                        /* Split romix: 1 thread/hash, driver picks lws */
                        clSetKernelArg(ctx->kernel_romix, 0, sizeof(cl_mem), &ctx->X_buf);
                        clSetKernelArg(ctx->kernel_romix, 1, sizeof(cl_mem), &ctx->V_buf);
                        err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_romix, 1, NULL,
                                                     &ctx->global_size, NULL, 0, NULL, NULL);
                    }
                }
                if (err == CL_SUCCESS) {
                    clSetKernelArg(ctx->kernel_post, 0, sizeof(cl_mem),  &ctx->X_buf);
                    clSetKernelArg(ctx->kernel_post, 1, sizeof(cl_mem),  &header_buf);
                    clSetKernelArg(ctx->kernel_post, 2, sizeof(cl_mem),  &ctx->output_buf);
                    clSetKernelArg(ctx->kernel_post, 3, sizeof(cl_uint), &target32);
                    clSetKernelArg(ctx->kernel_post, 4, sizeof(cl_uint), &nonce_base);
                    err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel_post, 1, NULL,
                                                 &ctx->global_size, NULL, 0, NULL, &ev);
                }
            } else {
                /* Legacy: one big dagtech_search does the whole pipeline */
                clSetKernelArg(ctx->kernel, 0, sizeof(cl_mem),  &header_buf);
                clSetKernelArg(ctx->kernel, 1, sizeof(cl_mem),  &ctx->output_buf);
                clSetKernelArg(ctx->kernel, 2, sizeof(cl_mem),  &ctx->V_buf);
                clSetKernelArg(ctx->kernel, 3, sizeof(cl_uint), &target32);
                clSetKernelArg(ctx->kernel, 4, sizeof(cl_uint), &nonce_base);
                err = clEnqueueNDRangeKernel(ctx->queue, ctx->kernel, 1, NULL,
                                             &ctx->global_size, NULL, 0, NULL, &ev);
            }
            if (err != CL_SUCCESS) {
                kernel_errors++;
                if (kernel_errors <= 3 || (kernel_errors % 50) == 0)
                    fprintf(stderr, "[DagCore GPU] GPU %d kernel launch error: %d (count=%llu)\n",
                            gpu_idx, err, (unsigned long long)kernel_errors);
                usleep(500000);
                break;
            }

            /* Wait for the kernel to actually finish. If it FAILED to execute
             * (driver watchdog/TDR reset, out-of-resources, lost context), the
             * error surfaces here. We must NOT count this batch: counting failed
             * batches inflates the reported hashrate to absurd values while
             * finding zero shares, which hides the failure from the user. */
            cl_int werr = clWaitForEvents(1, &ev);
            clReleaseEvent(ev);
            long long gpu_elapsed = (long long)(dagtech_now_ms() - gpu_t0);
            if (werr != CL_SUCCESS) {
                kernel_errors++;
                if (kernel_errors <= 3 || (kernel_errors % 50) == 0)
                    fprintf(stderr, "[DagCore GPU] GPU %d kernel did not complete "
                            "(clWaitForEvents=%d, count=%llu) - not counting batch. "
                            "Check driver timeout (TDR) or lower --gpu-intensity.\n",
                            gpu_idx, werr, (unsigned long long)kernel_errors);
                usleep(500000);
                break;
            }

            /* Timing sanity floor: a real batch of >=16384 scrypt(N=1024) hashes
             * moves >=4 GB of memory and cannot finish in ~0 ms on ANY GPU. If it
             * does, the kernel reported success but did no real work (miscompiled
             * / no-op on this driver). Counting it would report a huge fake
             * hashrate with zero shares — the exact failure that hides a dead GPU.
             * Don't count it; surface it instead. */
            if (gpu_elapsed < 2) {
                kernel_errors++;
                if (kernel_errors <= 3 || (kernel_errors % 50) == 0)
                    fprintf(stderr, "[DagCore GPU] GPU %d batch finished implausibly fast "
                            "(%lld ms for %zu hashes, count=%llu) - kernel not doing real "
                            "work; not counting. Update GPU drivers / OpenCL runtime.\n",
                            gpu_idx, gpu_elapsed, ctx->global_size, (unsigned long long)kernel_errors);
                usleep(500000);
                break;
            }

            /* Read output */
            cl_uint output_result[2] = { 0xFFFFFFFFu, 0 };
            cl_int rerr = clEnqueueReadBuffer(ctx->queue, ctx->output_buf, CL_TRUE, 0,
                                2 * sizeof(cl_uint), output_result, 0, NULL, NULL);
            if (rerr != CL_SUCCESS) {
                kernel_errors++;
                if (kernel_errors <= 3 || (kernel_errors % 50) == 0)
                    fprintf(stderr, "[DagCore GPU] GPU %d result read failed "
                            "(clEnqueueReadBuffer=%d, count=%llu) - not counting batch.\n",
                            gpu_idx, rerr, (unsigned long long)kernel_errors);
                usleep(500000);
                break;
            }

            /* Batch genuinely completed — account for hashes (aggregate + per-GPU) */
            pthread_mutex_lock(&gpu_stats_mtx);
            ctx->hashes_session += ctx->global_size;
            gpu_hashes_session  += ctx->global_size;
            pthread_mutex_unlock(&gpu_stats_mtx);

            pthread_mutex_lock(&stats_mtx);
            total_hashes += ctx->global_size;
            pthread_mutex_unlock(&stats_mtx);

            /* GPU throttle: duty-cycle sleep proportional to kernel run time.
             * Sleeps in 100ms chunks so the thread stays responsive to new jobs.
             * gpu_throttle=50 means GPU runs half the time (50% duty cycle). */
            if (gpu_throttle < 100 && gpu_elapsed > 0) {
                long long sleep_ms = gpu_elapsed * (100 - gpu_throttle) / gpu_throttle;
                long long slept = 0;
                while (slept < sleep_ms && running) {
                    long long chunk = sleep_ms - slept;
                    if (chunk > 100) chunk = 100;
                    usleep((unsigned int)(chunk * 1000));
                    slept += chunk;
                    pthread_mutex_lock(&job_mtx);
                    int job_changed = (current_job.seq != job_seq);
                    pthread_mutex_unlock(&job_mtx);
                    if (job_changed) break;
                }
            }

            /* If candidate found, CPU re-verify before submitting */
            if (output_result[1] > 0 && output_result[0] != 0xFFFFFFFFu) {
                uint32_t cand_nonce = output_result[0];
                /* Build header with candidate nonce */
                uint8_t verify_hdr[80];
                memcpy(verify_hdr, header80, 80);
                verify_hdr[76] = cand_nonce & 0xff;
                verify_hdr[77] = (cand_nonce >> 8) & 0xff;
                verify_hdr[78] = (cand_nonce >> 16) & 0xff;
                verify_hdr[79] = (cand_nonce >> 24) & 0xff;

                uint8_t hash[32];
                dagtech_hash(verify_hdr, hash, V_cpu);

                /* Full 64-bit target check (same as CPU worker) */
                uint64_t hash_top64 =
                    ((uint64_t)hash[31] << 56) | ((uint64_t)hash[30] << 48) |
                    ((uint64_t)hash[29] << 40) | ((uint64_t)hash[28] << 32) |
                    ((uint64_t)hash[27] << 24) | ((uint64_t)hash[26] << 16) |
                    ((uint64_t)hash[25] <<  8) |  (uint64_t)hash[24];
                double threshold_d = (double)0x0000FFFF00000000ULL / dagtech_effective_diff(j.difficulty);
                uint64_t threshold64 = (threshold_d >= 18446744073709551615.0) ?
                                        0xFFFFFFFFFFFFFFFFULL : (uint64_t)threshold_d;

                if (hash_top64 <= threshold64) {
                    printf("[DagCore GPU] ** SHARE FOUND ** GPU %d nonce=0x%08x\n",
                           gpu_idx, cand_nonce);

                    /* Re-read job under lock before submitting */
                    DagTechJob jcur;
                    pthread_mutex_lock(&job_mtx);
                    jcur = current_job;
                    pthread_mutex_unlock(&job_mtx);
                    if (jcur.seq == job_seq && jcur.valid) {
                        extern void dagtech_submit_share_ext(const DagTechJob *j, uint32_t nonce);
                        dagtech_submit_share_ext(&jcur, cand_nonce);
                    }
                }
            }

            /* Advance nonce base, wrapping within this GPU's partition */
            nonce_base += nonce_stride;
            if (nonce_base < 0x80000000u)
                nonce_base = 0x80000000u + (uint32_t)gpu_idx * (uint32_t)ctx->global_size;
        }

        clReleaseMemObject(header_buf);
    }

    free(V_cpu);
    return NULL;
}

#endif /* DAGTECH_GPU */

/* =========================================================================
 * Stratum Protocol - DagTech Network Communication
 * ========================================================================= */
static int dagtech_connect_pool(void) {

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", pool_port);

    int rc = getaddrinfo(pool_host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "[DagCore] DNS resolution failed for %s: %s\n",
                pool_host, gai_strerror(rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        fprintf(stderr, "[DagCore] Failed to connect to %s:%d\n", pool_host, pool_port);
        return -1;
    }

    /* #44: disable Nagle. Stratum sends short JSON lines a few times per second;
     * Nagle holds each one until the previous segment is ACKed, which on a
     * high-RTT link (and with the peer's delayed-ACK timer) adds tens to
     * hundreds of milliseconds to every share submission. On a pool whose jobs
     * rotate several times per second that delay alone turns accepted shares
     * into stales. Best-effort: a failure here is not fatal to mining. */
    int nodelay = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&nodelay, sizeof(nodelay)) != 0)
        fprintf(stderr, "[DagCore] WARNING: could not set TCP_NODELAY\n");

    return 0;
}

static void dagtech_send(const char *line) {
    pthread_mutex_lock(&sock_mtx);
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s\n", line);
    send(sockfd, buf, (int)strlen(buf), 0);
    pthread_mutex_unlock(&sock_mtx);
}

static void dagtech_subscribe_authorize(void) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"DagCore/" DAGTECH_VERSION "\"]}");
    dagtech_send(buf);

    /* The pool requires a bare EVM address as the stratum username, so WORKER
     * goes in the password field - the slot a pool that supports worker names
     * reads it from.
     *
     * The current DagCore pool does not support them. Its own documentation
     * says "this pool build rejects address.worker logins, so there are no
     * per-machine names", and the password field is ignored, so WORKER has no
     * visible effect on the pool site today.
     *
     * Kept as it is on purpose: the name starts working the day the pool reads
     * that field, whereas sending address.worker as the username would be
     * rejected outright. Note that WORKER wins over PASSWORD when both are
     * set - they share this one field. */
    char pass_field[128];
    if (worker_name[0])
        snprintf(pass_field, sizeof(pass_field), "%s", worker_name);
    else
        snprintf(pass_field, sizeof(pass_field), "%s", password);

    snprintf(buf, sizeof(buf),
        "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"%s\"]}",
        wallet, pass_field);
    dagtech_send(buf);
}

static int extract_quoted(const char *line, char out[][256], int max) {
    int count = 0;
    const char *p = line;
    while (count < max && (p = strchr(p, '"')) != NULL) {
        p++;
        const char *end = strchr(p, '"');
        if (!end) break;
        int len = (int)(end - p);
        if (len > 255) len = 255;
        memcpy(out[count], p, len);
        out[count][len] = 0;
        count++;
        p = end + 1;
    }
    return count;
}

static void dagtech_parse_stratum(const char *line) {
    /* Subscribe response — id:1 exactly (not id:1000+), extract extranonce1 */
    if (strstr(line, "mining.subscribe") == NULL &&
        strstr(line, "\"result\"") && strstr(line, "\"id\":1,")) {
        char strings[20][256];
        int n = extract_quoted(line, strings, 20);
        for (int i = 0; i < n; i++) {
            if (strlen(strings[i]) == 8 &&
                strspn(strings[i], "0123456789abcdef") == 8) {
                strncpy(extranonce1_global, strings[i],
                        sizeof(extranonce1_global) - 1);
                printf("[DagCore] Subscribed - extranonce1=%s\n", extranonce1_global);
                break;
            }
        }
    }
    /* Authorize response — id:2, result:true, not a share accept */
    else if (strstr(line, "\"id\":2,") && strstr(line, "\"result\":true")) {
        printf("[DagCore] Authorized\n");
    }
    /* Difficulty update */
    else if (strstr(line, "mining.set_difficulty")) {
        const char *p = strstr(line, "params");
        if (p) {
            p = strchr(p, '[');
            if (p) {
                double new_diff = atof(p + 1);
                current_difficulty = new_diff;
                pthread_mutex_lock(&job_mtx);
                if (current_job.valid)
                    current_job.difficulty = new_diff;
                pthread_mutex_unlock(&job_mtx);
                printf("[DagCore] Difficulty: %.8f\n", current_difficulty);
            }
        }
    }
    /* New job notification */
    else if (strstr(line, "mining.notify")) {
        char strings[20][256];
        int n = extract_quoted(line, strings, 20);
        int offset = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(strings[i], "mining.notify") == 0) {
                offset = i + 1;
                break;
            }
        }
        if (offset < n && strcmp(strings[offset], "params") == 0)
            offset++;
        if (n - offset >= 5) {
            pthread_mutex_lock(&job_mtx);
            current_job.valid = 1;
            current_job.seq++;
            current_job.difficulty = current_difficulty;
            strncpy(current_job.job_id,     strings[offset],   sizeof(current_job.job_id) - 1);
            strncpy(current_job.prevhash,   strings[offset+1], sizeof(current_job.prevhash) - 1);
            strncpy(current_job.version,    strings[offset+2], sizeof(current_job.version) - 1);
            strncpy(current_job.bits,       strings[offset+3], sizeof(current_job.bits) - 1);
            strncpy(current_job.ntime,      strings[offset+4], sizeof(current_job.ntime) - 1);
            strncpy(current_job.extranonce1, extranonce1_global, sizeof(current_job.extranonce1) - 1);
            current_job.extranonce1[sizeof(current_job.extranonce1) - 1] = '\0';
            pthread_mutex_unlock(&job_mtx);
            printf("[DagCore] New job: %s (diff %.8f)\n",
                   current_job.job_id, current_job.difficulty);
        }
    }
    /* Share accepted */
    else if (strstr(line, "\"result\"") && strstr(line, "true")
             && !strstr(line, "false") && !strstr(line, "\"error\":[")) {
        /* Parse response id to attribute to CPU or GPU */
        const char *idp = strstr(line, "\"id\":");
        uint64_t resp_id = idp ? strtoull(idp + 5, NULL, 10) : (uint64_t)-1;
        int src = -1;
        double share_diff = current_difficulty;   /* if the id is not found */
        pthread_mutex_lock(&pending_mtx);
        for (int pi = 0; pi < pending_count; pi++) {
            int idx = (pending_head + pi) % PENDING_SUB_MAX;
            if (pending_subs[idx].id == resp_id) {
                src        = pending_subs[idx].is_gpu;
                share_diff = pending_subs[idx].diff;
                break;
            }
        }
        pthread_mutex_unlock(&pending_mtx);
        pthread_mutex_lock(&stats_mtx);
        accepted_work += share_diff * HASHES_PER_DIFF1;
        total_accepted++;
        if (src == 1) gpu_accepted++;
        else if (src == 0) cpu_accepted++;
        pthread_mutex_unlock(&stats_mtx);
        printf("[DagCore] Share ACCEPTED (%lu total | CPU:%lu GPU:%lu)\n",
               (unsigned long)total_accepted,
               (unsigned long)cpu_accepted,
               (unsigned long)gpu_accepted);
    }
    /* Share rejected — error code 21 = "Job not found" = stale (job expired
       before submit arrived). Stales are normal; actual rejects are a problem. */
    else if (strstr(line, "\"error\":[")) {
        const char *idp = strstr(line, "\"id\":");
        uint64_t resp_id = idp ? strtoull(idp + 5, NULL, 10) : (uint64_t)-1;
        int src = -1;
        pthread_mutex_lock(&pending_mtx);
        for (int pi = 0; pi < pending_count; pi++) {
            int idx = (pending_head + pi) % PENDING_SUB_MAX;
            if (pending_subs[idx].id == resp_id) { src = pending_subs[idx].is_gpu; break; }
        }
        pthread_mutex_unlock(&pending_mtx);
        int is_stale = strstr(line, "\"21\"") || strstr(line, ",21,") ||
                       strstr(line, "[21,")  || strstr(line, "stale") ||
                       strstr(line, "job not found");
        pthread_mutex_lock(&stats_mtx);
        if (is_stale) {
            total_stale++;
            if (src == 1) gpu_stale++;    else if (src == 0) cpu_stale++;
            pthread_mutex_unlock(&stats_mtx);
            printf("[DagCore] Share stale (job expired) (%lu total stale)\n",
                   (unsigned long)total_stale);
        } else {
            total_rejected++;
            if (src == 1) gpu_rejected++; else if (src == 0) cpu_rejected++;
            pthread_mutex_unlock(&stats_mtx);
            /* "Low difficulty share" (commonly error code 23) means our submit
             * threshold is too loose. Raise the active margin so subsequent
             * shares must be stronger, up to a sane cap. */
            int is_lowdiff = strstr(line, "low difficulty") || strstr(line, "low diff") ||
                             strstr(line, "\"23\"") || strstr(line, ",23,") || strstr(line, "[23,");
            if (is_lowdiff && auto_threshold) {
                active_margin *= 1.05;
                if (active_margin > 8.0) active_margin = 8.0;
                printf("[DagCore] Low-difficulty reject -> submit margin now %.3f\n",
                       active_margin);
            }
            printf("[DagCore] Share REJECTED: %s\n", line);
        }
    }
}

static void *dagtech_recv_thread(void *arg) {
    (void)arg;
    char buf[8192];
    char linebuf[16384] = {0};
    int linelen = 0;

    while (running) {
        ssize_t n = recv(sockfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (running) printf("[DagCore] Pool connection lost\n");
            running = 0;
            break;
        }
        buf[n] = 0;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                linebuf[linelen] = 0;
                if (linelen > 0) dagtech_parse_stratum(linebuf);
                linelen = 0;
            } else if (linelen < (int)sizeof(linebuf) - 1) {
                linebuf[linelen++] = buf[i];
            }
        }
    }
    return NULL;
}

/* =========================================================================
 * Block Header Construction
 * ========================================================================= */
static int dagtech_make_header(const DagTechJob *j, uint32_t nonce, uint8_t header[80]) {
    if (strlen(j->version) != 8 || strlen(j->prevhash) < 64 ||
        strlen(j->ntime) != 8 || strlen(j->bits) != 8 ||
        strlen(j->extranonce1) != 8) return -1;

    uint8_t version[4], prevhash[32], ntime_b[4], bits_b[4];
    uint8_t en1[4], en2[4], en_combined[8], merkle[32];

    hex_to_bytes(j->version,    version,  4);
    hex_to_bytes(j->prevhash,   prevhash, 32);
    hex_to_bytes(j->ntime,      ntime_b,  4);
    hex_to_bytes(j->bits,       bits_b,   4);
    hex_to_bytes(j->extranonce1, en1,     4);
    memset(en2, 0, 4);

    memcpy(en_combined, en1, 4);
    memcpy(en_combined + 4, en2, 4);
    sha256d(en_combined, 8, merkle);

    memcpy(header,      version,  4);
    memcpy(header + 4,  prevhash, 32);
    memcpy(header + 36, merkle,   32);
    memcpy(header + 68, ntime_b,  4);
    memcpy(header + 72, bits_b,   4);
    header[76] = nonce & 0xff;
    header[77] = (nonce >> 8) & 0xff;
    header[78] = (nonce >> 16) & 0xff;
    header[79] = (nonce >> 24) & 0xff;

    return 0;
}

/* Rate limiter: minimum gap between share submissions.
 * #44: was 200 ms (max 5 shares/s). Any share found inside that window was
 * dropped SILENTLY - not submitted, not counted as stale, invisible in the
 * stats. At GPU hashrates the burst rate comfortably exceeds 5/s, so this was
 * discarding real work with no way to notice. 20 ms still protects the pool
 * from a runaway loop while letting normal bursts through. */
static uint64_t last_submit_ms = 0;
static uint64_t rate_limited_shares = 0;  /* #44: shares dropped by the limiter */
static pthread_mutex_t submit_rate_mtx = PTHREAD_MUTEX_INITIALIZER;
/* SUBMIT_MIN_INTERVAL_MS in config.env; 0 turns the limiter off. On the
 * production rig 20 ms costs next to nothing in steady state (effective 99.4%
 * of raw); its drops come almost all from the first minutes after a start,
 * while vardiff still has the difficulty low and shares come in floods. */
#define SUBMIT_MIN_INTERVAL_MS_DEFAULT 20
#define SUBMIT_MIN_INTERVAL_MS_MAX     1000
static int submit_min_interval_ms = SUBMIT_MIN_INTERVAL_MS_DEFAULT;

static uint64_t dagtech_now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

static void dagtech_submit_share(const DagTechJob *j, uint32_t nonce, int is_gpu) {
    /* Throttle: skip if we submitted too recently */
    uint64_t now_ms = dagtech_now_ms();
    pthread_mutex_lock(&submit_rate_mtx);
    uint64_t elapsed_ms = now_ms - last_submit_ms;
    if (elapsed_ms < (uint64_t)submit_min_interval_ms) {
        rate_limited_shares++;
        pthread_mutex_unlock(&submit_rate_mtx);
        return;
    }
    last_submit_ms = now_ms;
    pthread_mutex_unlock(&submit_rate_mtx);

    char nonce_hex[16];
    uint8_t nb[4];
    nb[0] = nonce & 0xff;
    nb[1] = (nonce >> 8) & 0xff;
    nb[2] = (nonce >> 16) & 0xff;
    nb[3] = (nonce >> 24) & 0xff;
    bytes_to_hex(nb, 4, nonce_hex);

    /* Capture and increment submission id before sending */
    pthread_mutex_lock(&stats_mtx);
    uint64_t sub_id = 1000 + total_submitted;
    total_submitted++;
    if (is_gpu) gpu_submitted++; else cpu_submitted++;
    pthread_mutex_unlock(&stats_mtx);

    /* Record pending so the pool response can be attributed to the right source */
    pthread_mutex_lock(&pending_mtx);
    int slot = (pending_head + pending_count) % PENDING_SUB_MAX;
    pending_subs[slot].id     = sub_id;
    pending_subs[slot].is_gpu = is_gpu;
    pending_subs[slot].diff   = j->difficulty;
    if (pending_count < PENDING_SUB_MAX) pending_count++;
    else pending_head = (pending_head + 1) % PENDING_SUB_MAX;
    pthread_mutex_unlock(&pending_mtx);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%lu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"00000000\",\"%s\",\"%s\"]}",
        (unsigned long)sub_id, wallet, j->job_id, j->ntime, nonce_hex);
    dagtech_send(buf);
}

/* External alias used by GPU thread (avoids forward-declaration complexity) */
void dagtech_submit_share_ext(const DagTechJob *j, uint32_t nonce) {
    dagtech_submit_share(j, nonce, 1);  /* GPU */
}

static int dagtech_check_target(const uint8_t *hash, double difficulty) {
    uint64_t hash_top64 = ((uint64_t)hash[31] << 56) | ((uint64_t)hash[30] << 48) |
                          ((uint64_t)hash[29] << 40) | ((uint64_t)hash[28] << 32) |
                          ((uint64_t)hash[27] << 24) | ((uint64_t)hash[26] << 16) |
                          ((uint64_t)hash[25] <<  8) |  (uint64_t)hash[24];
    double threshold_d = (double)0x0000FFFF00000000ULL / dagtech_effective_diff(difficulty);
    uint64_t threshold64 = (threshold_d >= 18446744073709551615.0) ?
                            0xFFFFFFFFFFFFFFFFULL : (uint64_t)threshold_d;
    return hash_top64 <= threshold64;
}

/* =========================================================================
 * CPU Mining Thread - DagTech Worker
 * CPU uses nonce range 0x00000000 - 0x7FFFFFFF
 * ========================================================================= */
static void *dagtech_mine_thread(void *arg) {
    int tid = *(int *)arg;
    /* Distribute CPU workers across the lower half of nonce space */
    uint32_t nonce = (uint32_t)tid * (0x7FFFFFFFu / (num_threads > 0 ? num_threads : 1));
    uint64_t local_hashes = 0;

    uint32_t *V = (uint32_t *)malloc(SCRYPT_N * 128);
    if (!V) {
        fprintf(stderr, "[DagCore] FATAL: Worker %d out of memory\n", tid);
        return NULL;
    }

    printf("[DagCore] CPU Worker %d started (nonce range 0x%08x)\n", tid, nonce);

    while (running) {
        DagTechJob j;
        pthread_mutex_lock(&job_mtx);
        j = current_job;
        pthread_mutex_unlock(&job_mtx);

        if (!j.valid) { usleep(100000); continue; }

        uint64_t job_seq = j.seq;

        long long t0 = dagtech_tick_ms();
        for (int batch = 0; batch < 64 && running; batch++) {
            /* Stay in CPU nonce range */
            nonce &= 0x7FFFFFFFu;

            uint8_t header[80];
            if (dagtech_make_header(&j, nonce, header) < 0) break;

            uint8_t hash[32];
            dagtech_hash(header, hash, V);
            local_hashes++;

            if (dagtech_check_target(hash, j.difficulty)) {
                printf("[DagCore] ** SHARE FOUND ** CPU Worker %d, nonce=0x%08x\n", tid, nonce);
                dagtech_submit_share(&j, nonce, 0);  /* CPU */
            }

            nonce++;
            nonce &= 0x7FFFFFFFu;

            /* Check for new job */
            pthread_mutex_lock(&job_mtx);
            if (current_job.seq != job_seq) {
                pthread_mutex_unlock(&job_mtx);
                break;
            }
            pthread_mutex_unlock(&job_mtx);
        }

        pthread_mutex_lock(&cpu_stats_mtx);
        cpu_hashes_session += local_hashes;
        pthread_mutex_unlock(&cpu_stats_mtx);

        pthread_mutex_lock(&stats_mtx);
        total_hashes += local_hashes;
        pthread_mutex_unlock(&stats_mtx);
        local_hashes = 0;

        /* CPU throttle: sleep proportional to batch time.
         * Sleeps in 100ms chunks so the thread can still respond quickly
         * to new jobs even when the correct sleep duration is many seconds.
         * The old 2000ms hard cap broke throttling on slower machines where
         * a 64-hash batch takes longer than 2s. */
        if (cpu_limit < 100) {
            long long elapsed = dagtech_tick_ms() - t0;
            if (elapsed > 0) {
                long long sleep_ms = elapsed * (100 - cpu_limit) / cpu_limit;
                long long slept = 0;
                while (slept < sleep_ms && running) {
                    long long chunk = sleep_ms - slept;
                    if (chunk > 100) chunk = 100;
                    usleep((unsigned int)(chunk * 1000));
                    slept += chunk;
                    /* Stop sleeping early if a new job arrived */
                    pthread_mutex_lock(&job_mtx);
                    int job_changed = (current_job.seq != job_seq);
                    pthread_mutex_unlock(&job_mtx);
                    if (job_changed) break;
                }
            }
        }
    }
    free(V);
    return NULL;
}

/* One nvidia-smi per metrics request, not one per value: the clocks ride
 * along with the telemetry that was already being queried. */
static int get_gpu_stats(double *temp, double *usage, double *memory, double *power,
                         double *core_clk, double *mem_clk) {
    FILE *fp = popen("nvidia-smi --query-gpu=temperature.gpu,utilization.gpu,memory.used,"
                     "power.draw,clocks.current.graphics,clocks.current.memory "
                     "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!fp) return -1;

    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1;
    }

    pclose(fp);

    if (sscanf(buf, "%lf, %lf, %lf, %lf, %lf, %lf",
               temp, usage, memory, power, core_clk, mem_clk) != 6)
        return -1;

    return 0;
}

static double get_cpu_temp(void) {
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) return -1.0;

    struct dirent *ent;
    /* Big enough for "/sys/class/hwmon/" + a maximum-length d_name (255) +
     * "/temp16_label" + NUL, so the snprintf() calls below cannot truncate. */
    char path[320], name[64], label[64];
    double temp = -1.0;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0) continue;

        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (!fgets(name, sizeof(name), f)) {
            fclose(f);
            continue;
        }
        fclose(f);

        if (strncmp(name, "coretemp", 8) != 0) continue;

        for (int i = 1; i <= 16; i++) {
            snprintf(path, sizeof(path),
                     "/sys/class/hwmon/%s/temp%d_label", ent->d_name, i);
            f = fopen(path, "r");
            if (!f) continue;

            if (!fgets(label, sizeof(label), f)) {
                fclose(f);
                continue;
            }
            fclose(f);

            if (strncmp(label, "Package id 0", 12) == 0) {
                snprintf(path, sizeof(path),
                         "/sys/class/hwmon/%s/temp%d_input", ent->d_name, i);
                f = fopen(path, "r");
                if (f) {
                    long millideg;
                    if (fscanf(f, "%ld", &millideg) == 1)
                        temp = millideg / 1000.0;
                    fclose(f);
                }
                closedir(dir);
                return temp;
            }
        }
    }

    closedir(dir);
    return temp;
}

/* =========================================================================
 * Control API (#46) - helpers
 * ========================================================================= */

/* Declared again here: the existing forward declaration lives inside the
 * #ifdef DAGTECH_GPU autotune block, so it is invisible to the CPU-only
 * build, where these helpers are compiled all the same. */
static void dagtech_mkdir_parents(const char *filepath);

/* Ask nvidia-smi for the power-limit envelope of one GPU. The CSV query is
 * used rather than "-q -d POWER" on purpose: that output also contains a
 * "Min"/"Max" pair for recent power *samples*, which a naive parse picks up
 * instead of the limits. Returns 0 on success. */
static int nvsmi_query_power(int idx, double *mn, double *mx, double *def, double *cur) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "nvidia-smi --query-gpu=power.min_limit,power.max_limit,"
             "power.default_limit,power.limit --format=csv,noheader,nounits -i %d 2>/dev/null",
             idx);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char buf[256];
    char *got = fgets(buf, sizeof(buf), fp);
    int rc = pclose(fp);
    if (!got || rc != 0) return -1;
    double a, b, c, d;
    if (sscanf(buf, "%lf , %lf , %lf , %lf", &a, &b, &c, &d) != 4) return -1;
    if (mn) *mn = a;
    if (mx) *mx = b;
    if (def) *def = c;
    if (cur) *cur = d;
    return 0;
}

/* ---- NVML clock offsets (#48) ------------------------------------------
 * nvidia-smi cannot set VF curve offsets, so this goes through NVML directly.
 * The library is opened with dlopen at first use rather than linked: the miner
 * must keep building and running on a machine with no NVIDIA driver at all,
 * and the CPU-only build must not grow a dependency on it.
 *
 * Only the long-stable pair nvmlDevice{Get,Set}{Gpc,Mem}ClkVfOffset is used.
 * Driver 595 also exports nvmlDeviceGetClockOffsets, which would report the
 * permitted minimum and maximum, but its argument struct is not in the NVML
 * headers shipped here (API version 12) and guessing a struct layout that the
 * driver writes into is how you get memory corruption, not clock limits. So
 * the envelope below is a conservative guard and NVML has the final say -
 * whatever it refuses is reported back verbatim.
 *
 * DAGCORE_NVML_LIB redirects the library, which is what makes this testable
 * without touching a real card. */

#ifndef _WIN32
typedef int (*nvml_init_fn)(void);
typedef int (*nvml_shutdown_fn)(void);
typedef int (*nvml_handle_fn)(unsigned int, void **);
typedef int (*nvml_get_off_fn)(void *, int *);
typedef int (*nvml_set_off_fn)(void *, int);
typedef int (*nvml_minmax_off_fn)(void *, int *, int *);
typedef int (*nvml_lock_fn)(void *, unsigned int, unsigned int);
typedef int (*nvml_unlock_fn)(void *);
typedef int (*nvml_clkinfo_fn)(void *, int, unsigned int *);
typedef int (*nvml_supmem_fn)(void *, unsigned int *, unsigned int *);
typedef int (*nvml_pstate_fn)(void *, int, int, unsigned int *, unsigned int *);
typedef const char *(*nvml_errstr_fn)(int);

#define NVML_CLK_GRAPHICS 0
#define NVML_CLK_MEM      2

static struct {
    int   tried;          /* 0 = not attempted yet */
    int   ok;             /* 1 = usable */
    void *lib;
    void *dev;
    nvml_shutdown_fn   shutdown;
    nvml_get_off_fn    get_core, get_mem;
    nvml_set_off_fn    set_core, set_mem;
    nvml_minmax_off_fn range_core, range_mem;
    nvml_lock_fn       lock_core, lock_mem;
    nvml_unlock_fn     unlock_core, unlock_mem;
    nvml_clkinfo_fn    max_clk;
    nvml_supmem_fn     sup_mem;
    nvml_pstate_fn     pstate;
    nvml_errstr_fn     errstr;
    char  why[160];       /* why it is unusable, for the dashboard */
} g_nvml;

static const char *nvml_err(int rc) {
    if (g_nvml.errstr) return g_nvml.errstr(rc);
    return "NVML error";
}

/* Resolve everything up front: a half-loaded NVML is worse than none. */
static int nvml_load(void) {
    if (g_nvml.tried) return g_nvml.ok;
    g_nvml.tried = 1;
    snprintf(g_nvml.why, sizeof(g_nvml.why), "not initialised");

    const char *libname = getenv("DAGCORE_NVML_LIB");
    if (!libname || !libname[0]) libname = "libnvidia-ml.so.1";

    g_nvml.lib = dlopen(libname, RTLD_LAZY);
    if (!g_nvml.lib) {
        snprintf(g_nvml.why, sizeof(g_nvml.why), "cannot load %s", libname);
        return 0;
    }
    nvml_init_fn   init = (nvml_init_fn)  dlsym(g_nvml.lib, "nvmlInit_v2");
    nvml_handle_fn hnd  = (nvml_handle_fn)dlsym(g_nvml.lib, "nvmlDeviceGetHandleByIndex_v2");
    g_nvml.shutdown     = (nvml_shutdown_fn)dlsym(g_nvml.lib, "nvmlShutdown");
    g_nvml.get_core     = (nvml_get_off_fn)dlsym(g_nvml.lib, "nvmlDeviceGetGpcClkVfOffset");
    g_nvml.set_core     = (nvml_set_off_fn)dlsym(g_nvml.lib, "nvmlDeviceSetGpcClkVfOffset");
    g_nvml.get_mem      = (nvml_get_off_fn)dlsym(g_nvml.lib, "nvmlDeviceGetMemClkVfOffset");
    g_nvml.set_mem      = (nvml_set_off_fn)dlsym(g_nvml.lib, "nvmlDeviceSetMemClkVfOffset");
    g_nvml.range_core   = (nvml_minmax_off_fn)dlsym(g_nvml.lib, "nvmlDeviceGetGpcClkMinMaxVfOffset");
    g_nvml.range_mem    = (nvml_minmax_off_fn)dlsym(g_nvml.lib, "nvmlDeviceGetMemClkMinMaxVfOffset");
    g_nvml.lock_core    = (nvml_lock_fn)dlsym(g_nvml.lib, "nvmlDeviceSetGpuLockedClocks");
    g_nvml.lock_mem     = (nvml_lock_fn)dlsym(g_nvml.lib, "nvmlDeviceSetMemoryLockedClocks");
    g_nvml.unlock_core  = (nvml_unlock_fn)dlsym(g_nvml.lib, "nvmlDeviceResetGpuLockedClocks");
    g_nvml.unlock_mem   = (nvml_unlock_fn)dlsym(g_nvml.lib, "nvmlDeviceResetMemoryLockedClocks");
    g_nvml.max_clk      = (nvml_clkinfo_fn)dlsym(g_nvml.lib, "nvmlDeviceGetMaxClockInfo");
    g_nvml.sup_mem      = (nvml_supmem_fn)dlsym(g_nvml.lib, "nvmlDeviceGetSupportedMemoryClocks");
    g_nvml.pstate       = (nvml_pstate_fn)dlsym(g_nvml.lib, "nvmlDeviceGetMinMaxClockOfPState");
    g_nvml.errstr       = (nvml_errstr_fn)dlsym(g_nvml.lib, "nvmlErrorString");

    if (!init || !hnd || !g_nvml.get_core || !g_nvml.set_core ||
        !g_nvml.get_mem || !g_nvml.set_mem || !g_nvml.lock_core || !g_nvml.lock_mem ||
        !g_nvml.unlock_core || !g_nvml.unlock_mem || !g_nvml.max_clk || !g_nvml.sup_mem) {
        snprintf(g_nvml.why, sizeof(g_nvml.why),
                 "%s lacks the clock entry points", libname);
        return 0;
    }
    int rc = init();
    if (rc != 0) {
        snprintf(g_nvml.why, sizeof(g_nvml.why), "nvmlInit failed: %s", nvml_err(rc));
        return 0;
    }
    rc = hnd((unsigned)gpu_device, &g_nvml.dev);
    if (rc != 0) {
        snprintf(g_nvml.why, sizeof(g_nvml.why), "no NVML handle for GPU %d: %s",
                 gpu_device, nvml_err(rc));
        return 0;
    }
    g_nvml.ok = 1;
    snprintf(g_nvml.why, sizeof(g_nvml.why), "ok");
    return 1;
}

static int nvml_read_offsets(int *core, int *mem) {
    if (!nvml_load()) return -1;
    int c = 0, m = 0;
    if (g_nvml.get_core(g_nvml.dev, &c) != 0) return -1;
    if (g_nvml.get_mem(g_nvml.dev, &m) != 0) return -1;
    if (core) *core = c;
    if (mem) *mem = m;
    return 0;
}

static int nvml_write_offset(int is_mem, int mhz, char *err, size_t err_size) {
    if (!nvml_load()) { snprintf(err, err_size, "%s", g_nvml.why); return -1; }
    int rc = is_mem ? g_nvml.set_mem(g_nvml.dev, mhz) : g_nvml.set_core(g_nvml.dev, mhz);
    if (rc != 0) { snprintf(err, err_size, "%s", nvml_err(rc)); return -1; }
    return 0;
}

/* The offset range the card itself permits. Replaces the constants this code
 * used to carry: on an RTX 3080 those were wrong in both directions - core
 * guarded to +1500 where the card allows +1000, memory to +4000 where it
 * allows +6000. */
static int nvml_offset_bounds(int is_mem, int *lo, int *hi) {
    if (!nvml_load()) return -1;
    nvml_minmax_off_fn f = is_mem ? g_nvml.range_mem : g_nvml.range_core;
    if (!f) return -1;
    int a = 0, b = 0;
    if (f(g_nvml.dev, &a, &b) != 0) return -1;
    if (lo) *lo = a;
    if (hi) *hi = b;
    return 0;
}

/* How far a memory offset displaces the clock table.
 *
 * The memory offset is expressed in data-rate MHz, and the clock moves by half
 * of it. Measured on an RTX 3080 with offset +1200: the base table tops out at
 * 9501 MHz, the card reports a 10101 MHz ceiling (9501 + 600), and a lock of
 * 9851 MHz - the value nvidia_oc had applied - is exactly 9251 + 600, the
 * second entry of the displaced table. Two exact matches, so the halving is
 * measured rather than assumed.
 *
 * The graphics offset is taken as 1:1, which is the conventional behaviour but
 * could not be verified here: the core offset on this card is 0, so there is
 * nothing to measure against. The ceiling does not depend on it either way -
 * that comes from the card - so only the floor would shift. */
static int nvml_mem_shift(void) {
    int off = 0;
    if (!nvml_load()) return 0;
    if (g_nvml.get_mem(g_nvml.dev, &off) != 0) return 0;
    return off / 2;
}

/* What a lock will actually accept, asked fresh every time: a new offset moves
 * the whole table, so a value cached at startup goes stale the moment someone
 * changes one. */
static int nvml_clock_bounds(int is_mem, int *lo, int *hi) {
    if (!nvml_load()) return -1;
    unsigned int top = 0;
    if (g_nvml.max_clk(g_nvml.dev, is_mem ? NVML_CLK_MEM : NVML_CLK_GRAPHICS, &top) != 0)
        return -1;

    unsigned int bottom = 0;
    if (is_mem) {
        /* Lowest entry of the base table, displaced like the rest of it. */
        unsigned int n = 32, tab[32];
        if (g_nvml.sup_mem(g_nvml.dev, &n, tab) != 0 || n == 0) return -1;
        bottom = tab[0];
        for (unsigned int i = 1; i < n; i++) if (tab[i] < bottom) bottom = tab[i];
        int shift = nvml_mem_shift();
        long b = (long)bottom + shift;
        bottom = (unsigned int)(b < 0 ? 0 : b);
    } else if (g_nvml.pstate) {
        unsigned int a = 0, b = 0;
        if (g_nvml.pstate(g_nvml.dev, NVML_CLK_GRAPHICS, 0, &a, &b) == 0) bottom = a;
    }
    if (lo) *lo = (int)bottom;
    if (hi) *hi = (int)top;
    return 0;
}

/* How far an offset displaces a domain's clock table.
 *
 * Memory: half the offset, measured (see nvml_mem_shift).
 * Core: taken as 1:1. That is the conventional behaviour but it is unverified
 * here - the core offset on this card is 0, so there is nothing to measure
 * against. With a zero offset the two readings coincide either way. */
static int clk_shift(int is_mem) {
    if (is_mem) return nvml_mem_shift();
    int off = 0;
    if (!nvml_load() || g_nvml.get_core(g_nvml.dev, &off) != 0) return 0;
    return off;
}

static int clk_effective(int is_mem, int base) {
    if (base <= 0) return 0;
    return base + clk_shift(is_mem);
}

/* Nearest entry of the base table. Memory has an explicit table from NVML;
 * graphics is a 15 MHz ladder (2100, 2085, ... 210 on this card), so the
 * nearest multiple of 15 inside the range is the same answer. */
static int clk_snap_base(int is_mem, int want) {
    if (want <= 0) return 0;
    if (is_mem) {
        if (!nvml_load() || !g_nvml.sup_mem) return want;
        unsigned int n = 32, tab[32];
        if (g_nvml.sup_mem(g_nvml.dev, &n, tab) != 0 || n == 0) return want;
        int best = (int)tab[0], bestd = abs(want - (int)tab[0]);
        for (unsigned int i = 1; i < n; i++) {
            int d = abs(want - (int)tab[i]);
            if (d < bestd) { bestd = d; best = (int)tab[i]; }
        }
        return best;
    }
    int lo = 210, hi = 2100;
    int top = 0, sh = clk_shift(0);
    if (nvml_clock_bounds(0, NULL, &top) == 0 && top > 0) hi = top - sh;
    int v = ((want + 7) / 15) * 15;
    if (v < lo) v = lo;
    if (v > hi) v = (hi / 15) * 15;
    return v;
}

/* Lock a domain to a single clock. This is what nvidia_oc does, and what
 * nvidia-smi -lgc/-lmc does underneath - but going straight to NVML avoids
 * nvidia-smi's own client-side validation, which rejects values the driver
 * accepts once an offset has moved the table. */
static int nvml_lock_clock(int is_mem, int mhz, char *err, size_t err_size) {
    if (!nvml_load()) { snprintf(err, err_size, "%s", g_nvml.why); return -1; }
    nvml_lock_fn f = is_mem ? g_nvml.lock_mem : g_nvml.lock_core;
    int rc = f(g_nvml.dev, (unsigned int)mhz, (unsigned int)mhz);
    if (rc != 0) { snprintf(err, err_size, "%s", nvml_err(rc)); return -1; }
    return 0;
}

static int nvml_unlock_clock(int is_mem, char *err, size_t err_size) {
    if (!nvml_load()) { snprintf(err, err_size, "%s", g_nvml.why); return -1; }
    nvml_unlock_fn f = is_mem ? g_nvml.unlock_mem : g_nvml.unlock_core;
    int rc = f(g_nvml.dev);
    if (rc != 0) { snprintf(err, err_size, "%s", nvml_err(rc)); return -1; }
    return 0;
}

/* Clock currently reported for a domain, used to spot a lock that no longer
 * matches the table after an offset change. */
static int nvml_current_clock(int is_mem, int *mhz) {
    if (!nvml_load()) return -1;
    nvml_clkinfo_fn f = (nvml_clkinfo_fn)dlsym(g_nvml.lib, "nvmlDeviceGetClockInfo");
    if (!f) return -1;
    unsigned int v = 0;
    if (f(g_nvml.dev, is_mem ? NVML_CLK_MEM : NVML_CLK_GRAPHICS, &v) != 0) return -1;
    *mhz = (int)v;
    return 0;
}

#else
static int nvml_load(void) { return 0; }
static int nvml_read_offsets(int *c, int *m) { (void)c; (void)m; return -1; }
static int nvml_write_offset(int a, int b, char *e, size_t n) {
    (void)a; (void)b; snprintf(e, n, "NVML is not wired up on Windows"); return -1;
}
static int nvml_offset_bounds(int a, int *l, int *h) { (void)a; (void)l; (void)h; return -1; }
static int nvml_clock_bounds(int a, int *l, int *h)  { (void)a; (void)l; (void)h; return -1; }
static int nvml_mem_shift(void) { return 0; }
static int nvml_lock_clock(int a, int b, char *e, size_t n) {
    (void)a; (void)b; snprintf(e, n, "NVML is not wired up on Windows"); return -1;
}
static int nvml_unlock_clock(int a, char *e, size_t n) {
    (void)a; snprintf(e, n, "NVML is not wired up on Windows"); return -1;
}
static int nvml_current_clock(int a, int *m) { (void)a; (void)m; return -1; }
#endif

/* Apply a power limit. Needs root (the service runs as root); nvidia-smi
 * prints the reason on failure, so it is captured and logged rather than
 * reduced to an exit code. */
static int nvsmi_set_power_limit(int idx, int watts, char *err, size_t err_size) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "nvidia-smi -i %d -pl %d 2>&1", idx, watts);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(err, err_size, "cannot run nvidia-smi");
        return -1;
    }
    char line[256], last[256] = "";
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (l) { strncpy(last, line, sizeof(last) - 1); last[sizeof(last) - 1] = '\0'; }
    }
    int rc = pclose(fp);
    if (rc != 0) {
        snprintf(err, err_size, "%s", last[0] ? last : "nvidia-smi failed");
        return -1;
    }
    return 0;
}

/* Defined below, next to the rest of the override handling. */
static int overrides_set(const char *key, const char *value);

/* ---- Clock trial (#47) --------------------------------------------------
 * A clock lock is applied live but NOT persisted straight away. It has to
 * survive ten minutes without the rejected-share counter moving before it is
 * written to overrides.env. Until then a crash, a hang or any restart brings
 * the card back to the last setting that actually proved itself, because
 * nothing on disk mentions the new one.
 *
 * Core and memory each get their own trial, so tuning one does not silently
 * discard the other's probation. A reset is saved immediately: returning to
 * the driver's own management is the safe direction and needs no proof. */
#define TRIAL_MS (10 * 60 * 1000)
typedef struct {
    int      active;
    int      value;              /* value under test (step, or offset MHz) */
    int      prev;               /* what to come back to if it goes wrong */
    uint64_t start_ms;
    uint64_t rejected_at_start;
    int      rejected_seen;      /* rejects that appeared during the trial */
    char     status[16];         /* none | running | saved | failed */
} ClockTrial;
/* [0] core clock, [1] memory clock, [2] core offset, [3] memory offset. */
#define TRIAL_N 4
static ClockTrial g_trial[TRIAL_N] = {
    {0,0,0,0,0,0,"none"}, {0,0,0,0,0,0,"none"},
    {0,0,0,0,0,0,"none"}, {0,0,0,0,0,0,"none"}
};
static const char *TRIAL_KEY[TRIAL_N] = {
    "GPU_CORE_CLOCK_BASE", "GPU_MEM_CLOCK_BASE", "GPU_CORE_OFFSET", "GPU_MEM_OFFSET"
};

/* Put a domain back to the value it had before the trial. Used when the test
 * fails and when the operator cancels it - "the previous value comes back
 * automatically" has to be literally true, or the promise in the help text is
 * a lie. */
static void trial_revert(int which) {
    char err[200] = "";
    int prev = g_trial[which].prev;
    if (which <= 1) {                       /* clock lock: step, or none */
        if (prev > 0) {
            if (which) gpu_mem_clock_base = prev; else gpu_core_clock_base = prev;
            nvml_lock_clock(which, clk_effective(which, prev), err, sizeof(err));
        } else {
            if (which) gpu_mem_clock_base = 0; else gpu_core_clock_base = 0;
            nvml_unlock_clock(which, err, sizeof(err));
        }
    } else {                                /* offset */
        int is_mem = (which == 3);
        if (is_mem) gpu_mem_offset = prev; else gpu_core_offset = prev;
        nvml_write_offset(is_mem, prev, err, sizeof(err));
        /* An offset move displaces the table, so a lock has to follow it. */
        int base = is_mem ? gpu_mem_clock_base : gpu_core_clock_base;
        if (base > 0) nvml_lock_clock(is_mem, clk_effective(is_mem, base), err, sizeof(err));
    }
}

static void trial_start(int which, int value, int prev) {
    g_trial[which].active = 1;
    g_trial[which].value = value;
    g_trial[which].prev = prev;
    g_trial[which].rejected_seen = 0;
    g_trial[which].start_ms = dagtech_now_ms();
    pthread_mutex_lock(&stats_mtx);
    g_trial[which].rejected_at_start = total_rejected;
    pthread_mutex_unlock(&stats_mtx);
    snprintf(g_trial[which].status, sizeof(g_trial[which].status), "running");
}

/* Rejected shares seen since the trial began - live while it runs, frozen at
 * the verdict once it ends, so the dashboard can show why it failed. */
static int trial_rejects(int which) {
    if (!g_trial[which].active) return g_trial[which].rejected_seen;
    pthread_mutex_lock(&stats_mtx);
    uint64_t rej = total_rejected;
    pthread_mutex_unlock(&stats_mtx);
    if (rej <= g_trial[which].rejected_at_start) return 0;
    return (int)(rej - g_trial[which].rejected_at_start);
}

static int trial_remaining_s(int which) {
    if (!g_trial[which].active) return 0;
    uint64_t gone = dagtech_now_ms() - g_trial[which].start_ms;
    if (gone >= TRIAL_MS) return 0;
    return (int)((TRIAL_MS - gone) / 1000);
}

/* Called once a second from the statistics loop. */
static void trial_tick(void) {
    for (int i = 0; i < TRIAL_N; i++) {
        if (!g_trial[i].active) continue;
        pthread_mutex_lock(&stats_mtx);
        uint64_t rej = total_rejected;
        pthread_mutex_unlock(&stats_mtx);

        if (rej != g_trial[i].rejected_at_start) {
            g_trial[i].rejected_seen = (int)(rej - g_trial[i].rejected_at_start);
            g_trial[i].active = 0;
            snprintf(g_trial[i].status, sizeof(g_trial[i].status), "failed");
            fprintf(stderr, "[DagCore] Trial FAILED: %d rejected share%s appeared; "
                    "%s=%d not saved, reverting to %d\n",
                    g_trial[i].rejected_seen, g_trial[i].rejected_seen == 1 ? "" : "s",
                    TRIAL_KEY[i], g_trial[i].value, g_trial[i].prev);
            trial_revert(i);
            continue;
        }
        if (dagtech_now_ms() - g_trial[i].start_ms < TRIAL_MS) continue;

        char val[16];
        snprintf(val, sizeof(val), "%d", g_trial[i].value);
        if (overrides_set(TRIAL_KEY[i], val) == 0) {
            snprintf(g_trial[i].status, sizeof(g_trial[i].status), "saved");
            printf("[DagCore] Clock trial passed: %s=%d saved after 10 min with no new rejects\n",
                   TRIAL_KEY[i], g_trial[i].value);
        } else {
            snprintf(g_trial[i].status, sizeof(g_trial[i].status), "failed");
            fprintf(stderr, "[DagCore] Clock trial passed but overrides could not be written\n");
        }
        g_trial[i].active = 0;
    }
}

/* Only these keys may ever be written by the API. Anything else - wallet,
 * pool, metrics bind - stays the operator's to set in config.env. */
static int overrides_key_allowed(const char *key) {
    return strcmp(key, "GPU_POWER_LIMIT") == 0 ||
           strcmp(key, "GPU_INTENSITY")   == 0 ||
           strcmp(key, "GPU_CORE_CLOCK")  == 0 ||
           strcmp(key, "GPU_MEM_CLOCK")   == 0 ||
           strcmp(key, "GPU_CORE_CLOCK_BASE") == 0 ||
           strcmp(key, "GPU_MEM_CLOCK_BASE")  == 0 ||
           strcmp(key, "GPU_CORE_OFFSET") == 0 ||
           strcmp(key, "GPU_MEM_OFFSET")  == 0;
}

static void overrides_apply(const char *key, const char *val) {
    if (strcmp(key, "GPU_POWER_LIMIT") == 0) gpu_power_limit = atoi(val);
    else if (strcmp(key, "GPU_CORE_CLOCK") == 0) gpu_core_clock = atoi(val);
    else if (strcmp(key, "GPU_MEM_CLOCK")  == 0) gpu_mem_clock  = atoi(val);
    else if (strcmp(key, "GPU_CORE_CLOCK_BASE") == 0) gpu_core_clock_base = atoi(val);
    else if (strcmp(key, "GPU_MEM_CLOCK_BASE")  == 0) gpu_mem_clock_base  = atoi(val);
    else if (strcmp(key, "GPU_CORE_OFFSET") == 0) gpu_core_offset = atoi(val);
    else if (strcmp(key, "GPU_MEM_OFFSET")  == 0) gpu_mem_offset  = atoi(val);
    else if (strcmp(key, "GPU_INTENSITY") == 0) {
        int v = atoi(val);
        if (v >= 0 && v <= 100) { gpu_intensity = v; gpu_intensity_count = 0; }
    }
}

/* Load the override file on top of config.env. Unknown keys are ignored, not
 * an error: the file is machine-written, and a key from a newer version must
 * not stop an older miner from starting. */
static void overrides_load(void) {
    const char *path = dt_overrides_path();
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (!overrides_key_allowed(line)) continue;
        overrides_apply(line, eq + 1);
        n++;
    }
    fclose(f);
    if (n) printf("[DagCore] Overrides loaded from %s (%d key%s)\n", path, n, n == 1 ? "" : "s");
}

/* Rewrite the override file with one key changed. Every other line is copied
 * through verbatim, and the result is renamed into place, so a crash halfway
 * cannot leave a truncated file behind. */
static int overrides_set(const char *key, const char *value) {
    if (!overrides_key_allowed(key)) return -1;
    const char *path = dt_overrides_path();
    dagtech_mkdir_parents(path);

    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) return -1;

    int written = 0;
    FILE *in = fopen(path, "r");
    if (in) {
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            char probe[512];
            strncpy(probe, line, sizeof(probe) - 1);
            probe[sizeof(probe) - 1] = '\0';
            char *eq = strchr(probe, '=');
            if (eq) {
                *eq = '\0';
                if (strcmp(probe, key) == 0) {
                    fprintf(out, "%s=%s\n", key, value);
                    written = 1;
                    continue;
                }
            }
            fputs(line, out);
        }
        fclose(in);
    } else {
        fprintf(out, "# Written by the DAGCore control API. Edit config.env instead.\n");
    }
    if (!written) fprintf(out, "%s=%s\n", key, value);
    if (fclose(out) != 0) { remove(tmp); return -1; }
#ifndef _WIN32
    chmod(tmp, 0640);
#endif
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* Read the API token, creating it on first start. 0600 is set before anything
 * is written, so the secret is never briefly world-readable. */
static void token_init(void) {
    const char *path = dt_token_path();
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(g_api_token, sizeof(g_api_token), f)) {
            size_t l = strlen(g_api_token);
            while (l > 0 && (g_api_token[l-1] == '\n' || g_api_token[l-1] == '\r' ||
                             g_api_token[l-1] == ' ')) g_api_token[--l] = '\0';
        }
        fclose(f);
        if (g_api_token[0]) return;
    }

    unsigned char raw[32];
    int have = 0;
#ifndef _WIN32
    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) {
        have = (fread(raw, 1, sizeof(raw), ur) == sizeof(raw));
        fclose(ur);
    }
#endif
    if (!have) {
        fprintf(stderr, "[DagCore] WARNING: no /dev/urandom; control API disabled\n");
        g_api_token[0] = '\0';
        return;
    }
    for (size_t i = 0; i < sizeof(raw); i++)
        snprintf(g_api_token + i * 2, 3, "%02x", raw[i]);

    dagtech_mkdir_parents(path);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[DagCore] WARNING: cannot write %s: %s - control API disabled\n",
                path, strerror(errno));
        g_api_token[0] = '\0';
        return;
    }
#ifndef _WIN32
    chmod(path, 0600);
#endif
    fprintf(f, "%s\n", g_api_token);
    fclose(f);
    printf("[DagCore] Control API token created in %s\n", path);
    printf("[DagCore] Token: %s\n", g_api_token);
    printf("[DagCore]        paste it into the dashboard to enable the controls\n");
}

/* Length-independent comparison, so a wrong token cannot be narrowed down by
 * timing the reply. */
static int token_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), i;
    unsigned char diff = (unsigned char)(la ^ lb);
    for (i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ (i < lb ? b[i] : 0));
    return diff == 0;
}

/* Decide once at startup whether the controls can work at all, and say why
 * not - the dashboard shows the reason instead of dead buttons. */
static void control_init(void) {
    token_init();
    g_control_ok = 0;
    if (!g_api_token[0]) {
        snprintf(g_control_reason, sizeof(g_control_reason), "no API token");
        return;
    }
    if (gpu_enabled != 1) {
        snprintf(g_control_reason, sizeof(g_control_reason), "GPU mining is disabled");
        return;
    }
    if (g_num_gpus > 1) {
        /* The miner indexes GPUs through OpenCL, nvidia-smi through NVML, and
         * the two orders are not guaranteed to agree. Setting a power limit on
         * the wrong card is worse than not offering the control. */
        snprintf(g_control_reason, sizeof(g_control_reason),
                 "multi-GPU: needs PCI bus-ID mapping (not implemented)");
        return;
    }
    if (nvsmi_query_power(gpu_device, &g_pl_min, &g_pl_max, &g_pl_default, &g_pl_current) != 0) {
        snprintf(g_control_reason, sizeof(g_control_reason), "nvidia-smi not available");
        return;
    }
    /* Clock envelope now comes from NVML, and is re-read per request rather
     * than cached: an offset change moves the whole table. A failure here does
     * not disable the controls - the power limit still works and the page
     * hides the clock rows when the range is unknown. */
    if (nvml_clock_bounds(0, &g_core_lo, &g_core_hi) != 0)
        fprintf(stderr, "[DagCore] WARNING: no core clock range from NVML\n");
    if (nvml_clock_bounds(1, &g_mem_lo, &g_mem_hi) != 0)
        fprintf(stderr, "[DagCore] WARNING: no memory clock range from NVML\n");
    g_core_boost = g_core_hi;
    g_mem_boost  = g_mem_hi;

    g_control_ok = 1;
    snprintf(g_control_reason, sizeof(g_control_reason), "ok");
}

/* A string as a JSON string body (no quotes): escapes quotes, backslashes -
 * every Windows path - and control characters. Truncates to fit. */
static void json_escape(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)          o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else                        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* GPU_DEVICE as it would be written: "all", "0,1" or "0". */
static void gpu_device_sel_str(char *out, size_t n) {
    if (gpu_use_all) { snprintf(out, n, "all"); return; }
    if (gpu_device_count > 0) {
        size_t o = 0;
        out[0] = '\0';
        for (int i = 0; i < gpu_device_count && o < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s%d", i ? "," : "", gpu_device_list[i]);
        return;
    }
    snprintf(out, n, "%d", gpu_device);
}

/* ---- tiny HTTP helpers (this server speaks just enough HTTP) ---- */
static void http_send_json(int fd, int status, const char *reason, const char *body) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
             "Access-Control-Allow-Origin: *\r\nConnection: close\r\n"
             "Content-Length: %d\r\n\r\n", status, reason, (int)strlen(body));
    send(fd, hdr, (int)strlen(hdr), 0);
    send(fd, body, (int)strlen(body), 0);
}

static void http_send_err(int fd, int status, const char *reason, const char *msg) {
    char body[320];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    http_send_json(fd, status, reason, body);
}

/* Where the value of "<key>" starts in a flat JSON object, or NULL. Only a
 * quoted key followed by a colon counts, so a string value that happens to
 * read "port" is not mistaken for the key - /api/config sends several keys,
 * and a pool could be named anything. */
static const char *json_value_of(const char *body, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    for (const char *p = strstr(body, pat); p; p = strstr(p + 1, pat)) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        return q;
    }
    return NULL;
}

/* Pull one integer out of a flat JSON object. The bodies this API accepts are
 * flat and small, so a full parser would be more code than the feature.
 * Anything more complex is rejected by the range checks. */
static int json_get_int(const char *body, const char *key, long *out) {
    const char *p = json_value_of(body, key);
    if (!p) return -1;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return -1;
    *out = v;
    return 0;
}

/* True when the body carries "<key>": true. The bodies this API accepts are
 * one key and one value, so this is enough. */
static int json_is_true(const char *body, const char *key) {
    const char *p = json_value_of(body, key);
    return p && strncmp(p, "true", 4) == 0;
}

/* A string value, copied out. -1 if the key is missing or not a string,
 * -2 if the value is too long or uses escapes: nothing /api/config accepts
 * (addresses, host names, names, device lists) ever needs one. */
static int json_get_str(const char *body, const char *key, char *out, size_t n) {
    const char *p = json_value_of(body, key);
    if (!p || *p != '"') return -1;
    p++;
    size_t o = 0;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' || o + 1 >= n) return -2;
        out[o++] = *p;
    }
    if (*p != '"') return -1;
    out[o] = '\0';
    return 0;
}

/* The path out of a request line: "GET /help?x HTTP/1.1" -> "/help". */
static void http_path(const char *req, char *out, size_t out_size) {
    out[0] = '\0';
    const char *sp = strchr(req, ' ');
    if (!sp) return;
    sp++;
    size_t i = 0;
    while (sp[i] && sp[i] != ' ' && sp[i] != '?' && sp[i] != '\r' && i + 1 < out_size) {
        out[i] = sp[i];
        i++;
    }
    out[i] = '\0';
}

/* Serve one file out of the dashboard directory.
 *
 * The name is chosen from a fixed set below, never taken from the request, so
 * there is no path for a caller to traverse out of the directory. */
static void serve_dashboard_file(int cfd, const char *name, const char *ctype,
                                 const char *extra_hdr) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dashboard_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(cfd, nf, (int)strlen(nf), 0);
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(fsize + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)fsize, f);
        if (got != (size_t)fsize) fsize = (long)got;   /* serve what we read */
        buf[fsize] = '\0';
        char hdr[320];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%s"
                 "Connection: close\r\nContent-Length: %ld\r\n\r\n",
                 ctype, extra_hdr ? extra_hdr : "", fsize);
        send(cfd, hdr, (int)strlen(hdr), 0);
        send(cfd, buf, (int)fsize, 0);
        free(buf);
    }
    fclose(f);
}

/* Case-insensitive header lookup; copies the value into out. */
static int http_header(const char *req, const char *name, char *out, size_t out_size) {
    size_t nl = strlen(name);
    const char *p = req;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol == p) break;
        size_t i = 0;
        while (i < nl && p[i] && (tolower((unsigned char)p[i]) == tolower((unsigned char)name[i]))) i++;
        if (i == nl && p[i] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t len = (size_t)(eol - v);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 0;
        }
        p = eol + 2;
    }
    return -1;
}

/* Route and serve one control request. Auth first, capability second, then
 * the endpoint - so an unauthenticated caller learns nothing about the rig. */
/* ---- Basic configuration from the dashboard: POST /api/config ----------
 *
 * Wallet, pool, port, worker, CPU threads and GPU selection, written into
 * config.env itself. This reverses an earlier rule - the browser never
 * touched config.env, so it could never redirect payouts - on purpose: the
 * point is to configure a rig without a shell. What protects it now:
 *   - the control token, as for every other change;
 *   - validation of every field before anything is written, so the miner
 *     cannot be left with a config it will not start with;
 *   - the file is rewritten through a temporary and renamed into place, and
 *     the previous version is kept as config.env.bak;
 *   - only these six keys change; every other line, comment included, is
 *     copied through;
 *   - a setting given on the command line is refused: it overrides
 *     config.env on every start, so saving it there would change nothing.
 * Like intensity, it takes effect on a restart: under systemd the miner exits
 * and is started again, otherwise the reply says to restart it. */

static int cfg_valid_wallet(const char *w) {
    if (strlen(w) != 42 || w[0] != '0' || (w[1] != 'x' && w[1] != 'X')) return 0;
    int nonzero = 0;
    for (int i = 2; i < 42; i++) {
        if (!isxdigit((unsigned char)w[i])) return 0;
        if (w[i] != '0') nonzero = 1;
    }
    return nonzero;           /* the all-zero address is the example's placeholder */
}

/* A host name or IPv4 address as the Stratum connect expects it: no scheme,
 * no port, no spaces. Whether it resolves is checked separately. */
static int cfg_valid_host(const char *h) {
    size_t l = strlen(h);
    if (l == 0 || l > 253 || h[0] == '.' || h[0] == '-' || h[l-1] == '.' || h[l-1] == '-')
        return 0;
    for (size_t i = 0; i < l; i++)
        if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-') return 0;
    return 1;
}

static int cfg_valid_worker(const char *w) {
    size_t l = strlen(w);
    if (l == 0 || l >= sizeof(worker_name)) return 0;
    for (size_t i = 0; i < l; i++)
        if (!isalnum((unsigned char)w[i]) && w[i] != '.' && w[i] != '_' && w[i] != '-') return 0;
    return 1;
}

/* "all", "N" or "N,M,...": every index a detected card, none twice. Written
 * back normalised (no spaces). */
static int cfg_parse_gpu_sel(const char *v, char *norm, size_t n, char *err, size_t en) {
    if (g_gpu_detected == 0) {
        snprintf(err, en, "no GPU detected - this build or this machine cannot select one");
        return -1;
    }
    if (strcmp(v, "all") == 0) { snprintf(norm, n, "all"); return 0; }
    int seen[MAX_GPUS] = {0}, count = 0;
    size_t o = 0;
    norm[0] = '\0';
    const char *p = v;
    while (*p) {
        if (!isdigit((unsigned char)*p)) {
            snprintf(err, en, "gpu_device must be all, a card number, or numbers separated by commas");
            return -1;
        }
        char *end;
        long idx = strtol(p, &end, 10);
        if (idx < 0 || idx >= g_gpu_detected) {
            snprintf(err, en, "gpu_device: there is no card %ld (detected: 0 to %d)",
                     idx, g_gpu_detected - 1);
            return -1;
        }
        if (seen[idx]) {
            snprintf(err, en, "gpu_device: card %ld is listed twice", idx);
            return -1;
        }
        seen[idx] = 1;
        o += (size_t)snprintf(norm + o, n - o, "%s%ld", count ? "," : "", idx);
        count++;
        p = end;
        if (*p == ',') { p++; if (!*p) break; }
        else if (*p) {
            snprintf(err, en, "gpu_device must be all, a card number, or numbers separated by commas");
            return -1;
        }
    }
    if (count == 0) {
        snprintf(err, en, "gpu_device is empty");
        return -1;
    }
    return 0;
}

/* Rewrite config.env with the given keys set, as dagtech_load_config reads
 * it: KEY=value at the start of a line, '#' comments, last occurrence wins.
 * The first occurrence of each key is replaced and later ones dropped, so an
 * old duplicate further down cannot undo the change. The previous file is
 * kept as <path>.bak; the new one goes through <path>.tmp and a rename. Both
 * get the original's permissions - config.env holds the wallet and the pool
 * password and is 0640, and a fresh file would get the umask's 0644. */
static int config_write_keys(const char *path, const char *const *keys, const char *const *vals,
                             int n, char *err, size_t en) {
    char tmp[1100], bak[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    int done[16] = {0};
    if (n > 16) n = 16;
#ifndef _WIN32
    struct stat st;
    mode_t mode = (stat(path, &st) == 0) ? (st.st_mode & 07777) : 0640;
#endif

    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fprintf(stderr, "[DagCore] config save: cannot write %s: %s\n", tmp, strerror(errno));
        snprintf(err, en, "cannot write next to config.env: %s", strerror(errno));
        if (in) fclose(in);
        return -1;
    }
    FILE *b = in ? fopen(bak, "w") : NULL;
#ifndef _WIN32
    /* Before a byte is written, so neither copy is ever readable wider. */
    chmod(tmp, mode);
    if (b) chmod(bak, mode);
#endif
    if (in && !b) {
        fprintf(stderr, "[DagCore] config save: cannot write %s: %s\n", bak, strerror(errno));
        snprintf(err, en, "cannot write the backup config.env.bak: %s", strerror(errno));
        fclose(in); fclose(out); remove(tmp);
        return -1;
    }
    if (in) {
        char line[1024];
        while (fgets(line, sizeof(line), in)) {
            fputs(line, b);
            int k = -1;
            if (line[0] != '#') {
                char *eq = strchr(line, '=');
                if (eq) {
                    size_t kl = (size_t)(eq - line);
                    for (int i = 0; i < n; i++)
                        if (strlen(keys[i]) == kl && strncmp(line, keys[i], kl) == 0) { k = i; break; }
                }
            }
            if (k < 0) { fputs(line, out); continue; }
            if (!done[k]) { fprintf(out, "%s=%s\n", keys[k], vals[k]); done[k] = 1; }
            /* a later duplicate of a key already written: dropped */
        }
        fclose(in);
        if (fclose(b) != 0) {
            snprintf(err, en, "cannot write the backup config.env.bak");
            fclose(out); remove(tmp);
            return -1;
        }
    }
    for (int i = 0; i < n; i++)
        if (!done[i]) fprintf(out, "%s=%s\n", keys[i], vals[i]);
    if (fclose(out) != 0) {
        snprintf(err, en, "cannot finish writing the new config.env");
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "[DagCore] config save: cannot replace %s: %s\n", path, strerror(errno));
        snprintf(err, en, "cannot replace config.env: %s", strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

static void api_config(int cfd, const char *body) {
    const char *keys[6];
    char vals[6][300];
    const char *valp[6];
    int n = 0, changed = 0;
    char s[300], msg[300];

    /* Refuse what the command line pins, before looking at anything else. */
    static const struct { const char *field; int bit; const char *flag; } pinned[] = {
        { "wallet", CLI_WALLET, "--wallet" }, { "pool", CLI_POOL, "--pool" },
        { "port", CLI_PORT, "--port" },       { "worker", CLI_WORKER, "--worker" },
        { "threads", CLI_THREADS, "--threads" }, { "gpu_device", CLI_GPU_DEVICE, "--gpu-device" },
    };
    for (size_t i = 0; i < sizeof(pinned) / sizeof(pinned[0]); i++) {
        if ((g_cli_set & pinned[i].bit) && json_value_of(body, pinned[i].field)) {
            snprintf(msg, sizeof(msg), "%s is set on the command line (%s), which overrides "
                     "config.env - change it there", pinned[i].field, pinned[i].flag);
            http_send_err(cfd, 409, "Conflict", msg);
            return;
        }
    }

    int r;
    if ((r = json_get_str(body, "wallet", s, sizeof(s))) != -1) {
        if (r != 0 || !cfg_valid_wallet(s)) {
            http_send_err(cfd, 400, "Bad Request",
                          "wallet must be 0x followed by 40 hex digits, and not the all-zero example");
            return;
        }
        keys[n] = "WALLET"; snprintf(vals[n], sizeof(vals[n]), "%s", s);
        if (strcmp(s, wallet) != 0) changed = 1;
        n++;
    }
    if ((r = json_get_str(body, "pool", s, sizeof(s))) != -1) {
        if (r != 0 || !cfg_valid_host(s)) {
            http_send_err(cfd, 400, "Bad Request",
                          "pool must be a host name or IP address, without stratum+tcp:// or a port");
            return;
        }
        /* A pool that does not resolve would leave the miner retrying forever
         * after the restart. Resolving blocks this (single-threaded) server
         * for as long as DNS takes, which is acceptable for a rare save. */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(s, NULL, &hints, &res) != 0) {
            snprintf(msg, sizeof(msg), "pool %s does not resolve - check the name", s);
            http_send_err(cfd, 400, "Bad Request", msg);
            return;
        }
        freeaddrinfo(res);
        keys[n] = "POOL"; snprintf(vals[n], sizeof(vals[n]), "%s", s);
        if (strcmp(s, pool_host) != 0) changed = 1;
        n++;
    }
    long v;
    if (json_value_of(body, "port")) {
        if (json_get_int(body, "port", &v) != 0 || v < 1 || v > 65535) {
            http_send_err(cfd, 400, "Bad Request", "port must be a number from 1 to 65535");
            return;
        }
        keys[n] = "PORT"; snprintf(vals[n], sizeof(vals[n]), "%ld", v);
        if (v != pool_port) changed = 1;
        n++;
    }
    if ((r = json_get_str(body, "worker", s, sizeof(s))) != -1) {
        if (r != 0 || !cfg_valid_worker(s)) {
            http_send_err(cfd, 400, "Bad Request",
                          "worker must be 1 to 63 letters, digits, dots, dashes or underscores");
            return;
        }
        keys[n] = "WORKER"; snprintf(vals[n], sizeof(vals[n]), "%s", s);
        if (strcmp(s, worker_name) != 0) changed = 1;
        n++;
    }
    if (json_value_of(body, "threads")) {
        if (json_get_int(body, "threads", &v) != 0 || v < -1 || v > g_cpu_cores) {
            snprintf(msg, sizeof(msg), "threads must be -1 (auto) or 0 to %d", g_cpu_cores);
            http_send_err(cfd, 400, "Bad Request", msg);
            return;
        }
        /* 0 leaves only the GPU miner; with no card mining it mines nothing. */
        if (v == 0 && g_num_gpus == 0) {
            http_send_err(cfd, 400, "Bad Request",
                          "threads 0 means GPU only, and no GPU is mining - use -1 (auto) or a number");
            return;
        }
        keys[n] = "THREADS"; snprintf(vals[n], sizeof(vals[n]), "%ld", v);
        if (v != g_threads_cfg) changed = 1;
        n++;
    }
    if ((r = json_get_str(body, "gpu_device", s, sizeof(s))) != -1) {
        char norm[64], err[200];
        if (r != 0) {
            http_send_err(cfd, 400, "Bad Request", "gpu_device is not valid");
            return;
        }
        if (cfg_parse_gpu_sel(s, norm, sizeof(norm), err, sizeof(err)) != 0) {
            http_send_err(cfd, 400, "Bad Request", err);
            return;
        }
        char cur[64];
        gpu_device_sel_str(cur, sizeof(cur));
        keys[n] = "GPU_DEVICE"; snprintf(vals[n], sizeof(vals[n]), "%s", norm);
        if (strcmp(norm, cur) != 0) changed = 1;
        n++;
    }
    if (n == 0) {
        http_send_err(cfd, 400, "Bad Request",
                      "nothing to save: send wallet, pool, port, worker, threads or gpu_device");
        return;
    }
    if (!changed) {
        http_send_json(cfd, 200, "OK", "{\"ok\":true,\"saved\":false,\"changed\":false}");
        return;
    }

    for (int i = 0; i < n; i++) valp[i] = vals[i];
    char err[300];
    if (config_write_keys(g_config_path, keys, valp, n, err, sizeof(err)) != 0) {
        http_send_err(cfd, 500, "Internal Server Error", err);
        return;
    }

    int supervised = (getenv("INVOCATION_ID") != NULL);
    char resp[240];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"saved\":true,\"changed\":true,\"restarting\":%s%s}",
             supervised ? "true" : "false",
             supervised ? "" : ",\"note\":\"miner is not supervised; restart it to apply\"");
    http_send_json(cfd, 200, "OK", resp);
    printf("[DagCore] Configuration saved to %s (%d setting%s; previous version in %s.bak)\n",
           g_config_path, n, n == 1 ? "" : "s", g_config_path);
    if (supervised) {
        printf("[DagCore] Exiting for restart to apply it\n");
        keep_alive = 0;
        running = 0;
    } else {
        printf("[DagCore] Restart the miner to apply it (not supervised)\n");
    }
}

static void dagtech_handle_control(int cfd, const char *req, const char *body) {
    char tok[128] = "";
    if (!g_api_token[0] ||
        http_header(req, "x-dagcore-token", tok, sizeof(tok)) != 0 ||
        !token_equal(g_api_token, tok)) {
        http_send_err(cfd, 401, "Unauthorized", "missing or invalid token");
        return;
    }
    /* Configuration does not depend on the GPU tuning controls being usable:
     * it works on a CPU-only rig and with nvidia-smi missing. */
    if (strncmp(req, "POST /api/config ", 17) == 0) {
        api_config(cfd, body);
        return;
    }
    if (!g_control_ok) {
        char msg[200];
        snprintf(msg, sizeof(msg), "controls unavailable: %s", g_control_reason);
        http_send_err(cfd, 409, "Conflict", msg);
        return;
    }

    if (strncmp(req, "POST /api/power-limit", 21) == 0) {
        /* Re-query rather than trust the startup snapshot: a driver reload or
         * a VBIOS change can move the envelope while the miner is running. */
        double mn, mx, df, cur;
        if (nvsmi_query_power(gpu_device, &mn, &mx, &df, &cur) != 0) {
            http_send_err(cfd, 500, "Internal Server Error", "nvidia-smi query failed");
            return;
        }
        g_pl_min = mn; g_pl_max = mx; g_pl_default = df; g_pl_current = cur;

        /* "Default" means the card's own figure - 320 W on an RTX 3080 - not
         * zero. Zero is not a power limit, and offering it as the way back
         * would be a trap. */
        long w;
        if (json_is_true(body, "reset")) {
            w = (long)df;
        } else if (json_get_int(body, "watts", &w) != 0) {
            http_send_err(cfd, 400, "Bad Request",
                          "body must contain \\\"watts\\\" or \\\"reset\\\":true");
            return;
        }
        if (w < (long)mn || w > (long)mx) {
            char msg[160];
            snprintf(msg, sizeof(msg), "watts must be between %.0f and %.0f", mn, mx);
            http_send_err(cfd, 400, "Bad Request", msg);
            return;
        }
        char err[200] = "";
        if (nvsmi_set_power_limit(gpu_device, (int)w, err, sizeof(err)) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "nvidia-smi: %s", err);
            fprintf(stderr, "[DagCore] Power limit %ld W rejected: %s\n", w, err);
            http_send_err(cfd, 500, "Internal Server Error", msg);
            return;
        }
        gpu_power_limit = (int)w;

        /* The power cap decides how often the card can actually hold a clock,
         * so changing it invalidates whatever a running trial had measured.
         * Restart them rather than credit the new cap with the old evidence. */
        int restarted = 0;
        for (int i = 0; i < TRIAL_N; i++) {
            if (!g_trial[i].active) continue;
            trial_start(i, g_trial[i].value, g_trial[i].prev);
            restarted++;
        }
        if (restarted)
            printf("[DagCore] Power limit changed - %d running test%s restarted\n",
                   restarted, restarted == 1 ? "" : "s");

        char val[16];
        snprintf(val, sizeof(val), "%ld", w);
        int saved = (overrides_set("GPU_POWER_LIMIT", val) == 0);
        nvsmi_query_power(gpu_device, NULL, NULL, NULL, &g_pl_current);
        printf("[DagCore] Power limit set to %ld W via control API%s\n",
               w, saved ? "" : " (WARNING: not persisted)");
        char resp[160];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"applied\":%ld,\"saved\":%s,\"tests_restarted\":%d}",
                 w, saved ? "true" : "false", restarted);
        http_send_json(cfd, 200, "OK", resp);
        return;
    }

    if (strncmp(req, "POST /api/intensity", 19) == 0) {
        long v;
        if (json_get_int(body, "value", &v) != 0) {
            http_send_err(cfd, 400, "Bad Request", "body must contain \\\"value\\\"");
            return;
        }
        if (v < 0 || v > 100) {
            http_send_err(cfd, 400, "Bad Request", "value must be between 0 and 100");
            return;
        }
        char val[16];
        snprintf(val, sizeof(val), "%ld", v);
        if (overrides_set("GPU_INTENSITY", val) != 0) {
            http_send_err(cfd, 500, "Internal Server Error", "could not write overrides file");
            return;
        }
        /* Intensity is fixed when the GPU buffers are allocated, so it only
         * takes effect on a restart. systemd sets INVOCATION_ID; without it
         * nothing would bring the miner back, so we save and say so instead
         * of exiting into nowhere. */
        int supervised = (getenv("INVOCATION_ID") != NULL);
        char resp[240];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"saved\":true,\"restarting\":%s%s}",
                 supervised ? "true" : "false",
                 supervised ? "" : ",\"note\":\"miner is not supervised; restart it to apply\"");
        http_send_json(cfd, 200, "OK", resp);
        if (supervised) {
            printf("[DagCore] Intensity set to %ld via control API - exiting for restart\n", v);
            keep_alive = 0;
            running = 0;
        } else {
            printf("[DagCore] Intensity %ld saved; restart required (not supervised)\n", v);
        }
        return;
    }

    /* Clock endpoints. Both take {"mhz":N} to lock, or {"reset":true} to hand
     * the domain back to the driver. */
    {
        int which = -1;
        if (strncmp(req, "POST /api/core-clock", 20) == 0) which = 0;
        else if (strncmp(req, "POST /api/mem-clock", 19) == 0) which = 1;

        if (which >= 0) {
            /* Fresh bounds: an offset set a moment ago has already moved them. */
            int lo = -1, hi = -1;
            if (nvml_clock_bounds(which, &lo, &hi) == 0) {
                if (which) { g_mem_lo = lo; g_mem_hi = hi; g_mem_boost = hi; }
                else       { g_core_lo = lo; g_core_hi = hi; g_core_boost = hi; }
            }
            char err[200] = "";

            if (json_is_true(body, "reset")) {
                /* Record the intent before touching the card. "I do not want a
                 * lock here" is worth keeping even if NVML then refuses the
                 * unlock - otherwise a card that says there was nothing to
                 * release leaves the old value in overrides.env, and the next
                 * start puts the lock back. A reset is saved at once anyway:
                 * it is the safe direction and needs no trial. */
                if (which) gpu_mem_clock_base = 0; else gpu_core_clock_base = 0;
                g_trial[which].active = 0;
                snprintf(g_trial[which].status, sizeof(g_trial[which].status), "none");
                int saved = (overrides_set(TRIAL_KEY[which], "0") == 0);

                if (nvml_unlock_clock(which, err, sizeof(err)) != 0) {
                    fprintf(stderr, "[DagCore] %s unlock failed: %s\n",
                            which ? "Memory clock" : "Core clock", err);
                    char resp[400];
                    snprintf(resp, sizeof(resp),
                             "{\"ok\":false,\"saved\":%s,\"error\":\"NVML: %s%s\"}",
                             saved ? "true" : "false", err,
                             saved ? " - the saved config was cleared, so the lock "
                                     "will be gone after a restart"
                                   : " - and the saved config could not be written either");
                    http_send_json(cfd, 500, "Internal Server Error", resp);
                    return;
                }
                printf("[DagCore] %s reset to driver default via control API%s\n",
                       which ? "Memory clock" : "Core clock", saved ? "" : " (NOT saved)");
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "{\"ok\":true,\"reset\":true,\"saved\":%s}", saved ? "true" : "false");
                http_send_json(cfd, 200, "OK", resp);
                return;
            }

            long mhz;
            if (json_get_int(body, "mhz", &mhz) != 0) {
                http_send_err(cfd, 400, "Bad Request",
                              "body must contain \\\"mhz\\\" or \\\"reset\\\":true");
                return;
            }
            if (lo < 0 || hi < 0) {
                http_send_err(cfd, 409, "Conflict", "supported clock range is unknown");
                return;
            }
            if (mhz < lo || mhz > hi) {
                char msg[160];
                snprintf(msg, sizeof(msg), "mhz must be between %d and %d", lo, hi);
                http_send_err(cfd, 400, "Bad Request", msg);
                return;
            }
            /* The request names the frequency the card should run. What gets
             * stored is the step it corresponds to, so the lock keeps its
             * meaning when an offset later moves the table. */
            int shift = clk_shift(which);
            int base = clk_snap_base(which, (int)mhz - shift);
            int eff = base + shift;
            if (nvml_lock_clock(which, eff, err, sizeof(err)) != 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "NVML: %s", err);
                fprintf(stderr, "[DagCore] %s lock %d MHz (step %d) rejected: %s\n",
                        which ? "Memory clock" : "Core clock", eff, base, err);
                http_send_err(cfd, 500, "Internal Server Error", msg);
                return;
            }
            int prev_base = which ? gpu_mem_clock_base : gpu_core_clock_base;
            if (which) gpu_mem_clock_base = base; else gpu_core_clock_base = base;
            trial_start(which, base, prev_base);
            printf("[DagCore] %s locked to %d MHz (step %d %+d) - on trial for %d min\n",
                   which ? "Memory clock" : "Core clock", eff, base, shift, TRIAL_MS / 60000);
            char resp[220];
            snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"applied\":%d,\"base\":%d,\"shift\":%d,"
                     "\"trial\":true,\"trial_seconds\":%d}",
                     eff, base, shift, TRIAL_MS / 1000);
            http_send_json(cfd, 200, "OK", resp);
            return;
        }
    }

    /* Offset endpoints. {"mhz":N} to set (N may be negative), {"reset":true}
     * for offset 0. Same trial as the clock locks - an offset that is one step
     * too far shows up as rejected shares, not as a refusal. */
    {
        int which = -1, is_mem = 0, omin = 0, omax = 0;
        if (strncmp(req, "POST /api/core-offset", 21) == 0) { which = 2; is_mem = 0; }
        else if (strncmp(req, "POST /api/mem-offset", 20) == 0) { which = 3; is_mem = 1; }

        if (which >= 0) {
            char err[200] = "";
            /* The card states its own permitted offset range. */
            if (nvml_offset_bounds(is_mem, &omin, &omax) != 0) {
                http_send_err(cfd, 409, "Conflict", "NVML did not report an offset range");
                return;
            }
            long mhz;
            int reset = json_is_true(body, "reset");
            if (reset) mhz = 0;
            else if (json_get_int(body, "mhz", &mhz) != 0) {
                http_send_err(cfd, 400, "Bad Request",
                              "body must contain \\\"mhz\\\" or \\\"reset\\\":true");
                return;
            }
            if (mhz < omin || mhz > omax) {
                char msg[160];
                snprintf(msg, sizeof(msg), "mhz must be between %d and %d", omin, omax);
                http_send_err(cfd, 400, "Bad Request", msg);
                return;
            }
            if (nvml_write_offset(is_mem, (int)mhz, err, sizeof(err)) != 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "NVML: %s", err);
                fprintf(stderr, "[DagCore] %s offset %ld MHz rejected: %s\n",
                        is_mem ? "Memory" : "Core", mhz, err);
                http_send_err(cfd, 500, "Internal Server Error", msg);
                return;
            }
            /* Captured before the write: this is what a failed trial goes back to. */
            int prev_off = is_mem ? gpu_mem_offset : gpu_core_offset;
            if (prev_off == DT_OFF_UNSET) prev_off = 0;

            if (is_mem) gpu_mem_offset = (int)mhz; else gpu_core_offset = (int)mhz;

            /* The offset just moved the table. A lock stored as a step still
             * means the same step, so put it back where it belongs instead of
             * leaving it stranded at its old frequency - that mismatch is what
             * used to raise a warning on a row the operator never touched. */
            int relocked = 0, relock_eff = 0;
            {
                int base = is_mem ? gpu_mem_clock_base : gpu_core_clock_base;
                if (base > 0) {
                    char rerr[200] = "";
                    relock_eff = clk_effective(is_mem, base);
                    if (nvml_lock_clock(is_mem, relock_eff, rerr, sizeof(rerr)) == 0) {
                        relocked = 1;
                        /* Same step, different frequency - so whatever the
                         * clock trial had proved is about an operating point
                         * that no longer exists. Start it over rather than
                         * credit it with stability it did not earn. */
                        if (g_trial[is_mem].active)
                            trial_start(is_mem, base, g_trial[is_mem].prev);
                        printf("[DagCore] %s lock re-applied at %d MHz (step %d) after the "
                               "offset changed\n", is_mem ? "Memory clock" : "Core clock",
                               relock_eff, base);
                    } else {
                        fprintf(stderr, "[DagCore] WARNING: could not re-apply the %s lock "
                                "at %d MHz (step %d): %s\n",
                                is_mem ? "memory" : "core", relock_eff, base, rerr);
                    }
                }
            }

            if (reset) {
                g_trial[which].active = 0;
                snprintf(g_trial[which].status, sizeof(g_trial[which].status), "none");
                int saved = (overrides_set(TRIAL_KEY[which], "0") == 0);
                printf("[DagCore] %s offset reset to 0 via control API%s\n",
                       is_mem ? "Memory" : "Core", saved ? "" : " (NOT saved)");
                char resp[200];
                snprintf(resp, sizeof(resp),
                         "{\"ok\":true,\"reset\":true,\"saved\":%s,"
                         "\"relocked\":%s,\"lock_mhz\":%d}",
                         saved ? "true" : "false", relocked ? "true" : "false", relock_eff);
                http_send_json(cfd, 200, "OK", resp);
                return;
            }
            trial_start(which, (int)mhz, prev_off);
            printf("[DagCore] %s offset set to %+ld MHz - on trial for %d min\n",
                   is_mem ? "Memory" : "Core", mhz, TRIAL_MS / 60000);
            char resp[240];
            snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"applied\":%ld,\"trial\":true,\"trial_seconds\":%d,"
                     "\"relocked\":%s,\"lock_mhz\":%d}",
                     mhz, TRIAL_MS / 1000, relocked ? "true" : "false", relock_eff);
            http_send_json(cfd, 200, "OK", resp);
            return;
        }
    }

    /* Cancel a running trial and put the previous value back. The dashboard
     * offers this as "Cancel test" so a change can be undone without waiting
     * ten minutes for a verdict nobody wants. */
    if (strncmp(req, "POST /api/cancel-trial", 22) == 0) {
        static const char *NAMES[TRIAL_N] = {
            "core-clock", "mem-clock", "core-offset", "mem-offset"
        };
        int which = -1;
        for (int i = 0; i < TRIAL_N; i++) {
            char pat[64];
            snprintf(pat, sizeof(pat), "\"%s\"", NAMES[i]);
            if (strstr(body, pat)) { which = i; break; }
        }
        if (which < 0) {
            http_send_err(cfd, 400, "Bad Request",
                          "body must name one of core-clock, mem-clock, "
                          "core-offset, mem-offset");
            return;
        }
        if (!g_trial[which].active) {
            http_send_err(cfd, 409, "Conflict", "no test is running for that setting");
            return;
        }
        int back = g_trial[which].prev;
        g_trial[which].active = 0;
        snprintf(g_trial[which].status, sizeof(g_trial[which].status), "none");
        trial_revert(which);
        printf("[DagCore] Trial for %s cancelled; back to %d\n", TRIAL_KEY[which], back);
        char resp[160];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"cancelled\":true,\"reverted_to\":%d}", back);
        http_send_json(cfd, 200, "OK", resp);
        return;
    }

    /* Adopt what the card is already running.
     *
     * A rig is usually tuned before this miner sees it - on rig1 the memory was
     * locked to 9851 MHz with a +1200 offset by nvidia_oc. Re-entering those
     * numbers through the trial flow, to reach the state the card is already
     * in, is busywork.
     *
     * Offsets are adopted unconditionally: NVML returns the value that was
     * set, so there is nothing to interpret.
     *
     * Clocks are adopted only when the request names them. NVML has no getter
     * for locked clocks - Set/Reset{Gpu,Memory}LockedClocks exist, Get does
     * not, and the ApplicationsClocksSetting event bit does not cover them
     * (checked on rig1: memory locked to 9851, reason mask 0x4, bit 0x2
     * clear). An earlier version inferred a lock from a clock holding still
     * for 600 ms. Within three minutes of one session that was wrong in both
     * directions: a core locked to 1995 MHz read as "moving", and an unlocked
     * core read as "pinned" twice, re-adding a lock the operator had just
     * reset. The operator knows what they locked; this code cannot find out,
     * so it asks instead of guessing. */
    if (strncmp(req, "POST /api/adopt", 15) == 0) {
        int core_off = 0, mem_off = 0;
        if (nvml_read_offsets(&core_off, &mem_off) != 0) {
            http_send_err(cfd, 409, "Conflict", "NVML could not read the current offsets");
            return;
        }
        int want_core = json_is_true(body, "core");
        int want_mem  = json_is_true(body, "mem");

        int core_clk = 0, mem_clk = 0;
        if (want_core && nvml_current_clock(0, &core_clk) != 0) {
            http_send_err(cfd, 409, "Conflict", "NVML could not read the core clock");
            return;
        }
        if (want_mem && nvml_current_clock(1, &mem_clk) != 0) {
            http_send_err(cfd, 409, "Conflict", "NVML could not read the memory clock");
            return;
        }

        char val[16];
        int wrote = 0, failed = 0;
        snprintf(val, sizeof(val), "%d", core_off);
        if (overrides_set("GPU_CORE_OFFSET", val) == 0) { gpu_core_offset = core_off; wrote++; }
        else failed++;
        snprintf(val, sizeof(val), "%d", mem_off);
        if (overrides_set("GPU_MEM_OFFSET", val) == 0) { gpu_mem_offset = mem_off; wrote++; }
        else failed++;

        /* Stored as a step, like every other lock, so adopting now and changing
         * an offset later does not strand the value. */
        int core_base = want_core ? clk_snap_base(0, core_clk - clk_shift(0)) : 0;
        int mem_base  = want_mem  ? clk_snap_base(1, mem_clk  - clk_shift(1)) : 0;
        if (want_core) {
            snprintf(val, sizeof(val), "%d", core_base);
            if (overrides_set("GPU_CORE_CLOCK_BASE", val) == 0) { gpu_core_clock_base = core_base; wrote++; }
            else failed++;
        }
        if (want_mem) {
            snprintf(val, sizeof(val), "%d", mem_base);
            if (overrides_set("GPU_MEM_CLOCK_BASE", val) == 0) { gpu_mem_clock_base = mem_base; wrote++; }
            else failed++;
        }
        /* Whatever was just settled needs no probation. A domain that was not
         * adopted keeps whatever trial it had running. */
        if (want_core) { g_trial[0].active = 0; snprintf(g_trial[0].status, sizeof(g_trial[0].status), "none"); }
        if (want_mem)  { g_trial[1].active = 0; snprintf(g_trial[1].status, sizeof(g_trial[1].status), "none"); }
        g_trial[2].active = 0; snprintf(g_trial[2].status, sizeof(g_trial[2].status), "none");
        g_trial[3].active = 0; snprintf(g_trial[3].status, sizeof(g_trial[3].status), "none");

        if (failed) {
            http_send_err(cfd, 500, "Internal Server Error",
                          "could not write every key to the overrides file");
            return;
        }
        printf("[DagCore] Adopted: offsets core %+d / memory %+d; core clock %s; "
               "memory clock %s (%d key%s written)\n",
               core_off, mem_off,
               want_core ? "adopted" : "not requested",
               want_mem ? "adopted" : "not requested",
               wrote, wrote == 1 ? "" : "s");
        char resp[320];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"adopted\":true,\"keys\":%d,"
                 "\"core_offset\":%d,\"mem_offset\":%d,"
                 "\"core_clock\":%d,\"core_base\":%d,\"core_adopted\":%s,"
                 "\"mem_clock\":%d,\"mem_base\":%d,\"mem_adopted\":%s}",
                 wrote, core_off, mem_off,
                 core_clk, core_base, want_core ? "true" : "false",
                 mem_clk, mem_base, want_mem ? "true" : "false");
        http_send_json(cfd, 200, "OK", resp);
        return;
    }

    http_send_err(cfd, 404, "Not Found", "unknown endpoint");
}

/* Records the hashrates /metrics is showing, every HIST_INTERVAL_S. Its own
 * thread rather than the stats loop, because that loop only runs while the
 * pool is connected - the page sampled through disconnects too, and so does
 * this. The rates are read unlocked: the same benign race /metrics has. */
static window_stats_t stats_window(void);
static void *dagtech_history_thread(void *arg) {
    (void)arg;
    while (keep_alive) {
        sleep(HIST_INTERVAL_S);
        pthread_mutex_lock(&stats_mtx);
        uint64_t hashes    = total_hashes;
        double   work      = accepted_work;
        uint64_t submitted = total_submitted;
        pthread_mutex_unlock(&stats_mtx);
        pthread_mutex_lock(&submit_rate_mtx);
        uint64_t dropped = rate_limited_shares;
        pthread_mutex_unlock(&submit_rate_mtx);
        /* The effective hashrate the dashboard showed at this moment, so a
         * reload redraws its line too. Only once the window is full: a
         * shorter one right after a start swings too much to plot. Taken
         * before hist_mtx, which stats_window() locks itself. */
        window_stats_t win = stats_window();
        double effective = win.full ? win.eff : -1.0;
        pthread_mutex_lock(&hist_mtx);
        hist_sample_t *s = &hist_buf[hist_head];
        s->t     = (int64_t)time(NULL);
        s->total = current_hashrate;
        s->gpu   = gpu_hashrate;
        s->cpu   = cpu_hashrate;
        s->effective     = effective;
        s->hashes        = hashes;
        s->accepted_work = work;
        s->submitted     = submitted;
        s->dropped       = dropped;
        hist_head = (hist_head + 1) % HIST_LEN;
        if (hist_count < HIST_LEN) hist_count++;
        pthread_mutex_unlock(&hist_mtx);
    }
    return NULL;
}

/* Effective and raw hashrate, and the submitted and dropped shares, over the
 * last EFFECTIVE_WINDOW_S: the counters now minus the oldest history sample
 * still inside the window. Until the miner has run that long the window
 * starts at start_time, where every counter was 0. The two rates cover the
 * same span, so their ratio compares like with like - the 10s raw figure on
 * the dashboard would make it jump. The dropped count gets a window because
 * the cumulative one is mostly the first minutes after a start, while vardiff
 * still has the difficulty low; on its own it reads like a constant loss. */
static window_stats_t stats_window(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&stats_mtx);
    uint64_t hashes    = total_hashes;
    double   work      = accepted_work;
    uint64_t submitted = total_submitted;
    pthread_mutex_unlock(&stats_mtx);
    pthread_mutex_lock(&submit_rate_mtx);
    uint64_t dropped = rate_limited_shares;
    pthread_mutex_unlock(&submit_rate_mtx);

    int64_t  t0 = (int64_t)start_time;
    uint64_t h0 = 0, s0 = 0, d0 = 0;
    double   w0 = 0.0;
    /* Full = the window no longer reaches back to the start. Say so as a flag:
     * span_s cannot tell, because a full window begins at the oldest 5s
     * sample inside it and so spans 595-600s, seldom exactly 600. */
    int full = (int64_t)now - t0 > EFFECTIVE_WINDOW_S;
    if (full) {
        pthread_mutex_lock(&hist_mtx);
        int first = (hist_head - hist_count + HIST_LEN) % HIST_LEN;
        for (int i = 0; i < hist_count; i++) {
            const hist_sample_t *s = &hist_buf[(first + i) % HIST_LEN];
            if (s->t >= (int64_t)now - EFFECTIVE_WINDOW_S) {
                t0 = s->t; h0 = s->hashes; w0 = s->accepted_work;
                s0 = s->submitted; d0 = s->dropped;
                break;
            }
        }
        pthread_mutex_unlock(&hist_mtx);
    }

    window_stats_t w;
    w.span_s    = (long)((int64_t)now - t0);
    w.full      = full;
    w.eff       = w.span_s > 0 ? (work - w0) / w.span_s : 0.0;
    w.raw       = w.span_s > 0 ? (double)(hashes - h0) / w.span_s : 0.0;
    w.submitted = submitted - s0;
    w.dropped   = dropped - d0;
    return w;
}

/* GET /history: the buffer oldest first, plus the miner's clock ("now"), so
 * the page can place the samples even when its own clock disagrees. */
static void serve_history(int cfd) {
    /* ~115 bytes per sample in practice; 192 leaves room for huge values. */
    size_t cap = 128 + (size_t)HIST_LEN * 192;
    char *out = malloc(cap);
    if (!out) {
        http_send_err(cfd, 500, "Internal Server Error", "out of memory");
        return;
    }
    size_t n = (size_t)snprintf(out, cap, "{\"interval_s\":%d,\"now\":%lld,\"samples\":[",
                                HIST_INTERVAL_S, (long long)time(NULL));
    pthread_mutex_lock(&hist_mtx);
    int first = (hist_head - hist_count + HIST_LEN) % HIST_LEN;
    for (int i = 0; i < hist_count; i++) {
        const hist_sample_t *s = &hist_buf[(first + i) % HIST_LEN];
        char eff[32];
        if (s->effective < 0) snprintf(eff, sizeof(eff), "null");
        else                  snprintf(eff, sizeof(eff), "%.2f", s->effective);
        int w = snprintf(out + n, cap - n,
                         "%s{\"t\":%lld,\"hashrate\":%.2f,\"gpu_hashrate\":%.2f,\"cpu_hashrate\":%.2f,"
                         "\"effective_hashrate\":%s}",
                         i ? "," : "", (long long)s->t, s->total, s->gpu, s->cpu, eff);
        if (w < 0 || (size_t)w >= cap - n - 2) break;   /* keep room for "]}" */
        n += (size_t)w;
    }
    pthread_mutex_unlock(&hist_mtx);
    snprintf(out + n, cap - n, "]}");
    http_send_json(cfd, 200, "OK", out);
    free(out);
}

/* =========================================================================
 * Built-in Metrics Server (for Dashboard)
 * ========================================================================= */
static void *dagtech_metrics_thread(void *arg) {
    (void)arg;

    #ifdef _WIN32
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    #else
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    #endif
    if (srv < 0) {
        fprintf(stderr, "[DagCore] Metrics server failed to create socket\n");
        return NULL;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(metrics_bind);
    addr.sin_port = htons(metrics_port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[DagCore] Metrics bind failed on %s:%d\n", metrics_bind, metrics_port);
        close(srv);
        return NULL;
    }
    listen(srv, 5);
    printf("[DagCore] Metrics server on http://%s:%d/metrics\n", metrics_bind, metrics_port);
    if (strcmp(metrics_bind, "127.0.0.1") != 0)
        printf("[DagCore] NOTE: metrics are reachable beyond localhost "
               "(wallet and pool details are served unauthenticated)\n");

    while (keep_alive) {
        struct sockaddr_in client;
        #ifdef _WIN32
        int clen = sizeof(client);
        SOCKET cfd = accept(srv, (struct sockaddr *)&client, &clen);
        #else
        socklen_t clen = sizeof(client);
        int cfd = accept(srv, (struct sockaddr *)&client, &clen);
        #endif
        if (cfd < 0) continue;

        /* A stalled client must not wedge the whole (single-threaded) server. */
#ifndef _WIN32
        {
            struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
#endif
        /* Read the request. A GET arrives in one packet, but a POST body can
         * be split, so keep reading until Content-Length bytes follow the
         * header terminator. */
        char reqbuf[8192];
        size_t have = 0;
        const char *body = NULL;
        for (;;) {
            int n = (int)recv(cfd, reqbuf + have, sizeof(reqbuf) - 1 - have, 0);
            if (n <= 0) break;
            have += (size_t)n;
            reqbuf[have] = '\0';
            char *hdr_end = strstr(reqbuf, "\r\n\r\n");
            if (!hdr_end) {
                if (have >= sizeof(reqbuf) - 1) break;
                continue;                       /* headers still incomplete */
            }
            body = hdr_end + 4;
            char cl[32];
            long want = 0;
            if (http_header(reqbuf, "content-length", cl, sizeof(cl)) == 0) want = atol(cl);
            if ((long)(have - (size_t)(body - reqbuf)) >= want) break;
            if (have >= sizeof(reqbuf) - 1) break;
        }
        if (have == 0) { close(cfd); continue; }
        reqbuf[have] = '\0';
        if (!body) body = "";

        /* ---- Control API (#46): token-protected POST ---- */
        if (strncmp(reqbuf, "POST ", 5) == 0) {
            dagtech_handle_control(cfd, reqbuf, body);
            close(cfd);
            continue;
        }

        if (strncmp(reqbuf, "GET ", 4) == 0) {
            char path[256];
            http_path(reqbuf, path, sizeof(path));
            if (strcmp(path, "/history") == 0) {
                serve_history(cfd);
                close(cfd);
                continue;
            }
        }

        /* Dashboard files. Anything that is not /metrics falls back to the
         * dashboard itself, so a bookmark to any path still lands somewhere
         * useful; the named paths get their own file. */
        if (dashboard_dir[0] && strncmp(reqbuf, "GET ", 4) == 0 &&
            strstr(reqbuf, "GET /metrics") == NULL) {
            char path[256];
            http_path(reqbuf, path, sizeof(path));
            const char *name = "index.html", *ctype = "text/html", *extra = NULL;
            if (strcmp(path, "/help") == 0 || strcmp(path, "/help.html") == 0) {
                name = "help.html";
            } else if (strcmp(path, "/fonts.css") == 0) {
                name  = "fonts.css";
                ctype = "text/css";
                /* Both pages share it and it never changes between builds. */
                extra = "Cache-Control: max-age=86400\r\n";
            } else if (strcmp(path, "/logo.webp") == 0) {
                name  = "logo.webp";
                ctype = "image/webp";
                extra = "Cache-Control: max-age=86400\r\n";
            }
            serve_dashboard_file(cfd, name, ctype, extra);
            close(cfd);
            continue;
        }

        /* Build per-GPU hashrate array string (no lock — same benign race as gpu_hashrate) */
        char gpu_hr_arr[256] = "[";
#ifdef DAGTECH_GPU
        for (int _gi = 0; _gi < g_num_gpus; _gi++) {
            char _tmp[32];
            snprintf(_tmp, sizeof(_tmp), "%s%.2f", _gi ? "," : "", g_gpus[_gi].hashrate);
            strncat(gpu_hr_arr, _tmp, sizeof(gpu_hr_arr) - strlen(gpu_hr_arr) - 2);
        }
#endif
        if (g_num_gpus == 0) strncat(gpu_hr_arr, "0.0", sizeof(gpu_hr_arr) - strlen(gpu_hr_arr) - 2);
        strncat(gpu_hr_arr, "]", sizeof(gpu_hr_arr) - strlen(gpu_hr_arr) - 1);

        /* Read system temperatures / GPU telemetry */
        double cpu_temp = get_cpu_temp();
        double gpu_temp = -1.0;
        double gpu_usage = -1.0;
        double gpu_memory = -1.0;
        double gpu_power = -1.0;
        double gpu_core_cur = -1.0;
        double gpu_mem_cur = -1.0;
        get_gpu_stats(&gpu_temp, &gpu_usage, &gpu_memory, &gpu_power,
                      &gpu_core_cur, &gpu_mem_cur);

        /* Offsets come from NVML, which is opened lazily; a card or driver
         * without them simply reports offset_available:false and the page
         * leaves those rows out. */
        int off_core = 0, off_mem = 0, off_ok = 0;
        const char *off_why = "not queried";
        if (g_control_ok) {
            off_ok = (nvml_read_offsets(&off_core, &off_mem) == 0);
#ifndef _WIN32
            off_why = g_nvml.why;
#else
            off_why = "not supported on Windows";
#endif
        } else {
            off_why = g_control_reason;
        }

        /* An offset moves the table a lock was chosen from, but the card does
         * not rescale an existing lock. Spot the mismatch here so the page can
         * say so instead of showing two numbers that quietly disagree. */
        /* The card states its own permitted offset range; the page uses it for
         * the slider bounds instead of carrying constants of its own. */
        int off_core_lo = 0, off_core_hi = 0, off_mem_lo = 0, off_mem_hi = 0;
        if (off_ok) {
            nvml_offset_bounds(0, &off_core_lo, &off_core_hi);
            nvml_offset_bounds(1, &off_mem_lo, &off_mem_hi);
        }
        int mem_shift  = off_ok ? nvml_mem_shift() : 0;
        int core_shift = off_ok ? clk_shift(0) : 0;
        int lock_core_eff = off_ok ? clk_effective(0, gpu_core_clock_base) : 0;
        int lock_mem_eff  = off_ok ? clk_effective(1, gpu_mem_clock_base) : 0;

        /* Now that a lock follows its step through an offset change, the only
         * thing left to warn about is a step the card cannot produce at all -
         * an offset that pushed the effective frequency outside the range.
         * A stale-looking frequency is no longer possible by construction. */
        int lock_stale = 0;
        if (off_ok) {
            if (lock_core_eff > 0 && g_core_lo > 0 &&
                (lock_core_eff < g_core_lo || lock_core_eff > g_core_hi)) lock_stale = 1;
            if (lock_mem_eff > 0 && g_mem_lo > 0 &&
                (lock_mem_eff < g_mem_lo || lock_mem_eff > g_mem_hi)) lock_stale = 1;
        }

        /* Before stats_mtx: stats_window() takes it itself. */
        window_stats_t win = stats_window();

        /* For the configuration form: the detected cards by name, the
         * current selection, and the settings the command line pins. */
        char gpu_devs[MAX_GPUS * 320 + 8] = "[";
        for (int gi = 0; gi < g_gpu_detected; gi++) {
            char esc[260], item[320];
            json_escape(g_gpu_names[gi], esc, sizeof(esc));
            snprintf(item, sizeof(item), "%s{\"index\":%d,\"name\":\"%s\"}", gi ? "," : "", gi, esc);
            strncat(gpu_devs, item, sizeof(gpu_devs) - strlen(gpu_devs) - 2);
        }
        strcat(gpu_devs, "]");
        char gpu_sel[64];
        gpu_device_sel_str(gpu_sel, sizeof(gpu_sel));
        char cfg_path_esc[1100];
        json_escape(g_config_path, cfg_path_esc, sizeof(cfg_path_esc));
        char cli_set[128] = "[";
        {
            static const struct { int bit; const char *name; } k[] = {
                { CLI_WALLET, "wallet" }, { CLI_POOL, "pool" }, { CLI_PORT, "port" },
                { CLI_WORKER, "worker" }, { CLI_THREADS, "threads" },
                { CLI_GPU_DEVICE, "gpu_device" },
            };
            int first = 1;
            for (size_t ki = 0; ki < sizeof(k) / sizeof(k[0]); ki++) {
                if (!(g_cli_set & k[ki].bit)) continue;
                strcat(cli_set, first ? "\"" : ",\"");
                strcat(cli_set, k[ki].name);
                strcat(cli_set, "\"");
                first = 0;
            }
            strcat(cli_set, "]");
        }

        /* Build JSON metrics response */
        pthread_mutex_lock(&stats_mtx);
        time_t uptime = time(NULL) - start_time;
        char json[8192];
        snprintf(json, sizeof(json),
            "{"
            "\"version\":\"%s\","
            "\"pool\":\"%s:%d\","
            "\"wallet\":\"%.10s...%s\","
            "\"wallet_full\":\"%s\","
            "\"worker\":\"%s\","
            "\"cpu_name\":\"%s\","
            "\"cpu_cores\":%d,"
            "\"threads\":%d,"
            "\"hashrate\":%.2f,"
            "\"cpu_hashrate\":%.2f,"
            "\"gpu_hashrate\":%.2f,"
            "\"cpu_temp\":%.1f,"
            "\"gpu_temp\":%.1f,"
            "\"gpu_usage\":%.1f,"
            "\"gpu_memory\":%.1f,"
            "\"gpu_power\":%.2f,"
            "\"gpu_power_limit\":%.0f,"
            "\"gpu_power_min\":%.0f,"
            "\"gpu_power_max\":%.0f,"
            "\"gpu_power_default\":%.0f,"
            "\"gpu_intensity\":%d,"
            "\"gpu_core_clock\":%.0f,"
            "\"gpu_core_clock_min\":%d,"
            "\"gpu_core_clock_max\":%d,"
            "\"gpu_core_clock_boost\":%d,"
            "\"gpu_core_clock_lock\":%d,"
            "\"gpu_core_clock_base\":%d,"
            "\"gpu_mem_clock\":%.0f,"
            "\"gpu_mem_clock_min\":%d,"
            "\"gpu_mem_clock_max\":%d,"
            "\"gpu_mem_clock_boost\":%d,"
            "\"gpu_mem_clock_lock\":%d,"
            "\"gpu_mem_clock_base\":%d,"
            "\"trial_core_status\":\"%s\","
            "\"trial_core_value\":%d,"
            "\"trial_core_remaining\":%d,"
            "\"trial_core_prev\":%d,"
            "\"trial_core_rejected\":%d,"
            "\"trial_mem_status\":\"%s\","
            "\"trial_mem_value\":%d,"
            "\"trial_mem_remaining\":%d,"
            "\"trial_mem_prev\":%d,"
            "\"trial_mem_rejected\":%d,"
            "\"gpu_core_offset\":%d,"
            "\"gpu_core_offset_min\":%d,"
            "\"gpu_core_offset_max\":%d,"
            "\"gpu_mem_offset\":%d,"
            "\"gpu_mem_offset_min\":%d,"
            "\"gpu_mem_offset_max\":%d,"
            "\"offset_available\":%s,"
            "\"offset_reason\":\"%s\","
            "\"trial_coreoff_status\":\"%s\","
            "\"trial_coreoff_value\":%d,"
            "\"trial_coreoff_remaining\":%d,"
            "\"trial_coreoff_prev\":%d,"
            "\"trial_coreoff_rejected\":%d,"
            "\"trial_memoff_status\":\"%s\","
            "\"trial_memoff_value\":%d,"
            "\"trial_memoff_remaining\":%d,"
            "\"trial_memoff_prev\":%d,"
            "\"trial_memoff_rejected\":%d,"
            "\"gpu_mem_clock_shift\":%d,"
            "\"gpu_core_clock_shift\":%d,"
            "\"clock_lock_stale\":%s,"
            "\"control_available\":%s,"
            "\"control_reason\":\"%s\","
            "\"total_hashes\":%" DT_PRIu64 ","
            "\"submitted\":%" DT_PRIu64 ","
            "\"accepted\":%" DT_PRIu64 ","
            "\"rejected\":%" DT_PRIu64 ","
            "\"stale\":%" DT_PRIu64 ","
            "\"dropped\":%" DT_PRIu64 ","
            "\"cpu_submitted\":%" DT_PRIu64 ","
            "\"gpu_submitted\":%" DT_PRIu64 ","
            "\"cpu_accepted\":%" DT_PRIu64 ","
            "\"gpu_accepted\":%" DT_PRIu64 ","
            "\"cpu_rejected\":%" DT_PRIu64 ","
            "\"gpu_rejected\":%" DT_PRIu64 ","
            "\"cpu_stale\":%" DT_PRIu64 ","
            "\"gpu_stale\":%" DT_PRIu64 ","
            "\"difficulty\":%.8f,"
            "\"uptime\":%ld,"
            "\"job_id\":\"%s\","
            "\"gpu_enabled\":%d,"
            "\"gpu_count\":%d,"
            "\"gpu_hashrates\":%s,"
            "\"effective_hashrate\":%.2f,"
            "\"effective_raw_hashrate\":%.2f,"
            "\"effective_pct\":%.1f,"
            "\"effective_window_s\":%ld,"
            "\"submit_min_interval_ms\":%d,"
            "\"dropped_window\":%" DT_PRIu64 ","
            "\"submitted_window\":%" DT_PRIu64 ","
            "\"effective_window_full\":%s,"
            "\"gpu_devices\":%s,"
            "\"gpu_device_sel\":\"%s\","
            "\"threads_config\":%d,"
            "\"threads_auto\":%d,"
            "\"config_path\":\"%s\","
            "\"config_cli\":%s"
            "}",
            DAGTECH_VERSION, pool_host, pool_port,
            wallet, wallet + strlen(wallet) - 4,
            wallet,
            worker_name,
            g_cpu_brand, g_cpu_cores,
            num_threads, current_hashrate, cpu_hashrate, gpu_hashrate,
            cpu_temp, gpu_temp, gpu_usage, gpu_memory, gpu_power,
            g_pl_current, g_pl_min, g_pl_max, g_pl_default, gpu_intensity,
            gpu_core_cur, g_core_lo, g_core_hi, g_core_boost,
            lock_core_eff, gpu_core_clock_base,
            gpu_mem_cur,  g_mem_lo,  g_mem_hi,  g_mem_boost,
            lock_mem_eff, gpu_mem_clock_base,
            g_trial[0].status, g_trial[0].value, trial_remaining_s(0),
            g_trial[0].prev, trial_rejects(0),
            g_trial[1].status, g_trial[1].value, trial_remaining_s(1),
            g_trial[1].prev, trial_rejects(1),
            off_core, off_core_lo, off_core_hi,
            off_mem, off_mem_lo, off_mem_hi,
            off_ok ? "true" : "false", off_why,
            g_trial[2].status, g_trial[2].value, trial_remaining_s(2),
            g_trial[2].prev, trial_rejects(2),
            g_trial[3].status, g_trial[3].value, trial_remaining_s(3),
            g_trial[3].prev, trial_rejects(3),
            mem_shift, core_shift, lock_stale ? "true" : "false",
            g_control_ok ? "true" : "false", g_control_reason,
            (unsigned long long)total_hashes,
            (unsigned long long)total_submitted,
            (unsigned long long)total_accepted,
            (unsigned long long)total_rejected,
            (unsigned long long)total_stale,
            (unsigned long long)rate_limited_shares,
            (unsigned long long)cpu_submitted,
            (unsigned long long)gpu_submitted,
            (unsigned long long)cpu_accepted,
            (unsigned long long)gpu_accepted,
            (unsigned long long)cpu_rejected,
            (unsigned long long)gpu_rejected,
            (unsigned long long)cpu_stale,
            (unsigned long long)gpu_stale,
            current_difficulty, (long)uptime,
            current_job.job_id,
            (gpu_enabled == 1) ? 1 : 0,
            g_num_gpus,
            gpu_hr_arr,
            win.eff, win.raw, win.raw > 0 ? 100.0 * win.eff / win.raw : 0.0, win.span_s,
            submit_min_interval_ms,
            (unsigned long long)win.dropped,
            (unsigned long long)win.submitted,
            win.full ? "true" : "false",
            gpu_devs, gpu_sel, g_threads_cfg, g_threads_auto, cfg_path_esc, cli_set);
        pthread_mutex_unlock(&stats_mtx);

        /* Same headers as before; the body no longer has to fit a fixed
         * 4 KB response buffer, which the card list could have overflowed. */
        http_send_json(cfd, 200, "OK", json);
        close(cfd);
    }

    close(srv);
    return NULL;
}

/* =========================================================================
 * Signal Handler
 * ========================================================================= */
/* Unblock the receive thread before joining it. That thread sits in recv()
 * on the pool socket, and a pool that stays connected but silent never makes
 * it return - so pthread_join() would wait for as long as the pool stays
 * quiet. In production the job stream hides this; a frozen pool would hang
 * shutdown until systemd's TimeoutStopSec fires SIGKILL.
 * shutdown() makes the pending recv() return 0 at once; close() follows. */
static void dagtech_unblock_pool_socket(void) {
    if (sockfd < 0) return;
#ifdef _WIN32
    shutdown(sockfd, SD_BOTH);
#else
    shutdown(sockfd, SHUT_RDWR);
#endif
}

static void dagtech_signal(int sig) {
    (void)sig;
    printf("\n[DagCore] Shutting down...\n");
    keep_alive = 0;
    running = 0;
}

/* =========================================================================
 * Usage / Help
 * ========================================================================= */
/* Options that take a separate value. Used only so that a flag given as the
 * last word on the command line reports "requires a value" instead of the
 * misleading "unknown option" - the parser's own tests are guarded by
 * "&& i + 1 < argc", so such a flag falls through to the unknown branch.
 *
 * KEEP IN SYNC: every new option that takes a value must be listed here too.
 * Forgetting it costs nothing until someone passes that option last, and then
 * the error blames the option instead of the missing value. */
static const char *DT_VALUE_OPTS[] = {
    "--wallet", "--pool", "--port", "--worker", "--password", "--threads",
    "--submit-margin", "--cpu-limit", "--metrics-port", "--metrics-bind",
    "--dashboard-dir", "--gpu-intensity", "--gpu-align", "--gpu-throttle",
    "--gpu-platform", "--gpu-device", "--config", NULL
};

/* An argument the parser did not recognise. Mining with a silently ignored
 * flag is worse than not starting: the rig looks healthy while running a
 * configuration nobody asked for, and a typo in a systemd ExecStart line can
 * sit there for weeks. */
static void dagtech_reject_arg(const char *arg) {
    const char **o;
    for (o = DT_VALUE_OPTS; *o; o++) {
        if (strcmp(arg, *o) == 0) {
            fprintf(stderr, "[DagCore] ERROR: option %s requires a value.\n", arg);
            fprintf(stderr, "          Run 'dagcore-miner --help' for the full list.\n");
            exit(1);
        }
    }
    fprintf(stderr, "[DagCore] ERROR: unknown option \"%s\".\n", arg);
    fprintf(stderr, "          Run 'dagcore-miner --help' for the full list.\n");
    exit(1);
}

static void dagtech_usage(void) {
    printf("\n");
    printf("  %s\n", DAGTECH_BANNER);
    printf("  %s\n\n", DAGTECH_AUTHOR);
    printf("  Usage: dagcore-miner [options]\n\n");
    printf("  Options:\n");
    printf("    --wallet <addr>        Your wallet address (REQUIRED)\n");
    printf("    --pool <host>          Pool hostname (default: %s)\n", DAGTECH_DEFAULT_POOL);
    printf("    --port <n>             Pool port (default: %d)\n", DAGTECH_DEFAULT_PORT);
    printf("    --threads <n>          CPU mining threads: 0 = none, GPU-only (default),\n");
    printf("                             -1 = auto (half the logical cores), N = N threads\n");
    printf("    --worker <name>        Worker name (default: dagtech)\n");
    printf("    --password <pw>        Pool password (default: x)\n");
    printf("    --submit-margin <f>    Share threshold margin >=1.0 (default: 1.0)\n");
    printf("    --no-auto-threshold    Don't auto-raise margin after low-difficulty rejects\n");
    printf("    --cpu-limit <n>        CPU usage limit percent per thread (1-100, default: 100)\n");
    printf("    --low-priority         Run at lowest CPU priority\n");
    printf("    --metrics-port <n>     Metrics HTTP port (default: %d)\n", metrics_port);
    printf("    --metrics-bind <ip>    Interface for metrics/dashboard (default: %s;\n", metrics_bind);
    printf("                             0.0.0.0 exposes them to the LAN)\n");
    printf("    --gpu                  Force enable GPU mining\n");
    printf("    --no-gpu               Disable GPU mining\n");
    printf("    --gpu-intensity <n|list>  GPU intensity per card: 80, 80,60 (default: 80)\n");
    printf("    --gpu-align <pow2|N>   Round GPU work-items down to a power of two\n");
    printf("                             (default) or to a multiple of N (N %% 32 == 0).\n");
    printf("                             pow2 measured fastest on RTX 3080: 1.614 MH/s\n");
    printf("                             vs 1.494 (align 256) and 1.472 (exact fit).\n");
    printf("    --gpu-throttle <n>     GPU duty-cycle limit percent (1-100, default: 100)\n");
    printf("    --gpu-platform <n>     OpenCL platform index (default: 0)\n");
    printf("    --gpu-device <n|n,m|all>  OpenCL device(s): 0, 0,1, all (default: 0)\n");
    printf("    --config <path>        Load config from file\n");
    printf("    --save-config          Save current settings to config file and exit\n");
    printf("    --help                 Show this help\n");
    printf("\n");
    printf("  Config file keys: WALLET, POOL, PORT, THREADS, WORKER, CPU_LIMIT,\n");
    printf("    METRICS_PORT, METRICS_BIND, GPU_ENABLED, GPU_INTENSITY, GPU_THROTTLE,\n");
    printf("    GPU_PLATFORM, GPU_DEVICE, GPU_ALIGN, GPU_POWER_LIMIT,\n");
    printf("    GPU_CORE_CLOCK_BASE, GPU_MEM_CLOCK_BASE, GPU_CORE_OFFSET,\n");
    printf("    GPU_MEM_OFFSET\n");
    printf("\n");
}

/* =========================================================================
 * Config Save / Load
 * ========================================================================= */
static int dagtech_file_exists(const char *p) {
    FILE *f = fopen(p, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Resolve the config.env path. Search order (first existing file wins):
 *   1. <exedir>/config.env          - next to the binary (e.g. install\bin\)
 *   2. <exedir>/../config.env       - install root, where the installer writes it
 *   3. ./config.env                 - current working directory
 *   4. $USERPROFILE/dagtech-gpu-miner/config.env  - legacy location (back-compat)
 * If none exist, returns (4) so any "not found" message points somewhere sane.
 * exe_path is typically argv[0]; pass NULL to skip the exe-relative candidates. */
static const char *dagtech_default_config_path(const char *exe_path) {
    static char path[1024];
    if (path[0]) return path;

    /* Derive the executable's directory (with trailing separator) from exe_path. */
    char dir[1024];
    char ps = '/';
    dir[0] = '\0';
    if (exe_path && exe_path[0]) {
        strncpy(dir, exe_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *sep = strrchr(dir, '/');
#ifdef _WIN32
        { char *sep2 = strrchr(dir, '\\'); if (sep2 > sep) sep = sep2; }
#endif
        if (sep) { ps = *sep; *(sep + 1) = '\0'; }  /* keep trailing separator */
        else dir[0] = '\0';
    }

    char cand[1024];

    /* 1. <exedir>/config.env */
    if (dir[0]) {
        snprintf(cand, sizeof(cand), "%sconfig.env", dir);
        if (dagtech_file_exists(cand)) {
            strncpy(path, cand, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            return path;
        }
    }

    /* 2. <exedir>/../config.env  (install root, e.g. C:\dagtech-gpu-miner\config.env) */
    if (dir[0]) {
        snprintf(cand, sizeof(cand), "%s..%cconfig.env", dir, ps);
        if (dagtech_file_exists(cand)) {
            strncpy(path, cand, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            return path;
        }
    }

    /* 3. ./config.env */
    if (dagtech_file_exists("config.env")) {
        strncpy(path, "config.env", sizeof(path) - 1);
        return path;
    }

    /* 4. legacy $USERPROFILE/dagtech-gpu-miner/config.env */
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEDRIVE");
#else
    home = getenv("HOME");
#endif
    if (home)
        snprintf(path, sizeof(path), "%s/dagtech-gpu-miner/config.env", home);
    else
        snprintf(path, sizeof(path), "dagtech-gpu-miner/config.env");

    return path;
}

/* Path separator test. On Windows both forms are accepted; on POSIX a
 * backslash is an ordinary filename character and must not split a path. */
#ifdef _WIN32
  #define DT_IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
  #define DT_IS_SEP(c) ((c) == '/')
#endif

static void dt_mkdir_one(const char *dir) {
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0700);
#endif
}

/* Creates the directory that will hold `filepath`, including any missing
 * intermediate components - the XDG cache default nests two levels deep
 * ($XDG_CACHE_HOME/dagcore-miner) and the parent is not guaranteed to exist.
 * Best-effort and silent: callers report the real error when the fopen() that
 * follows fails, which is the only failure the user can act on. */
static void dagtech_mkdir_parents(const char *filepath) {
    char tmp[512];
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *sep = NULL;
    for (char *c = tmp; *c; c++)
        if (DT_IS_SEP(*c)) sep = c;
    if (!sep) return;
    *sep = '\0';

    /* Skip the root so we never try to create "/" or "C:". */
    size_t start = 0;
    if (tmp[0] && DT_IS_SEP(tmp[0])) start = 1;
#ifdef _WIN32
    if (tmp[0] && tmp[1] == ':') start = DT_IS_SEP(tmp[2]) ? 3 : 2;
#endif

    for (size_t i = start; tmp[i]; i++) {
        if (!DT_IS_SEP(tmp[i])) continue;
        tmp[i] = '\0';
        dt_mkdir_one(tmp);
        tmp[i] = '/';
    }
    dt_mkdir_one(tmp);
}

/* Default autotune cache location.
 *   Windows: unchanged - the path the installer provisions.
 *   POSIX:   XDG basedir spec, $XDG_CACHE_HOME/dagcore-miner/autotune.json,
 *            falling back to $HOME/.cache/dagcore-miner/autotune.json. Without
 *            a usable HOME we land in the working directory so a service
 *            account with no home still starts instead of failing to write.
 * AUTOTUNE_CACHE (env var or config.env) overrides this; see main(). */
static void dagtech_default_autotune_cache(char *out, size_t out_size) {
#ifdef _WIN32
    snprintf(out, out_size, "C:\\dagtech-gpu-miner\\autotune.json");
#else
    const char *xdg  = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0])
        snprintf(out, out_size, "%s/dagcore-miner/autotune.json", xdg);
    else if (home && home[0])
        snprintf(out, out_size, "%s/.cache/dagcore-miner/autotune.json", home);
    else
        snprintf(out, out_size, "autotune.json");
#endif
}

static void dagtech_load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if      (strcmp(key, "WALLET")       == 0) strncpy(wallet,       val, sizeof(wallet)       - 1);
        else if (strcmp(key, "POOL")         == 0) strncpy(pool_host,    val, sizeof(pool_host)    - 1);
        else if (strcmp(key, "PORT")         == 0) pool_port     = atoi(val);
        else if (strcmp(key, "THREADS")      == 0) num_threads   = atoi(val);
        else if (strcmp(key, "WORKER")       == 0) strncpy(worker_name,  val, sizeof(worker_name)  - 1);
        else if (strcmp(key, "PASSWORD")     == 0) strncpy(password,     val, sizeof(password)     - 1);
        else if (strcmp(key, "SUBMIT_MARGIN")== 0) { submit_margin = atof(val); if (submit_margin < 1.0) submit_margin = 1.0; if (submit_margin > 8.0) submit_margin = 8.0; }
        else if (strcmp(key, "AUTO_THRESHOLD")==0) auto_threshold = atoi(val);
        else if (strcmp(key, "SUBMIT_MIN_INTERVAL_MS") == 0) {
            submit_min_interval_ms = atoi(val);
            if (submit_min_interval_ms < 0) submit_min_interval_ms = 0;
            if (submit_min_interval_ms > SUBMIT_MIN_INTERVAL_MS_MAX)
                submit_min_interval_ms = SUBMIT_MIN_INTERVAL_MS_MAX;
        }
        else if (strcmp(key, "LOW_PRIORITY") == 0) cpu_priority  = atoi(val);
        else if (strcmp(key, "CPU_LIMIT")    == 0) { cpu_limit = atoi(val); if (cpu_limit < 1) cpu_limit = 1; if (cpu_limit > 100) cpu_limit = 100; }
        else if (strcmp(key, "GPU_THROTTLE") == 0) { gpu_throttle = atoi(val); if (gpu_throttle < 1) gpu_throttle = 1; if (gpu_throttle > 100) gpu_throttle = 100; }
        else if (strcmp(key, "METRICS_PORT") == 0) metrics_port  = atoi(val);
        else if (strcmp(key, "METRICS_BIND") == 0) {
            if (metrics_parse_bind(val) != 0) metrics_bind_reject(val, "METRICS_BIND");
        }
        else if (strcmp(key, "DASHBOARD_DIR")== 0) strncpy(dashboard_dir,val, sizeof(dashboard_dir)- 1);
        else if (strcmp(key, "GPU_ENABLED")  == 0) gpu_enabled   = atoi(val);
        else if (strcmp(key, "GPU_POWER_LIMIT") == 0) gpu_power_limit = atoi(val);
        else if (strcmp(key, "GPU_CORE_CLOCK") == 0) gpu_core_clock = atoi(val);
        else if (strcmp(key, "GPU_MEM_CLOCK")  == 0) gpu_mem_clock  = atoi(val);
        else if (strcmp(key, "GPU_CORE_CLOCK_BASE") == 0) gpu_core_clock_base = atoi(val);
        else if (strcmp(key, "GPU_MEM_CLOCK_BASE")  == 0) gpu_mem_clock_base  = atoi(val);
        else if (strcmp(key, "GPU_CORE_OFFSET") == 0) gpu_core_offset = atoi(val);
        else if (strcmp(key, "GPU_MEM_OFFSET")  == 0) gpu_mem_offset  = atoi(val);
        else if (strcmp(key, "GPU_ALIGN")    == 0) {
            if (gpu_parse_align(val) != 0) gpu_align_reject(val, "GPU_ALIGN");
        }
        else if (strcmp(key, "GPU_INTENSITY")== 0) {
            if (strchr(val, ',')) {
                gpu_intensity_count = 0;
                char tmp[64]; strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
                char *tok = strtok(tmp, ",");
                while (tok && gpu_intensity_count < MAX_GPUS) {
                    int v = atoi(tok); if (v < 0) v = 0; if (v > 100) v = 100;
                    gpu_intensity_list[gpu_intensity_count++] = v; tok = strtok(NULL, ",");
                }
                if (gpu_intensity_count > 0) gpu_intensity = gpu_intensity_list[0];
            } else {
                gpu_intensity = atoi(val);
                if (gpu_intensity < 0) gpu_intensity = 0;
                if (gpu_intensity > 100) gpu_intensity = 100;
                gpu_intensity_count = 0;
            }
        }
        /* #42 autotune knobs */
        else if (strcmp(key, "AUTOTUNE")               == 0) gpu_autotune = atoi(val) ? 1 : 0;
        else if (strcmp(key, "AUTOTUNE_FORCE")         == 0) gpu_autotune_force = atoi(val) ? 1 : 0;
        else if (strcmp(key, "AUTOTUNE_TRIAL_SECONDS") == 0) {
            gpu_autotune_trial_seconds = atoi(val);
            if (gpu_autotune_trial_seconds < 5)   gpu_autotune_trial_seconds = 5;
            if (gpu_autotune_trial_seconds > 600) gpu_autotune_trial_seconds = 600;
        }
        else if (strcmp(key, "TARGET_BATCH_MS")        == 0) {
            gpu_target_batch_ms = atoi(val);
            if (gpu_target_batch_ms < 100)  gpu_target_batch_ms = 100;
            if (gpu_target_batch_ms > 5000) gpu_target_batch_ms = 5000;
        }
        else if (strcmp(key, "AUTOTUNE_BATCHES")       == 0) {
            strncpy(gpu_autotune_batches, val, sizeof(gpu_autotune_batches) - 1);
            gpu_autotune_batches[sizeof(gpu_autotune_batches) - 1] = '\0';
        }
        else if (strcmp(key, "AUTOTUNE_KERNEL_MODES")  == 0) {
            strncpy(gpu_autotune_modes, val, sizeof(gpu_autotune_modes) - 1);
            gpu_autotune_modes[sizeof(gpu_autotune_modes) - 1] = '\0';
        }
        else if (strcmp(key, "AUTOTUNE_CACHE")         == 0) {
            strncpy(gpu_autotune_cache, val, sizeof(gpu_autotune_cache) - 1);
            gpu_autotune_cache[sizeof(gpu_autotune_cache) - 1] = '\0';
        }
        else if (strcmp(key, "GPU_PLATFORM") == 0) gpu_platform  = atoi(val);
        else if (strcmp(key, "GPU_DEVICE")   == 0) {
            if (strcmp(val, "all") == 0) {
                gpu_use_all = 1;
            } else if (strchr(val, ',')) {
                gpu_device_count = 0; gpu_use_all = 0;
                char tmp[64]; strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
                char *tok = strtok(tmp, ",");
                while (tok && gpu_device_count < MAX_GPUS) {
                    gpu_device_list[gpu_device_count++] = atoi(tok);
                    tok = strtok(NULL, ",");
                }
            } else {
                gpu_device = atoi(val); gpu_device_count = 0; gpu_use_all = 0;
            }
        }
    }
    fclose(f);
    printf("[DagCore] Config loaded from %s\n", path);
}

static int dagtech_save_config(const char *path) {
    dagtech_mkdir_parents(path);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[DagCore] ERROR: Cannot write config to %s: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(f, "# DagTech GPU Miner configuration\n");
    fprintf(f, "# Generated by dagcore-miner --save-config\n");
    fprintf(f, "# Edit manually or re-run with --save-config to update.\n\n");

    fprintf(f, "WALLET=%s\n",        wallet);
    fprintf(f, "POOL=%s\n",          pool_host);
    fprintf(f, "PORT=%d\n",          pool_port);
    fprintf(f, "THREADS=%d\n",       num_threads);
    fprintf(f, "WORKER=%s\n",        worker_name);
    fprintf(f, "PASSWORD=%s\n",      password);
    fprintf(f, "SUBMIT_MARGIN=%.3f\n", submit_margin);
    fprintf(f, "AUTO_THRESHOLD=%d\n",  auto_threshold);
    fprintf(f, "SUBMIT_MIN_INTERVAL_MS=%d\n", submit_min_interval_ms);
    fprintf(f, "LOW_PRIORITY=%d\n",  cpu_priority);
    fprintf(f, "CPU_LIMIT=%d\n",     cpu_limit);
    fprintf(f, "GPU_THROTTLE=%d\n",  gpu_throttle);
    fprintf(f, "METRICS_PORT=%d\n",  metrics_port);
    fprintf(f, "METRICS_BIND=%s\n",  metrics_bind);
    fprintf(f, "GPU_ENABLED=%d\n",   gpu_enabled);
    if (gpu_align > 0) fprintf(f, "GPU_ALIGN=%d\n", gpu_align);
    else               fprintf(f, "GPU_ALIGN=pow2\n");
    if (gpu_intensity_count > 0) {
        fprintf(f, "GPU_INTENSITY=");
        for (int i = 0; i < gpu_intensity_count; i++)
            fprintf(f, "%s%d", i ? "," : "", gpu_intensity_list[i]);
        fprintf(f, "\n");
    } else {
        fprintf(f, "GPU_INTENSITY=%d\n", gpu_intensity);
    }
    fprintf(f, "GPU_PLATFORM=%d\n",  gpu_platform);
    if (gpu_use_all) {
        fprintf(f, "GPU_DEVICE=all\n");
    } else if (gpu_device_count > 0) {
        fprintf(f, "GPU_DEVICE=");
        for (int i = 0; i < gpu_device_count; i++)
            fprintf(f, "%s%d", i ? "," : "", gpu_device_list[i]);
        fprintf(f, "\n");
    } else {
        fprintf(f, "GPU_DEVICE=%d\n", gpu_device);
    }
    if (dashboard_dir[0])
        fprintf(f, "DASHBOARD_DIR=%s\n", dashboard_dir);

    fclose(f);
    printf("[DagCore] Config saved to %s\n", path);
    return 0;
}

/* =========================================================================
 * CPU detection (logical core count + brand string)
 * ========================================================================= */
static int dagtech_detect_cores(void) {
    int cores = 1;
    #ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cores = (int)si.dwNumberOfProcessors;
    #elif defined(__linux__)
    cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    #elif defined(__APPLE__)
    size_t len = sizeof(cores);
    sysctlbyname("hw.logicalcpu", &cores, &len, NULL, 0);
    #endif
    if (cores < 1) cores = 1;
    return cores;
}

/* Best-effort CPU brand string. Writes "Unknown CPU" if it can't be read. */
static void dagtech_cpu_brand(char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = '\0';
#if defined(__APPLE__)
    {
        size_t len = outsz;
        if (sysctlbyname("machdep.cpu.brand_string", out, &len, NULL, 0) == 0 && out[0])
            return;
        out[0] = '\0';
    }
#endif
#if defined(_MSC_VER)
    {
        int cpui[4];
        char brand[49];
        __cpuid(cpui, 0x80000000);
        if ((unsigned int)cpui[0] >= 0x80000004u) {
            for (int i = 0; i < 3; i++) { __cpuid(cpui, 0x80000002 + i); memcpy(brand + i*16, cpui, 16); }
            brand[48] = '\0';
            char *p = brand; while (*p == ' ') p++;
            size_t blen = strlen(p);
            while (blen > 0 && p[blen-1] == ' ') p[--blen] = '\0';
            snprintf(out, outsz, "%s", p);
            return;
        }
    }
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    {
        char brand[49];
        unsigned int regs[12];
        if (__get_cpuid_max(0x80000000u, NULL) >= 0x80000004u) {
            __get_cpuid(0x80000002u, &regs[0], &regs[1], &regs[2],  &regs[3]);
            __get_cpuid(0x80000003u, &regs[4], &regs[5], &regs[6],  &regs[7]);
            __get_cpuid(0x80000004u, &regs[8], &regs[9], &regs[10], &regs[11]);
            memcpy(brand, regs, 48);
            brand[48] = '\0';
            char *p = brand; while (*p == ' ') p++;
            size_t blen = strlen(p);
            while (blen > 0 && p[blen-1] == ' ') p[--blen] = '\0';
            snprintf(out, outsz, "%s", p);
            return;
        }
    }
#endif
    if (!out[0]) snprintf(out, outsz, "Unknown CPU");
}

/* =========================================================================
 * Auto-detect CPU thread count (half the logical cores, minimum 1)
 * ========================================================================= */
static int dagtech_detect_threads(void) {
    int threads = dagtech_detect_cores() / 2;
    if (threads < 1) threads = 1;
    return threads;
}

/* =========================================================================
 * Main Entry Point - DagTech GPU Miner
 * ========================================================================= */
int main(int argc, char **argv) {
    /* Flush log lines immediately — prevents output appearing in bursts when
       stdout is not a terminal (e.g. running as a background service). */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    signal(SIGINT, dagtech_signal);
    signal(SIGTERM, dagtech_signal);
#ifndef _WIN32
    /* A client that drops the connection mid-response (a closed browser tab,
     * a reset) makes send() raise SIGPIPE, which kills the process by
     * default. Ignored, send() returns EPIPE instead and the request is
     * simply abandoned. systemd already ignores it for the service; this
     * covers the miner run by hand. */
    signal(SIGPIPE, SIG_IGN);
#endif

    /* ---- Pass 1: look for --config <path> before loading defaults ---- */
    const char *config_path = dagtech_default_config_path(argv[0]);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
            break;
        }
    }
    g_config_path = config_path;

    /* ---- Load config file (CLI args below will override) ---- */
    dagtech_load_config(config_path);

    /* Dashboard-written overrides sit between the operator's config.env and
     * the command line: a setting changed from the browser wins over the file
     * an operator wrote by hand, but an explicit CLI flag still wins over both. */
    overrides_load();

    /* ---- #42 env-var overrides for autotune knobs (env wins over config.env)
       Lets ad-hoc tuning happen without editing config.env, mirroring the
       GPU_KERNEL_MODE env-var override pattern. ---- */
    {
        const char *e;
        if ((e = getenv("AUTOTUNE"))               != NULL) gpu_autotune = atoi(e) ? 1 : 0;
        if ((e = getenv("AUTOTUNE_FORCE"))         != NULL) gpu_autotune_force = atoi(e) ? 1 : 0;
        if ((e = getenv("AUTOTUNE_TRIAL_SECONDS")) != NULL) {
            int v = atoi(e);
            if (v >= 5 && v <= 600) gpu_autotune_trial_seconds = v;
        }
        if ((e = getenv("TARGET_BATCH_MS"))        != NULL) {
            int v = atoi(e);
            if (v >= 100 && v <= 5000) gpu_target_batch_ms = v;
        }
        if ((e = getenv("AUTOTUNE_BATCHES"))       != NULL) {
            strncpy(gpu_autotune_batches, e, sizeof(gpu_autotune_batches) - 1);
            gpu_autotune_batches[sizeof(gpu_autotune_batches) - 1] = '\0';
        }
        if ((e = getenv("AUTOTUNE_KERNEL_MODES"))  != NULL) {
            strncpy(gpu_autotune_modes, e, sizeof(gpu_autotune_modes) - 1);
            gpu_autotune_modes[sizeof(gpu_autotune_modes) - 1] = '\0';
        }
        if ((e = getenv("AUTOTUNE_CACHE"))         != NULL) {
            strncpy(gpu_autotune_cache, e, sizeof(gpu_autotune_cache) - 1);
            gpu_autotune_cache[sizeof(gpu_autotune_cache) - 1] = '\0';
        }
    }

    /* Platform default, only if neither config.env nor AUTOTUNE_CACHE set one. */
    if (gpu_autotune_cache[0] == '\0')
        dagtech_default_autotune_cache(gpu_autotune_cache, sizeof(gpu_autotune_cache));

    /* ---- Pass 2: full argument parsing ---- */
    int do_save_config = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wallet") == 0 && i + 1 < argc) {
            strncpy(wallet, argv[++i], sizeof(wallet) - 1);
            g_cli_set |= CLI_WALLET;
        }
        else if (strcmp(argv[i], "--pool") == 0 && i + 1 < argc) {
            strncpy(pool_host, argv[++i], sizeof(pool_host) - 1);
            g_cli_set |= CLI_POOL;
        }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            pool_port = atoi(argv[++i]);
            g_cli_set |= CLI_PORT;
        }
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
            g_cli_set |= CLI_THREADS;
        }
        else if (strcmp(argv[i], "--worker") == 0 && i + 1 < argc) {
            strncpy(worker_name, argv[++i], sizeof(worker_name) - 1);
            g_cli_set |= CLI_WORKER;
        }
        else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc)
            strncpy(password, argv[++i], sizeof(password) - 1);
        else if (strcmp(argv[i], "--submit-margin") == 0 && i + 1 < argc) {
            submit_margin = atof(argv[++i]);
            if (submit_margin < 1.0) submit_margin = 1.0;
            if (submit_margin > 8.0) submit_margin = 8.0;
        }
        else if (strcmp(argv[i], "--no-auto-threshold") == 0)
            auto_threshold = 0;
        else if (strcmp(argv[i], "--cpu-limit") == 0 && i + 1 < argc) {
            cpu_limit = atoi(argv[++i]);
            if (cpu_limit < 1)   cpu_limit = 1;
            if (cpu_limit > 100) cpu_limit = 100;
        }
        else if (strcmp(argv[i], "--low-priority") == 0)
            cpu_priority = 1;
        else if (strcmp(argv[i], "--metrics-port") == 0 && i + 1 < argc)
            metrics_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--metrics-bind") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (metrics_parse_bind(v) != 0) metrics_bind_reject(v, "--metrics-bind");
        }
        else if (strcmp(argv[i], "--dashboard-dir") == 0 && i + 1 < argc)
            strncpy(dashboard_dir, argv[++i], sizeof(dashboard_dir) - 1);
        else if (strcmp(argv[i], "--gpu") == 0)
            gpu_enabled = 1;
        else if (strcmp(argv[i], "--no-gpu") == 0)
            gpu_enabled = 0;
        else if (strcmp(argv[i], "--gpu-intensity") == 0 && i + 1 < argc) {
            const char *val = argv[++i];
            if (strchr(val, ',')) {
                gpu_intensity_count = 0;
                char tmp[64]; strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
                char *tok = strtok(tmp, ",");
                while (tok && gpu_intensity_count < MAX_GPUS) {
                    int v = atoi(tok); if (v < 0) v = 0; if (v > 100) v = 100;
                    gpu_intensity_list[gpu_intensity_count++] = v; tok = strtok(NULL, ",");
                }
                if (gpu_intensity_count > 0) gpu_intensity = gpu_intensity_list[0];
            } else {
                gpu_intensity = atoi(val);
                if (gpu_intensity < 0)   gpu_intensity = 0;
                if (gpu_intensity > 100) gpu_intensity = 100;
                gpu_intensity_count = 0;
            }
        }
        else if (strcmp(argv[i], "--gpu-align") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (gpu_parse_align(v) != 0) gpu_align_reject(v, "--gpu-align");
        }
        else if (strcmp(argv[i], "--gpu-throttle") == 0 && i + 1 < argc) {
            gpu_throttle = atoi(argv[++i]);
            if (gpu_throttle < 1)   gpu_throttle = 1;
            if (gpu_throttle > 100) gpu_throttle = 100;
        }
        else if (strcmp(argv[i], "--gpu-platform") == 0 && i + 1 < argc)
            gpu_platform = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gpu-device") == 0 && i + 1 < argc) {
            const char *val = argv[++i];
            g_cli_set |= CLI_GPU_DEVICE;
            if (strcmp(val, "all") == 0) {
                gpu_use_all = 1;
            } else if (strchr(val, ',')) {
                gpu_device_count = 0; gpu_use_all = 0;
                char tmp[64]; strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
                char *tok = strtok(tmp, ",");
                while (tok && gpu_device_count < MAX_GPUS) {
                    gpu_device_list[gpu_device_count++] = atoi(tok);
                    tok = strtok(NULL, ",");
                }
            } else {
                gpu_device = atoi(val); gpu_device_count = 0; gpu_use_all = 0;
            }
        }
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            i++;  /* already handled in pass 1 */
        else if (strcmp(argv[i], "--save-config") == 0)
            do_save_config = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            dagtech_usage();
            return 0;
        }
        else
            dagtech_reject_arg(argv[i]);
    }

    /* ---- Handle --save-config ---- */
    if (do_save_config) {
        printf("\n");
        printf("  ============================================\n");
        printf("  %s\n", DAGTECH_BANNER);
        printf("  ============================================\n\n");
        if (wallet[0] == 0) {
            fprintf(stderr, "[DagCore] ERROR: --wallet is required when saving config.\n");
            return 1;
        }
        return dagtech_save_config(config_path) == 0 ? 0 : 1;
    }

    /* Banner */
    printf("\n");
    printf("  ============================================\n");
    printf("  %s\n", DAGTECH_BANNER);
    printf("  %s\n", DAGTECH_AUTHOR);
    printf("  ============================================\n\n");

    /* Validate wallet */
    if (wallet[0] == 0) {
        fprintf(stderr, "[DagCore] ERROR: Wallet address is required!\n");
        dagtech_usage();
        return 1;
    }
    if (strncmp(wallet, "0x", 2) != 0 || strlen(wallet) != 42) {
        fprintf(stderr, "[DagCore] WARNING: Wallet format looks unusual (expected 0x + 40 hex chars)\n");
    }

    /* Detect CPU model + logical core count for display (matches installer). */
    g_cpu_cores = dagtech_detect_cores();
    dagtech_cpu_brand(g_cpu_brand, sizeof(g_cpu_brand));

    /* A negative --threads / THREADS asks for auto-detect (half the logical
     * cores). Zero is NOT auto-detect: it means "no CPU threads at all", which
     * is the default because a GPU rig gains little from the CPU miner and the
     * cores are better left to feeding the cards. */
    g_threads_cfg  = num_threads;
    g_threads_auto = dagtech_detect_threads();
    if (num_threads < 0)
        num_threads = g_threads_auto;

    /* Seed the adaptive margin from the configured base. */
    active_margin = submit_margin;

    /* Set low priority if requested */
    if (cpu_priority) {
        #ifdef _WIN32
        SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
        #else
        /* nice() returns the new value, so -1 is a legitimate success result.
         * POSIX: clear errno first and test it to tell the two apart. */
        errno = 0;
        if (nice(19) == -1 && errno != 0)
            fprintf(stderr, "[DagCore] WARNING: could not lower process priority: %s\n",
                    strerror(errno));
        #endif
        printf("[DagCore] Running at LOW CPU priority\n");
    }

    printf("[DagCore] CPU:     %s\n", g_cpu_brand);
    printf("[DagCore] Cores:   %d logical (CPU thread range 1-%d)\n", g_cpu_cores, g_cpu_cores);
    printf("[DagCore] Wallet:  %s\n", wallet);
    printf("[DagCore] Pool:    %s:%d\n", pool_host, pool_port);
    printf("[DagCore] Threads: %d (CPU)\n", num_threads);
    printf("[DagCore] Worker:  %s\n", worker_name);
    if (submit_min_interval_ms != SUBMIT_MIN_INTERVAL_MS_DEFAULT)
        printf("[DagCore] Share submit gap: %d ms%s\n", submit_min_interval_ms,
               submit_min_interval_ms == 0 ? " (no limit)" : "");

#ifdef DAGTECH_GPU
    /* List and initialize GPU */
    gpu_list_devices();

    int use_gpu = 0;
    if (gpu_enabled == 1) {
        use_gpu = 1;
    } else if (gpu_enabled == 0) {
        use_gpu = 0;
        printf("[DagCore GPU] GPU disabled by config/flag.\n");
    } else {
        /* auto: try to init GPU */
        use_gpu = 1;
        printf("[DagCore GPU] Auto-detecting GPU (use --no-gpu to disable)...\n");
    }

    if (use_gpu) {
        if (gpu_init_all(argv[0]) == 0) {
            gpu_enabled = 1;
            printf("[DagCore GPU] Intensity: %d | Platform: %d | GPUs active: %d\n",
                   gpu_intensity, gpu_platform, g_num_gpus);
        } else {
            fprintf(stderr, "[DagCore GPU] GPU init failed - running CPU only.\n");
            gpu_enabled = 0;
        }
    }
#else
    printf("[DagCore] Built without GPU support (no -DDAGTECH_GPU).\n");
    gpu_enabled = 0;
#endif

    printf("\n");

    start_time = time(NULL);

    /* Initialise Winsock before any socket call (metrics thread or pool connect) */
    #ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    #endif

    /* Control API: decide whether the dashboard's controls can work, and
     * re-apply a power limit saved by a previous session. */
    control_init();
    if (gpu_power_limit > 0) {
        if (!g_control_ok) {
            fprintf(stderr, "[DagCore] WARNING: GPU_POWER_LIMIT=%d ignored (%s)\n",
                    gpu_power_limit, g_control_reason);
        } else {
            char err[200] = "";
            if (nvsmi_set_power_limit(gpu_device, gpu_power_limit, err, sizeof(err)) != 0)
                fprintf(stderr, "[DagCore] WARNING: could not apply GPU_POWER_LIMIT=%d: %s\n",
                        gpu_power_limit, err);
            else {
                nvsmi_query_power(gpu_device, NULL, NULL, NULL, &g_pl_current);
                printf("[DagCore] GPU power limit set to %d W\n", gpu_power_limit);
            }
        }
    }

    /* Offsets first, then the locks. An offset moves the table of clocks a
     * lock may use, so locking before offsetting would validate - and clamp -
     * against the old table. */
    if (g_control_ok && (gpu_core_offset != DT_OFF_UNSET || gpu_mem_offset != DT_OFF_UNSET)) {
        char err[200] = "";
        if (gpu_core_offset != DT_OFF_UNSET) {
            if (nvml_write_offset(0, gpu_core_offset, err, sizeof(err)) != 0)
                fprintf(stderr, "[DagCore] WARNING: could not set core offset %+d MHz: %s\n",
                        gpu_core_offset, err);
            else
                printf("[DagCore] Core clock offset %+d MHz\n", gpu_core_offset);
        }
        if (gpu_mem_offset != DT_OFF_UNSET) {
            if (nvml_write_offset(1, gpu_mem_offset, err, sizeof(err)) != 0)
                fprintf(stderr, "[DagCore] WARNING: could not set memory offset %+d MHz: %s\n",
                        gpu_mem_offset, err);
            else
                printf("[DagCore] Memory clock offset %+d MHz\n", gpu_mem_offset);
        }
    }


    /* One-off migration from the retired absolute keys. The step is recovered
     * by subtracting the shift that was in force when the value was written -
     * which is the shift in force now, since offsets were applied a moment
     * ago. On rig1: GPU_MEM_CLOCK=9851 with a +1200 offset becomes
     * GPU_MEM_CLOCK_BASE=9251, an exact entry of the base table. */
    if (g_control_ok) {
        char valbuf[16];
        if (gpu_core_clock_base <= 0 && gpu_core_clock > 0) {
            gpu_core_clock_base = clk_snap_base(0, gpu_core_clock - clk_shift(0));
            snprintf(valbuf, sizeof(valbuf), "%d", gpu_core_clock_base);
            overrides_set("GPU_CORE_CLOCK_BASE", valbuf);
            overrides_set("GPU_CORE_CLOCK", "0");
            printf("[DagCore] Migrated GPU_CORE_CLOCK=%d to GPU_CORE_CLOCK_BASE=%d "
                   "(offset shift %+d)\n", gpu_core_clock, gpu_core_clock_base, clk_shift(0));
            gpu_core_clock = 0;
        }
        if (gpu_mem_clock_base <= 0 && gpu_mem_clock > 0) {
            gpu_mem_clock_base = clk_snap_base(1, gpu_mem_clock - clk_shift(1));
            snprintf(valbuf, sizeof(valbuf), "%d", gpu_mem_clock_base);
            overrides_set("GPU_MEM_CLOCK_BASE", valbuf);
            overrides_set("GPU_MEM_CLOCK", "0");
            printf("[DagCore] Migrated GPU_MEM_CLOCK=%d to GPU_MEM_CLOCK_BASE=%d "
                   "(offset shift %+d)\n", gpu_mem_clock, gpu_mem_clock_base, clk_shift(1));
            gpu_mem_clock = 0;
        }
    }

    /* Clock locks saved by a previous session (a trial that passed, or a value
     * the operator put in config.env), applied at the step they were saved as
     * and translated through the offset in force now. */
    if (g_control_ok && (gpu_core_clock_base > 0 || gpu_mem_clock_base > 0)) {
        char err[200] = "";
        if (gpu_core_clock_base > 0) {
            int eff = clk_effective(0, gpu_core_clock_base);
            if (nvml_lock_clock(0, eff, err, sizeof(err)) != 0)
                fprintf(stderr, "[DagCore] WARNING: could not lock core clock to %d MHz "
                        "(step %d): %s\n", eff, gpu_core_clock_base, err);
            else
                printf("[DagCore] Core clock locked to %d MHz (step %d %+d)\n",
                       eff, gpu_core_clock_base, clk_shift(0));
        }
        if (gpu_mem_clock_base > 0) {
            int eff = clk_effective(1, gpu_mem_clock_base);
            if (nvml_lock_clock(1, eff, err, sizeof(err)) != 0)
                fprintf(stderr, "[DagCore] WARNING: could not lock memory clock to %d MHz "
                        "(step %d): %s\n", eff, gpu_mem_clock_base, err);
            else
                printf("[DagCore] Memory clock locked to %d MHz (step %d %+d)\n",
                       eff, gpu_mem_clock_base, clk_shift(1));
        }
    } else if (!g_control_ok && (gpu_core_clock_base > 0 || gpu_mem_clock_base > 0)) {
        fprintf(stderr, "[DagCore] WARNING: clock locks ignored (%s)\n", g_control_reason);
    }

    /* Start metrics server thread */
    pthread_t metrics_tid;
    pthread_create(&metrics_tid, NULL, dagtech_metrics_thread, NULL);
    pthread_t history_tid;
    pthread_create(&history_tid, NULL, dagtech_history_thread, NULL);

    /* Reconnection loop */
    while (keep_alive) {
        running = 1;
        current_job.valid = 0;
        cpu_hashes_session = 0;
        gpu_hashes_session = 0;
#ifdef DAGTECH_GPU
        for (int _gi = 0; _gi < g_num_gpus; _gi++) {
            g_gpus[_gi].hashes_session = 0;
            g_gpus[_gi].hashrate       = 0.0;
        }
#endif

        printf("[DagCore] Connecting to pool %s:%d...\n", pool_host, pool_port);
        if (dagtech_connect_pool() < 0) {
            fprintf(stderr, "[DagCore] Cannot connect - retrying in 10s\n");
            sleep(10);
            continue;
        }
        printf("[DagCore] Connected!\n");
        dagtech_subscribe_authorize();

        /* Start receiver thread */
        pthread_t recv_tid;
        pthread_create(&recv_tid, NULL, dagtech_recv_thread, NULL);

        /* Wait for first job */
        printf("[DagCore] Waiting for work from pool...\n");
        for (int i = 0; i < 100 && running && !current_job.valid; i++)
            usleep(100000);

        if (!current_job.valid) {
            fprintf(stderr, "[DagCore] No job received - will retry in 10s\n");
            running = 0;
            dagtech_unblock_pool_socket();
            pthread_join(recv_tid, NULL);
            close(sockfd);
            if (keep_alive) sleep(10);
            continue;
        }

        /* Start CPU mining threads. num_threads == 0 is the default (GPU-only),
         * and malloc(0) may legitimately return NULL - so skip the allocation
         * entirely rather than rely on a particular libc's behaviour. */
        pthread_t *threads = NULL;
        int *tids = NULL;
        if (num_threads > 0) {
            threads = malloc(num_threads * sizeof(pthread_t));
            tids    = malloc(num_threads * sizeof(int));
            if (!threads || !tids) {
                fprintf(stderr, "[DagCore] ERROR: out of memory allocating %d CPU thread(s)\n",
                        num_threads);
                free(threads); free(tids);
                threads = NULL; tids = NULL;
                num_threads = 0;
            }
        }
        for (int i = 0; i < num_threads; i++) {
            tids[i] = i;
            pthread_create(&threads[i], NULL, dagtech_mine_thread, &tids[i]);
        }

        /* Start GPU threads — one per active device */
        pthread_t gpu_tids[MAX_GPUS];
        int gpu_threads_started = 0;
#ifdef DAGTECH_GPU
        if (gpu_enabled == 1) {
            for (int gi = 0; gi < g_num_gpus; gi++) {
                if (g_gpus[gi].ready) {
                    pthread_create(&gpu_tids[gpu_threads_started], NULL,
                                   dagtech_gpu_thread, &g_gpus[gi]);
                    gpu_threads_started++;
                }
            }
        }
#endif

        {
            char _gs[32] = "off";
#ifdef DAGTECH_GPU
            if (gpu_enabled == 1)
                snprintf(_gs, sizeof(_gs), "%d active", g_num_gpus);
#endif
            printf("[DagCore] Mining started! CPU workers: %d | GPU: %s\n\n",
                   num_threads, _gs);
        }

        /* Statistics reporting loop */
        time_t last_report = time(NULL);
        uint64_t last_total  = 0;
        uint64_t last_cpu_h  = 0;
        uint64_t last_gpu_h  = 0;
#ifdef DAGTECH_GPU
        uint64_t last_gpu_ind[MAX_GPUS];
        memset(last_gpu_ind, 0, sizeof(last_gpu_ind));
#endif
        /* Tick every second and report every ten. The nap used to be 10s.
         * That was never a problem for signals - SIGINT interrupts sleep() -
         * but `running` is also cleared from *other threads*: by the control
         * API when it exits for a restart, and by the receive thread when the
         * pool connection drops. Those set a flag and nothing wakes the
         * sleeper, so the loop only noticed after the full nap. Measured from
         * a dropped pool connection to the loop exiting: 9.7s before, 0.7s
         * now. The reporting cadence is unchanged; only the reaction time. */
        while (running) {
            sleep(1);
            if (!running) break;
            trial_tick();
            time_t now = time(NULL);
            double elapsed = difftime(now, last_report);
            if (elapsed >= 10) {
                pthread_mutex_lock(&stats_mtx);
                uint64_t h = total_hashes;
                pthread_mutex_unlock(&stats_mtx);

                pthread_mutex_lock(&cpu_stats_mtx);
                uint64_t ch = cpu_hashes_session;
                pthread_mutex_unlock(&cpu_stats_mtx);

                pthread_mutex_lock(&gpu_stats_mtx);
                uint64_t gh = gpu_hashes_session;
#ifdef DAGTECH_GPU
                uint64_t _ghi[MAX_GPUS];
                for (int _gi = 0; _gi < g_num_gpus; _gi++)
                    _ghi[_gi] = g_gpus[_gi].hashes_session;
#endif
                pthread_mutex_unlock(&gpu_stats_mtx);

                current_hashrate = (h  - last_total) / elapsed;
                cpu_hashrate     = (ch - last_cpu_h) / elapsed;
                gpu_hashrate     = (gh - last_gpu_h) / elapsed;
#ifdef DAGTECH_GPU
                for (int _gi = 0; _gi < g_num_gpus; _gi++) {
                    g_gpus[_gi].hashrate = (_ghi[_gi] - last_gpu_ind[_gi]) / elapsed;
                    last_gpu_ind[_gi]    = _ghi[_gi];
                }
#endif

                time_t uptime = now - start_time;
                int up_h = (int)(uptime / 3600);
                int up_m = (int)((uptime % 3600) / 60);

                if (gpu_enabled == 1) {
#ifdef DAGTECH_GPU
                    if (g_num_gpus > 1) {
                        /* Per-GPU breakdown when multiple cards active */
                        char _gd[512] = "";
                        for (int _gi = 0; _gi < g_num_gpus; _gi++) {
                            char _t[48];
                            snprintf(_t, sizeof(_t), " | GPU[%d]: %.2f H/s",
                                     _gi, g_gpus[_gi].hashrate);
                            strncat(_gd, _t, sizeof(_gd) - strlen(_gd) - 1);
                        }
                        printf("[DagCore] %.2f H/s | CPU: %.2f H/s%s | "
                               "Shares: %" DT_PRIu64 "/%" DT_PRIu64 "/%" DT_PRIu64 "/%" DT_PRIu64
                               " (sub/acc/rej/stale) | Uptime: %dh%dm\n",
                               current_hashrate, cpu_hashrate, _gd,
                               (unsigned long long)total_submitted,
                               (unsigned long long)total_accepted,
                               (unsigned long long)total_rejected,
                               (unsigned long long)total_stale,
                               up_h, up_m);
                    } else
#endif
                    {
                    printf("[DagCore] %.2f H/s | CPU: %.2f H/s | GPU: %.2f H/s | "
                           "Shares: %" DT_PRIu64 "/%" DT_PRIu64 "/%" DT_PRIu64 "/%" DT_PRIu64
                           " (sub/acc/rej/stale) | Uptime: %dh%dm\n",
                           current_hashrate, cpu_hashrate, gpu_hashrate,
                           (unsigned long long)total_submitted,
                           (unsigned long long)total_accepted,
                           (unsigned long long)total_rejected,
                           (unsigned long long)total_stale,
                           up_h, up_m);
                    }
                } else {
                    printf("[DagCore] %.1f H/s | "
                           "Shares: %" DT_PRIu64 "/%" DT_PRIu64 "/%" DT_PRIu64 "/%" DT_PRIu64
                           " (sub/acc/rej/stale) | Uptime: %dh%dm\n",
                           current_hashrate,
                           (unsigned long long)total_submitted,
                           (unsigned long long)total_accepted,
                           (unsigned long long)total_rejected,
                           (unsigned long long)total_stale,
                           up_h, up_m);
                }

                last_total = h;
                last_cpu_h = ch;
                last_gpu_h = gh;
                last_report = now;
            }
        }

        /* Clean up session */
        for (int i = 0; i < num_threads; i++)
            pthread_join(threads[i], NULL);
        for (int i = 0; i < gpu_threads_started; i++)
            pthread_join(gpu_tids[i], NULL);
        dagtech_unblock_pool_socket();
        pthread_join(recv_tid, NULL);
        free(threads);
        free(tids);
        close(sockfd);

        if (keep_alive) {
            printf("[DagCore] Reconnecting in 10s...\n");
            sleep(10);
        }
    }

#ifdef DAGTECH_GPU
    if (gpu_enabled == 1)
        gpu_cleanup();
#endif

    #ifdef _WIN32
    WSACleanup();
    #endif

    printf("[DagCore] Shutdown complete. Total hashes: %" DT_PRIu64 "\n",
           (unsigned long long)total_hashes);
    return 0;
}
