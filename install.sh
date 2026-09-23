#!/usr/bin/env bash
#
# DAGCore Miner installer.
#
# Written for someone who has a graphics card and a wallet address, and no
# reason to know what a compiler is. Every step says what it is about to do in
# plain words; anything that changes the system asks first, and --dry-run
# shows the whole run without touching anything.
#
#   ./install.sh                    interactive
#   ./install.sh --wallet 0x... --yes   unattended, for a fleet
#   ./install.sh --dry-run          show what would happen
#
set -euo pipefail

# ---------------------------------------------------------------- defaults --
PREFIX="/opt/dagcore-miner"
CONFDIR="/etc/dagcore-miner"
STATEDIR="/var/lib/dagcore-miner"
SERVICE="dagcore-miner"
UNIT="/etc/systemd/system/${SERVICE}.service"

DEF_POOL="stratum.dagcore.net"
DEF_PORT="3334"
DEF_THREADS="0"
MIN_DISK_MB=300

WALLET=""; POOL=""; PORT=""; THREADS=""; GPU_DEVICE=""
GPU_COUNT=0
LAN=""; ASSUME_YES=0; DRY_RUN=0; WANT_SERVICE=""; WANT_START=""
MODE=""                       # install | upgrade | reconfigure
CONFIG_PREEXISTED=0           # a config from BEFORE this run, not one make just made
PLACEHOLDER_WALLET="0x0000000000000000000000000000000000000000"

# ------------------------------------------------------------------ output --
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  B=$'\033[1m'; DIM=$'\033[2m'; R=$'\033[0m'
  GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; CYAN=$'\033[36m'
else
  B=""; DIM=""; R=""; GREEN=""; YELLOW=""; RED=""; CYAN=""
fi

step()  { printf '\n%s==>%s %s%s%s\n' "$CYAN" "$R" "$B" "$*" "$R"; }
say()   { printf '    %s\n' "$*"; }
ok()    { printf '    %s✓%s %s\n' "$GREEN" "$R" "$*"; }
# For lines that assert an action completed: silent during a dry run, where
# nothing completed.
did()   { [ "$DRY_RUN" = 1 ] || ok "$*"; }
warn()  { printf '    %s!%s  %s\n' "$YELLOW" "$R" "$*"; }
fail()  { printf '\n%sSomething went wrong:%s %s\n' "$RED" "$R" "$*" >&2; }
die()   { fail "$*"; printf '\nNothing further was changed.\n\n' >&2; exit 1; }

# Everything that changes the system goes through here, so --dry-run is honest.
run() {
  if [ "$DRY_RUN" = 1 ]; then
    printf '    %swould run:%s %s\n' "$DIM" "$R" "$*"
    return 0
  fi
  "$@"
}
# Same, for writing a file from stdin.
run_write() {
  local dest="$1"
  if [ "$DRY_RUN" = 1 ]; then
    printf '    %swould write:%s %s\n' "$DIM" "$R" "$dest"
    # Show the contents too - a dry run you cannot read is not much of a check.
    sed 's/^/      | /'
    return 0
  fi
  cat > "$dest"
}

usage() {
  cat <<USAGE
DAGCore Miner installer

  ./install.sh [options]

Options
  --wallet 0x...      wallet address (required when not asked interactively)
  --pool HOST         pool hostname            (default: $DEF_POOL)
  --port N            pool port                (default: $DEF_PORT)
  --threads N         CPU mining threads, 0 = GPU only   (default: $DEF_THREADS)
  --gpu-device WHICH  which graphics cards to use: all, a single number, or a
                      list like 0,1. Default: all of them when there is more
                      than one card, otherwise the only one.
  --lan               serve the dashboard to the local network, not just this
                      machine. Read the security note before using it.
  --no-service        do not create a systemd service
  --start             start mining at the end (implied by --yes)
  --yes               take every default and ask nothing
  --prefix DIR        install location         (default: $PREFIX)
  --dry-run           print what would happen; change nothing
  --help              this text
USAGE
}

