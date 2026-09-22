#!/usr/bin/env bash
#
# Remove DAGCore Miner. The program goes without asking; your settings and the
# tuning you arrived at are asked about separately, because they are the parts
# that took effort and cannot be rebuilt from the package.
#
set -euo pipefail

PREFIX="/opt/dagcore-miner"
CONFDIR="/etc/dagcore-miner"
STATEDIR="/var/lib/dagcore-miner"
SERVICE="dagcore-miner"
UNIT="/etc/systemd/system/${SERVICE}.service"
DRY_RUN=0; ASSUME_YES=0; KEEP_CONFIG=""; KEEP_STATE=""

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  B=$'\033[1m'; DIM=$'\033[2m'; R=$'\033[0m'
  GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; CYAN=$'\033[36m'
else B=""; DIM=""; R=""; GREEN=""; YELLOW=""; RED=""; CYAN=""; fi
step() { printf '\n%s==>%s %s%s%s\n' "$CYAN" "$R" "$B" "$*" "$R"; }
say()  { printf '    %s\n' "$*"; }
ok()   { printf '    %s✓%s %s\n' "$GREEN" "$R" "$*"; }
# Asserts an action completed, so it stays quiet during a dry run.
did()  { [ "$DRY_RUN" = 1 ] || ok "$*"; }
warn() { printf '    %s!%s  %s\n' "$YELLOW" "$R" "$*"; }
die()  { printf '\n%sSomething went wrong:%s %s\n' "$RED" "$R" "$*" >&2; exit 1; }

run() {
  if [ "$DRY_RUN" = 1 ]; then printf '    %swould run:%s %s\n' "$DIM" "$R" "$*"; return 0; fi
  "$@"
}
confirm() {
  local q="$1" def="${2:-n}" ans=""
  if [ "$ASSUME_YES" = 1 ] || [ ! -t 0 ]; then [ "$def" = y ]; return; fi
  local hint="[y/N]"; [ "$def" = y ] && hint="[Y/n]"
  while :; do
    printf '    %s %s ' "$q" "$hint" >&2
    IFS= read -r ans || true; ans="${ans:-$def}"
    case "$ans" in [Yy]*) return 0 ;; [Nn]*) return 1 ;; esac
    printf '    Please answer y or n.\n' >&2
  done
}

usage() {
  cat <<USAGE
Remove DAGCore Miner

  ./uninstall.sh [options]

  --keep-config    keep $CONFDIR (wallet, pool, worker)
  --keep-tuning    keep $STATEDIR (clocks, offsets, power limit)
  --purge          remove everything, including both of those
  --yes            ask nothing; keeps config and tuning unless --purge
  --dry-run        show what would happen; change nothing
  --help           this text
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    --keep-config) KEEP_CONFIG=y; shift ;;
    --keep-tuning) KEEP_STATE=y;  shift ;;
    --purge)       KEEP_CONFIG=n; KEEP_STATE=n; shift ;;
    --yes|-y)      ASSUME_YES=1;  shift ;;
    --dry-run)     DRY_RUN=1;     shift ;;
    --help|-h)     usage; exit 0 ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
done

[ "$(id -u)" = 0 ] || [ "$DRY_RUN" = 1 ] || \
  die "this needs to remove files owned by root. Run it with:  sudo ./uninstall.sh"

printf '\n%s  Removing DAGCore Miner%s\n' "$B" "$R"
[ "$DRY_RUN" = 1 ] && printf '  %sdry run - nothing will be changed%s\n' "$YELLOW" "$R"

if [ ! -e "$PREFIX" ] && [ ! -e "$UNIT" ] && [ ! -e "$CONFDIR" ]; then
  say ""; ok "nothing to remove - DAGCore Miner does not appear to be installed."
  exit 0
fi

step "Stopping the miner"
if command -v systemctl >/dev/null 2>&1 && [ -e "$UNIT" ]; then
  run systemctl stop "$SERVICE" 2>/dev/null || true
  run systemctl disable "$SERVICE" 2>/dev/null || true
  run rm -f "$UNIT"
  run systemctl daemon-reload 2>/dev/null || true
  did "service stopped and removed"
else
  say "No service was installed."
  say "If you started it by hand, close that window or press Ctrl+C in it."
fi

step "Removing the program"
run rm -rf "$PREFIX"
did "removed $PREFIX"

step "Your settings"
say "$CONFDIR holds the wallet address, pool and worker name."
if [ -e "$CONFDIR" ]; then
  if [ -z "$KEEP_CONFIG" ]; then
    if confirm "Keep them, in case you reinstall?" y; then KEEP_CONFIG=y; else KEEP_CONFIG=n; fi
  fi
  if [ "$KEEP_CONFIG" = y ]; then
    did "kept $CONFDIR"
  else
    run rm -rf "$CONFDIR"
    did "removed $CONFDIR"
  fi
else
  say "Nothing there."
fi

step "Your tuning"
say "$STATEDIR holds the power limit, clocks and offsets you arrived at -"
say "including anything that passed a ten-minute stability test."
if [ -e "$STATEDIR" ]; then
  if [ -z "$KEEP_STATE" ]; then
    if confirm "Keep them?" y; then KEEP_STATE=y; else KEEP_STATE=n; fi
  fi
  if [ "$KEEP_STATE" = y ]; then
    did "kept $STATEDIR"
  else
    run rm -rf "$STATEDIR"
    did "removed $STATEDIR"
  fi
else
  say "Nothing there."
fi

printf '\n'
if [ "$DRY_RUN" = 1 ]; then
  say "That is everything that would be removed."
else
  ok "DAGCore Miner has been removed."
  [ "${KEEP_CONFIG:-n}" = y ] && say "Settings kept in $CONFDIR"
  [ "${KEEP_STATE:-n}" = y ]  && say "Tuning kept in $STATEDIR"
fi
if [ "$DRY_RUN" = 1 ]; then
  printf '\n  %sThis was a dry run. Nothing on this machine was changed.%s\n' "$YELLOW" "$R"
fi
printf '\n'