# -------------------------------------------------------------- arguments ---
while [ $# -gt 0 ]; do
  case "$1" in
    --wallet)  WALLET="${2:-}"; shift 2 ;;
    --pool)    POOL="${2:-}";   shift 2 ;;
    --port)    PORT="${2:-}";   shift 2 ;;
    # The pool ignores worker names; still accepted so old install commands run.
    --worker)  warn "--worker is ignored: the pool has no worker names"; shift 2 ;;
    --threads) THREADS="${2:-}"; shift 2 ;;
    --gpu-device) GPU_DEVICE="${2:-}"; shift 2 ;;
    --prefix)  PREFIX="${2:-}"; shift 2 ;;
    --lan)     LAN=1; shift ;;
    --no-service) WANT_SERVICE=n; shift ;;
    --start)   WANT_START=y; shift ;;
    --yes|-y)  ASSUME_YES=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
done

# ----------------------------------------------------------------- asking ---
ask() {            # ask "question" "default" -> echoes the answer
  local q="$1" def="$2" ans=""
  if [ "$ASSUME_YES" = 1 ] || [ ! -t 0 ]; then printf '%s' "$def"; return; fi
  printf '    %s [%s]: ' "$q" "$def" >&2
  IFS= read -r ans || true
  printf '%s' "${ans:-$def}"
}

confirm() {        # confirm "question" "y|n" -> 0 for yes
  local q="$1" def="${2:-y}" ans=""
  if [ "$ASSUME_YES" = 1 ] || [ ! -t 0 ]; then [ "$def" = y ]; return; fi
  local hint="[Y/n]"; [ "$def" = y ] || hint="[y/N]"
  while :; do
    printf '    %s %s ' "$q" "$hint" >&2
    IFS= read -r ans || true
    ans="${ans:-$def}"
    case "$ans" in [Yy]*) return 0 ;; [Nn]*) return 1 ;; esac
    printf '    Please answer y or n.\n' >&2
  done
}

valid_wallet() { printf '%s' "$1" | grep -Eq '^0x[0-9a-fA-F]{40}$'; }

# ------------------------------------------------------------ preflight -----
need_root() {
  if [ "$(id -u)" != 0 ] && [ "$DRY_RUN" != 1 ]; then
    die "this installer needs to write to $PREFIX and $CONFDIR.
    Run it again with sudo:  sudo ./install.sh $*"
  fi
}

check_platform() {
  step "Checking this machine"
  [ "$(uname -s)" = "Linux" ] || die "this miner runs on Linux; found $(uname -s)."
  case "$(uname -m)" in
    x86_64|amd64) ok "Linux on x86-64" ;;
    *) die "this miner is built for 64-bit Intel/AMD machines; found $(uname -m)." ;;
  esac
}

# How many NVIDIA graphics cards are in this machine.
#
# nvidia-smi is the authority when it is there. Otherwise the kernel's PCI
# list works, with one catch: a modern card registers several functions under
# the NVIDIA vendor ID - the GPU, an audio device for HDMI, sometimes a USB
# controller - so counting by vendor alone counts one card as three. Class
# 0x03xx is the display controller, one per card.
count_gpus() {
  local n=0 d cls
  if command -v nvidia-smi >/dev/null 2>&1; then
    n="$(nvidia-smi -L 2>/dev/null | grep -c '^GPU ' || true)"
    if [ "${n:-0}" -gt 0 ]; then printf '%s' "$n"; return 0; fi
  fi
  n=0
  for d in /sys/bus/pci/devices/*; do
    [ -r "$d/vendor" ] && [ -r "$d/class" ] || continue
    [ "$(cat "$d/vendor")" = "0x10de" ] || continue
    cls="$(cat "$d/class")"
    case "$cls" in 0x03*) n=$((n + 1)) ;; esac
  done
  printf '%s' "$n"
}

check_card() {
  GPU_COUNT="$(count_gpus)"
  if [ "${GPU_COUNT:-0}" -lt 1 ]; then
    die "no NVIDIA graphics card found on this machine.
    This miner needs one. If the card is installed, check that it is seated
    properly and that the machine sees it."
  fi
  if [ "$GPU_COUNT" = 1 ]; then
    ok "1 NVIDIA graphics card detected"
  else
    ok "$GPU_COUNT NVIDIA graphics cards detected"
  fi
}

driver_hint() {
  case "$PKG" in
    apt)    echo "sudo apt update && sudo apt install nvidia-driver-570" ;;
    dnf)    echo "sudo dnf install akmod-nvidia  (needs the RPM Fusion repository)" ;;
    pacman) echo "sudo pacman -S nvidia nvidia-utils" ;;
    *)      echo "install the NVIDIA driver package for your distribution" ;;
  esac
}

check_driver() {
  if [ -r /proc/driver/nvidia/version ]; then
    local ver
    ver="$(grep -o '[0-9]\{3,\}\.[0-9.]*' /proc/driver/nvidia/version 2>/dev/null | head -1)"
    ok "NVIDIA driver loaded${ver:+ (version $ver)}"
    return
  fi
  die "the NVIDIA driver is not installed, or is not loaded.
    The card is there but the system cannot talk to it yet.

    Install the driver, reboot, then run this installer again:
      $(driver_hint)

    This installer will not install a graphics driver by itself: it needs a
    reboot and can leave a machine without a display if it goes wrong."
}

check_disk() {
  local target="$PREFIX" avail
  while [ ! -d "$target" ] && [ "$target" != "/" ]; do target="$(dirname "$target")"; done
  avail="$(df -Pm "$target" 2>/dev/null | awk 'NR==2 {print $4}')"
  if [ -z "$avail" ]; then warn "could not check free disk space; continuing"; return; fi
  if [ "$avail" -lt "$MIN_DISK_MB" ]; then
    die "not enough free space on $target: ${avail} MB free, about ${MIN_DISK_MB} MB needed."
  fi
  ok "disk space: ${avail} MB free"
}

detect_pkg() {
  if command -v apt-get >/dev/null 2>&1; then PKG=apt
  elif command -v dnf >/dev/null 2>&1; then PKG=dnf
  elif command -v pacman >/dev/null 2>&1; then PKG=pacman
  else PKG=""; fi
}

# --------------------------------------------------------- dependencies -----
have_cc()      { command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; }
have_make()    { command -v make >/dev/null 2>&1; }
have_cl_hdr()  { [ -r /usr/include/CL/cl.h ]; }
have_cl_lib()  { ldconfig -p 2>/dev/null | grep -q 'libOpenCL\.so'; }
have_cl_icd()  { ls /etc/OpenCL/vendors/*.icd >/dev/null 2>&1; }

# Where the NVIDIA OpenCL driver actually is. Prefer the soname, which the
# loader resolves, and fall back to a full path when it is not in the cache.
nvidia_ocl_lib() {
  local p
  p="$(ldconfig -p 2>/dev/null | awk '/libnvidia-opencl\.so\.1/ {print $NF; exit}')"
  if [ -n "$p" ] && [ -e "$p" ]; then printf 'libnvidia-opencl.so.1'; return 0; fi
  for p in /usr/lib/x86_64-linux-gnu/libnvidia-opencl.so.1 \
           /usr/lib64/libnvidia-opencl.so.1 \
           /usr/lib/libnvidia-opencl.so.1 \
           /usr/local/nvidia/lib64/libnvidia-opencl.so.1; do
    [ -e "$p" ] && { printf '%s' "$p"; return 0; }
  done
  return 1
}

pkgs_for() {
  case "$PKG" in
    apt)    echo "build-essential ocl-icd-opencl-dev opencl-headers" ;;
    dnf)    echo "gcc make ocl-icd-devel opencl-headers" ;;
    pacman) echo "base-devel ocl-icd opencl-headers" ;;
  esac
}

install_pkgs() {
  local list="$1"
  case "$PKG" in
    apt)    run apt-get update -qq && run apt-get install -y $list ;;
    dnf)    run dnf install -y $list ;;
    pacman) run pacman -Sy --needed --noconfirm $list ;;
  esac
}

check_deps() {
  step "Checking what is needed to build the miner"
  local missing=""
  have_cc   || missing="$missing compiler"
  have_make || missing="$missing make"
  have_cl_hdr || missing="$missing OpenCL-headers"
  have_cl_lib || missing="$missing OpenCL-library"

  if [ -z "$missing" ]; then
    ok "everything needed is already installed"
  else
    say "These are missing:$missing"
    if [ -z "$PKG" ]; then
      die "could not find apt, dnf or pacman, so the missing pieces cannot be
    installed automatically. Install the development tools and the OpenCL
    headers for your distribution, then run this installer again."
    fi
    local list; list="$(pkgs_for)"
    say ""
    say "They come from these packages:"
    say "  $list"
    if ! confirm "Install them now with $PKG?" y; then
      die "cannot continue without them. Install them yourself and run this again."
    fi
    step "Installing packages"
    install_pkgs "$list" || die "the package manager could not install them.
    Check the output above, fix the problem, and run this installer again."
    # Nothing was installed during a dry run, so checking for it would always
    # fail - and did, on every dry run before this was fixed.
    if [ "$DRY_RUN" != 1 ]; then
      have_cl_hdr || die "the OpenCL headers are still missing after installing.
    Your distribution may name that package differently."
      ok "packages installed"
    fi
  fi

  # An ICD file is how a program finds the NVIDIA OpenCL driver at runtime.
  # Without one the miner reports "No OpenCL platforms found", falls back to
  # CPU mining and produces nothing - the card is there and unused.
  if have_cl_icd; then
    ok "OpenCL driver registration present"
  else
    local ocl_lib=""
    ocl_lib="$(nvidia_ocl_lib || true)"
    if [ -n "$ocl_lib" ]; then
      warn "the NVIDIA OpenCL driver is installed but not registered."
      say  "  found:   $ocl_lib"
      say  "  missing: /etc/OpenCL/vendors/nvidia.icd"
      say  ""
      say  "This is the normal state inside a container - RunPod, Vast.ai and"
      say  "the CUDA images ship the library without the registration file."
      say  "Without it the miner cannot see the card and would mine on the CPU"
      say  "only, at effectively zero hashrate."
      say  ""
      if confirm "Create the registration file now?" y; then
        run mkdir -p /etc/OpenCL/vendors
        printf '%s\n' "$ocl_lib" | run_write /etc/OpenCL/vendors/nvidia.icd
        run chmod 0644 /etc/OpenCL/vendors/nvidia.icd
        if [ "$DRY_RUN" != 1 ]; then
          have_cl_icd && ok "registered - the miner can now find the card" \
                      || warn "the file was written but still is not being found"
        fi
      else
        warn "skipped. The miner will not use the graphics card."
        say  "Create it later with:"
        say  "  sudo mkdir -p /etc/OpenCL/vendors"
        say  "  echo $ocl_lib | sudo tee /etc/OpenCL/vendors/nvidia.icd"
      fi
    else
      warn "no OpenCL driver registration and no NVIDIA OpenCL library found."
      say  "The miner will build but will not see the card. The library comes"
      say  "with the driver; on Debian and Ubuntu the package is"
      say  "nvidia-opencl-icd."
    fi
  fi

  if command -v nvidia-smi >/dev/null 2>&1; then
    ok "nvidia-smi present - power limit and clock controls will work"
  else
    warn "nvidia-smi was not found."
    say  "Mining will work. These dashboard features will not:"
    say  "  - power limit"
    say  "  - core and memory clock locks"
    say  "  - clock offsets"
    say  "It usually comes with the driver package as nvidia-utils."
  fi
}

# --------------------------------------------------------------- build ------
build_and_install() {
  step "Building the miner"
  [ -f Makefile ] || die "this script must be run from the folder it came in;
    Makefile is not here. cd into the DAGCore folder and try again."
  if [ "$DRY_RUN" = 1 ]; then
    printf '    %swould run:%s make clean && make\n' "$DIM" "$R"
  else
    make clean >/dev/null 2>&1 || true
    if ! make >/tmp/dagcore-build.log 2>&1; then
      tail -n 20 /tmp/dagcore-build.log >&2
      die "the miner did not build. The last lines of the build log are above,
    and the whole log is in /tmp/dagcore-build.log."
    fi
    ok "built"
  fi

  step "Installing into $PREFIX"
  if [ "$DRY_RUN" = 1 ]; then
    printf '    %swould run:%s make install PREFIX=%s SYSCONFDIR=%s\n' \
      "$DIM" "$R" "$PREFIX" "$CONFDIR"
  else
    make install PREFIX="$PREFIX" SYSCONFDIR="$CONFDIR" >/dev/null 2>&1 || \
      die "could not copy the files into $PREFIX.
    Check that there is space and that you ran this with sudo."
    ok "miner, GPU kernel and dashboard installed"
  fi
}

# The last gate before declaring success: whatever ends up on disk has to be
# a wallet someone can be paid at. An installer that finishes cheerfully while
# the rig mines to 0x0000...0000 is worse than one that fails, because nothing
# looks wrong for hours.
verify_wallet_configured() {
  [ "$DRY_RUN" = 1 ] && return 0
  step "Checking the wallet the miner will use"

  local ondisk
  ondisk="$(sed -n 's/^WALLET=//p' "$CONFDIR/config.env" 2>/dev/null | head -1)"

  if [ -z "$ondisk" ]; then
    die "no wallet address in $CONFDIR/config.env.
    Nothing would be paid out. Run this installer again and give an address."
  fi

  if [ "$ondisk" = "$PLACEHOLDER_WALLET" ]; then
    warn "the configuration still holds the example wallet address."
    say  "Mining with it earns nothing at all - it is a placeholder."
    if [ -n "$WALLET" ] && valid_wallet "$WALLET"; then
      say ""
      say "The address you gave this run is $WALLET"
      if confirm "Write it into the configuration now?" y; then
        run sed -i "s|^WALLET=.*|WALLET=$WALLET|" "$CONFDIR/config.env"
        ondisk="$(sed -n 's/^WALLET=//p' "$CONFDIR/config.env" 2>/dev/null | head -1)"
        [ "$ondisk" = "$WALLET" ] || die "could not write the address into
    $CONFDIR/config.env. Edit it by hand and set WALLET= to your address."
        ok "wallet corrected"
      else
        die "stopped. Edit $CONFDIR/config.env, set WALLET= to your address,
    then start the miner."
      fi
    else
      die "edit $CONFDIR/config.env and set WALLET= to your own address,
    then start the miner. Nothing was started."
    fi
  fi

  valid_wallet "$ondisk" || die "the wallet in $CONFDIR/config.env does not look
    like an address: $ondisk
    It must be 0x followed by 40 hexadecimal characters."

  # When a wallet was given or asked for this run, that is the one that must
  # have landed. A mismatch means something overwrote it.
  if [ -n "$WALLET" ] && [ "$ondisk" != "$WALLET" ] && [ "$MODE" != upgrade ]; then
    die "the configuration ended up with a different wallet than the one given.
    on disk: $ondisk
    given:   $WALLET
    Edit $CONFDIR/config.env by hand before starting the miner."
  fi

  ok "mining to $ondisk"
}

# The install can succeed and still leave a rig earning nothing: with no ICD
# the miner starts, finds no OpenCL platform, and mines on the CPU at
# effectively zero. So ask the miner itself, before declaring victory. It lists
# its GPUs at startup, before it talks to a pool, so pointing it at a dead
# address is enough and no pool ever sees this.
verify_gpu_visible() {
  [ "$DRY_RUN" = 1 ] && return 0
  command -v timeout >/dev/null 2>&1 || return 0
  [ -x "$PREFIX/bin/dagcore-miner" ] || return 0

  step "Checking that the miner can see the card"
  local out=""
  out="$(timeout 25 "$PREFIX/bin/dagcore-miner" --config "$CONFDIR/config.env" \
          --pool 127.0.0.1 --port 1 --metrics-port 0 2>&1 | head -n 40 || true)"

  if printf '%s\n' "$out" | grep -q 'Initialised [1-9][0-9]* GPU'; then
    printf '%s\n' "$out" | grep -o 'Platform [0-9]* Device [0-9]*: .*' | head -n 8 |
      while IFS= read -r line; do ok "$line"; done
    local seen
    seen="$(printf '%s\n' "$out" | sed -n 's/.*Initialised \([0-9]*\) GPU.*/\1/p' | head -1)"
    seen="${seen:-0}"
    if [ "$seen" -ge "${GPU_COUNT:-1}" ]; then
      ok "the miner is using all $seen of the ${GPU_COUNT:-1} card(s) in this machine"
    elif [ "$GPU_DEVICE" = all ]; then
      warn "the miner is using $seen of the $GPU_COUNT cards in this machine."
      say  "The settings say to use all of them, so one is not being picked up."
      say  "It may be a different driver or a card OpenCL does not expose."
      say  "Check what the miner said with:  $PREFIX/start.sh"
    else
      ok "the miner is using card $GPU_DEVICE, as configured"
      say "This machine has $GPU_COUNT cards. To use all of them, set"
      say "GPU_DEVICE=all in $CONFDIR/config.env and restart."
    fi
    return 0
  fi

  warn "the miner did NOT find a usable graphics card."
  say  "It would run, but on the CPU only, which earns almost nothing."
  say  ""
  if ! have_cl_icd; then
    say  "The most likely reason is the missing OpenCL registration file"
    say  "discussed above. Create it and run this installer again."
  else
    say  "The registration file exists, so the cause is elsewhere. What the"
    say  "miner said:"
    printf '%s\n' "$out" | grep -iE 'opencl|gpu|platform' | head -n 5 |
      while IFS= read -r line; do say "  $line"; done
  fi
  say  ""
  if ! confirm "Continue anyway?" n; then
    die "stopped. Fix the graphics card setup and run this installer again."
  fi
}

# ---------------------------------------------------------- configuration ---
ask_settings() {
  step "Mining settings"

  while :; do
    [ -n "$WALLET" ] || WALLET="$(ask 'Your wallet address (0x...)' '')"
    if valid_wallet "$WALLET"; then break; fi
    if [ "$ASSUME_YES" = 1 ] || [ ! -t 0 ]; then
      die "a wallet address is required. Pass it with --wallet 0x...
    It must be 0x followed by 40 hexadecimal characters."
    fi
    warn "that does not look like a wallet address."
    say  "It must be 0x followed by 40 characters, digits and a-f."
    WALLET=""
  done
  ok "wallet $(printf '%s' "$WALLET" | cut -c1-10)...$(printf '%s' "$WALLET" | tail -c 5)"

  [ -n "$POOL" ]    || POOL="$(ask 'Pool address' "$DEF_POOL")"
  [ -n "$PORT" ]    || PORT="$(ask 'Pool port' "$DEF_PORT")"
  [ -n "$THREADS" ] || THREADS="$(ask 'CPU mining threads (0 = graphics card only)' "$DEF_THREADS")"

  printf '%s' "$PORT" | grep -Eq '^[0-9]+$' || die "the pool port must be a number; got '$PORT'."
  printf '%s' "$THREADS" | grep -Eq '^-?[0-9]+$' || die "CPU threads must be a number; got '$THREADS'."

  # A rig with two cards that mines on one earns half, and says nothing about
  # it. So more than one card means all of them by default, here and in
  # unattended runs.
  if [ -z "$GPU_DEVICE" ]; then
    if [ "${GPU_COUNT:-1}" -gt 1 ]; then
      say ""
      ok "Found $GPU_COUNT graphics cards - all will be used"
      say "Hashrate adds up across cards; leaving one out simply earns less."
      if confirm "Use all $GPU_COUNT?" y; then
        GPU_DEVICE=all
      else
        GPU_DEVICE="$(ask 'Which one? (0 is the first card)' 0)"
      fi
    else
      GPU_DEVICE=0
    fi
  fi
  case "$GPU_DEVICE" in
    all|[0-9]|[0-9][0-9]|*[0-9],[0-9]*) : ;;
    *) die "--gpu-device must be all, a single number, or a list like 0,1;
    got '$GPU_DEVICE'." ;;
  esac
  [ "$GPU_DEVICE" = all ] && ok "using every card" || ok "using card $GPU_DEVICE"

  if [ -z "$LAN" ]; then
    say ""
    say "The dashboard is a web page this miner serves, showing hashrate and"
    say "temperatures, and letting you tune the card."
    say ""
    if confirm "Keep it reachable only from this machine? (recommended)" y; then
      LAN=0
    else
      LAN=1
    fi
  fi
  if [ "$LAN" = 1 ]; then
    BIND="0.0.0.0"
    warn "the dashboard will be reachable from your whole local network."
    say  "Anyone on that network can read your wallet address from it."
    say  "Never forward this port to the internet."
  else
    BIND="127.0.0.1"
    ok "dashboard reachable from this machine only"
    say "To see it from another computer later, use an SSH tunnel:"
    say "  ssh -L 8881:127.0.0.1:8881 $(id -un)@$(hostname -s 2>/dev/null || echo this-machine)"
  fi
}

write_config() {
  step "Writing configuration"
  # The test is whether a config existed BEFORE this run, not whether one
  # exists now: "make install" drops the example file into place a moment
  # earlier, and looking at the filesystem afterwards made the installer
  # believe the operator already had a config. It then kept the example -
  # wallet and all - and the rig mined to 0x0000...0000.
  if [ "$CONFIG_PREEXISTED" = 1 ] && [ "$MODE" != reconfigure ]; then
    local existing
    existing="$(sed -n 's/^WALLET=//p' "$CONFDIR/config.env" 2>/dev/null | head -1)"
    ok "kept the configuration that was already here"
    say "  $CONFDIR/config.env"
    say "  mining to: ${existing:-(no wallet set)}"
    say "Nothing in it was changed. Choose Reconfigure to replace it."
    return
  fi
  run mkdir -p "$CONFDIR"
  run_write "$CONFDIR/config.env" <<CFG
# DAGCore Miner - written by install.sh on $(date -u '+%Y-%m-%d %H:%M UTC')
# The dashboard's Mining configuration can change the wallet, pool, port,
# threads and cards here (keeping the previous file as config.env.bak); its
# tuning goes to $STATEDIR/overrides.env, loaded on top of this file.
WALLET=$WALLET
POOL=$POOL
PORT=$PORT
THREADS=$THREADS
GPU_ENABLED=1
GPU_PLATFORM=0
GPU_DEVICE=$GPU_DEVICE
GPU_INTENSITY=100
GPU_ALIGN=pow2
METRICS_PORT=8881
METRICS_BIND=$BIND
DASHBOARD_DIR=$PREFIX/share/dagcore-miner/dashboard
CFG
  run chmod 0640 "$CONFDIR/config.env"
  did "wrote $CONFDIR/config.env"
}

# -------------------------------------------------------------- service -----
in_container() {
  [ -f /.dockerenv ] && return 0
  command -v systemd-detect-virt >/dev/null 2>&1 && systemd-detect-virt --container -q && return 0
  return 1
}
has_systemd() { [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; }

write_start_script() {
  run_write "$PREFIX/start.sh" <<START
#!/bin/sh
# Start the DAGCore miner in the foreground. Press Ctrl+C to stop it.
exec $PREFIX/bin/dagcore-miner --config $CONFDIR/config.env
START
  run chmod 0755 "$PREFIX/start.sh"
  did "wrote $PREFIX/start.sh"
}

setup_service() {
  step "Starting automatically"

  if ! has_systemd || in_container; then
    if in_container; then
      warn "this looks like a container, which has no service manager of its own."
    else
      warn "this system does not use systemd, so no service can be created."
    fi
    say "The miner will not start on its own. Start it by hand with:"
    say "  sudo $PREFIX/start.sh"
    write_start_script
    WANT_SERVICE=n
    return
  fi

  if [ -z "$WANT_SERVICE" ]; then
    say "A service makes the miner start when the machine boots, and restart"
    say "by itself if it ever stops."
    if confirm "Create it?" y; then WANT_SERVICE=y; else WANT_SERVICE=n; fi
  fi

  if [ "$WANT_SERVICE" != y ]; then
    say "No service created."
    write_start_script
    say "Start the miner by hand with:  sudo $PREFIX/start.sh"
    return
  fi

  run_write "$UNIT" <<UNITFILE
[Unit]
Description=DAGCore Miner
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=$PREFIX/bin
ExecStart=$PREFIX/bin/dagcore-miner --config $CONFDIR/config.env
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
UNITFILE
  run systemctl daemon-reload
  run systemctl enable "$SERVICE" >/dev/null 2>&1 || true
  did "service created and enabled at boot"
}

# ---------------------------------------------------------------- start -----
start_mining() {
  step "Starting"
  if [ -z "$WANT_START" ]; then
    if confirm "Start mining now?" y; then WANT_START=y; else WANT_START=n; fi
  fi
  if [ "$WANT_START" != y ]; then
    say "Not started."
    return
  fi
  if [ "$WANT_SERVICE" = y ]; then
    run systemctl restart "$SERVICE" || die "the service would not start.
    See what it said with:  journalctl -u $SERVICE -n 30"
    [ "$DRY_RUN" = 1 ] || sleep 4
    if [ "$DRY_RUN" != 1 ] && ! systemctl is-active --quiet "$SERVICE"; then
      die "the miner started and then stopped.
    See why with:  journalctl -u $SERVICE -n 30"
    fi
    did "mining"
  else
    say "Start it when you are ready:  sudo $PREFIX/start.sh"
    WANT_START=n
  fi
}

# -------------------------------------------------------------- summary -----
show_token() {
  local f="$CONFDIR/api-token"
  if [ "$DRY_RUN" = 1 ]; then
    say "Control token:   created at first start, in $f"
    return
  fi
  if [ -r "$f" ]; then
    say "Control token:   $(cat "$f")"
    say "                 (paste it into the dashboard to change settings)"
  else
    say "Control token:   appears in $f the first time the miner runs."
    say "                 Read it with:  sudo cat $f"
  fi
}

summary() {
  local url_host="127.0.0.1" ssh_host
  [ "$LAN" = 1 ] && url_host="$(hostname -I 2>/dev/null | awk '{print $1}')"
  [ -n "$url_host" ] || url_host="127.0.0.1"
  # For the tunnel hint the reader is on another machine, so 127.0.0.1 would be
  # their own computer. Give them something they can connect to.
  ssh_host="$(hostname -I 2>/dev/null | awk '{print $1}')"
  [ -n "$ssh_host" ] || ssh_host="$(hostname -s 2>/dev/null || echo this-machine)"

  printf '\n%s%s%s\n' "$B" "=========================================================" "$R"
  printf '%s  DAGCore Miner is installed%s\n' "$B" "$R"
  printf '%s%s%s\n\n' "$B" "=========================================================" "$R"

  say "Installed in:    $PREFIX"
  say "Configuration:   $CONFDIR/config.env"
  if [ "$DRY_RUN" != 1 ]; then
    say "Mining to:       $(sed -n 's/^WALLET=//p' "$CONFDIR/config.env" 2>/dev/null | head -1)"
  else
    say "Mining to:       ${WALLET:-(asked during a real run)}"
  fi
  say "Dashboard:       http://$url_host:8881/"
  say "Help page:       http://$url_host:8881/help"
  show_token
  printf '\n'
  say "Useful commands:"
  if [ "$WANT_SERVICE" = y ]; then
    say "  see if it is running   sudo systemctl status $SERVICE"
    say "  watch the log          sudo journalctl -u $SERVICE -f"
    say "  stop it                sudo systemctl stop $SERVICE"
    say "  start it               sudo systemctl start $SERVICE"
  else
    say "  start it               sudo $PREFIX/start.sh"
    say "  stop it                press Ctrl+C in that window"
  fi
  say "  remove everything      sudo ./uninstall.sh"
  printf '\n'
  if [ "$LAN" != 1 ]; then
    say "The dashboard is only reachable from this machine. From another"
    say "computer on your network, open a tunnel first:"
    say "  ssh -L 8881:127.0.0.1:8881 $(id -un)@$ssh_host"
    say "then open http://127.0.0.1:8881/ there."
    printf '\n'
  fi
  if [ "$DRY_RUN" = 1 ]; then
    printf '  %sThis was a dry run. Nothing on this machine was changed.%s\n\n' "$YELLOW" "$R"
  fi
}

# ------------------------------------------------------------------ main ----
printf '\n%s  DAGCore Miner installer%s\n' "$B" "$R"
[ "$DRY_RUN" = 1 ] && printf '  %sdry run - nothing will be changed%s\n' "$YELLOW" "$R"

detect_pkg
need_root "$@"
check_platform
check_card
check_driver
check_disk

# Captured before anything is installed: "make install" creates config.env
# from the example when there is none, so after that point the filesystem can
# no longer tell an operator's config from a freshly dropped template.
[ -f "$CONFDIR/config.env" ] && CONFIG_PREEXISTED=1

# An existing installation is not overwritten without being asked.
if [ -x "$PREFIX/bin/dagcore-miner" ]; then
  step "Found an existing installation in $PREFIX"
  if [ "$ASSUME_YES" = 1 ] || [ ! -t 0 ]; then
    MODE=upgrade
    say "Upgrading it and keeping the current settings."
  else
    say "  1) Upgrade    - rebuild and replace the program, keep all settings"
    say "  2) Reconfigure- ask the questions again and rewrite the settings"
    say "  3) Cancel"
    case "$(ask 'Which one?' 1)" in
      1) MODE=upgrade ;;
      2) MODE=reconfigure ;;
      *) say "Cancelled. Nothing was changed."; exit 0 ;;
    esac
  fi
else
  MODE=install
fi

check_deps

if [ "$MODE" = upgrade ] && [ "$CONFIG_PREEXISTED" = 1 ]; then
  # Keep what is there; the summary still needs to know how it is set up.
  LAN=0
  grep -q '^METRICS_BIND=0\.0\.0\.0' "$CONFDIR/config.env" 2>/dev/null && LAN=1
  GPU_DEVICE="$(sed -n 's/^GPU_DEVICE=//p' "$CONFDIR/config.env" 2>/dev/null | head -1)"
  [ -n "$GPU_DEVICE" ] || GPU_DEVICE=0
  build_and_install
  step "Configuration"
  ok "kept $CONFDIR/config.env unchanged"
else
  ask_settings
  build_and_install
  write_config
fi

run mkdir -p "$STATEDIR"
run chmod 0750 "$STATEDIR"
verify_wallet_configured
verify_gpu_visible
setup_service
start_mining
summary
