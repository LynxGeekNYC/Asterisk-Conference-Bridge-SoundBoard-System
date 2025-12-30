#!/usr/bin/env bash
set -euo pipefail

# asterisk-soundboard installer (ConfBridge soundboard)
# - Installs binary to /usr/local/bin/asterisk-soundboard
# - Creates /etc/asterisk/asterisk-soundboard.conf (if missing)
# - Creates sound directory /var/lib/asterisk/sounds/custom/soundboard
# - Adds AMI user block to /etc/asterisk/manager.conf (idempotent)
# - Reloads AMI
# - Runs basic connectivity and capability checks
#
# Usage:
#   sudo ./install.sh
#   sudo ./install.sh --bin ./asterisk_soundboard_adv --username soundboard --secret 'StrongSecret' --host 127.0.0.1 --port 5038
#
# Notes:
# - This script assumes Asterisk is installed and running.
# - It will NOT overwrite an existing config file unless you pass --force-config.

BIN_SRC=""
BIN_DEST="/usr/local/bin/asterisk-soundboard"
CFG_PATH="/etc/asterisk/asterisk-soundboard.conf"
MANAGER_CONF="/etc/asterisk/manager.conf"
SOUNDS_DIR="/var/lib/asterisk/sounds/custom/soundboard"
STATE_DIR="/var/lib/asterisk/soundboard"

HOST="127.0.0.1"
PORT="5038"
USERNAME="soundboard"
SECRET=""
FORCE_CONFIG="0"
NO_AMI_EDIT="0"

log() { printf '%s\n' "[*] $*"; }
warn() { printf '%s\n' "[!] $*" >&2; }
die() { printf '%s\n' "[x] $*" >&2; exit 1; }

need_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    die "Please run as root (use sudo)."
  fi
}

have_cmd() { command -v "$1" >/dev/null 2>&1; }

rand_secret() {
  if have_cmd openssl; then
    openssl rand -base64 24 | tr -d '\n'
  elif have_cmd python3; then
    python3 - <<'PY'
import secrets, string
alphabet = string.ascii_letters + string.digits + "!@#$%^&*()-_=+"
print("".join(secrets.choice(alphabet) for _ in range(28)))
PY
  else
    # last resort
    date +%s%N | sha256sum | awk '{print substr($1,1,28)}'
  fi
}

backup_file() {
  local f="$1"
  if [[ -f "$f" ]]; then
    local ts
    ts="$(date +%Y%m%d%H%M%S)"
    cp -a "$f" "${f}.bak.${ts}"
    log "Backup created: ${f}.bak.${ts}"
  fi
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --bin)
        BIN_SRC="${2:-}"; shift 2;;
      --dest)
        BIN_DEST="${2:-}"; shift 2;;
      --config)
        CFG_PATH="${2:-}"; shift 2;;
      --manager-conf)
        MANAGER_CONF="${2:-}"; shift 2;;
      --sounds-dir)
        SOUNDS_DIR="${2:-}"; shift 2;;
      --state-dir)
        STATE_DIR="${2:-}"; shift 2;;
      --host)
        HOST="${2:-}"; shift 2;;
      --port)
        PORT="${2:-}"; shift 2;;
      --username)
        USERNAME="${2:-}"; shift 2;;
      --secret)
        SECRET="${2:-}"; shift 2;;
      --force-config)
        FORCE_CONFIG="1"; shift 1;;
      --no-ami-edit)
        NO_AMI_EDIT="1"; shift 1;;
      -h|--help)
        cat <<EOF
Usage: sudo $0 [options]

Options:
  --bin <path>            Source binary path (default: auto-detect in current dir)
  --dest <path>           Install path for binary (default: ${BIN_DEST})
  --config <path>         Config file path (default: ${CFG_PATH})
  --manager-conf <path>   manager.conf path (default: ${MANAGER_CONF})
  --sounds-dir <path>     Soundboard sound directory (default: ${SOUNDS_DIR})
  --state-dir <path>      State directory (default: ${STATE_DIR})
  --host <ip/host>        AMI host (default: ${HOST})
  --port <port>           AMI port (default: ${PORT})
  --username <name>       AMI username to create/use (default: ${USERNAME})
  --secret <secret>       AMI secret (default: auto-generate if missing)
  --force-config          Overwrite existing config file
  --no-ami-edit           Do not modify manager.conf, print block instead

Examples:
  sudo $0 --bin ./asterisk_soundboard_adv
  sudo $0 --username soundboard --secret 'StrongSecret'
EOF
        exit 0
        ;;
      *)
        die "Unknown argument: $1 (use --help)"
        ;;
    esac
  done
}

detect_binary() {
  if [[ -n "$BIN_SRC" ]]; then
    [[ -f "$BIN_SRC" ]] || die "Binary not found at: $BIN_SRC"
    return
  fi

  # common build outputs in the current directory
  for c in "./asterisk-soundboard" "./asterisk_soundboard_adv" "./asterisk_soundboard" "./asterisk-soundboard-adv"; do
    if [[ -f "$c" ]]; then
      BIN_SRC="$c"
      return
    fi
  done

  die "No binary specified. Build it first or provide --bin <path>."
}

ensure_dirs() {
  log "Creating directories"
  mkdir -p "$(dirname "$BIN_DEST")"
  mkdir -p "$(dirname "$CFG_PATH")"
  mkdir -p "$SOUNDS_DIR"
  mkdir -p "$STATE_DIR"

  # Ownership and perms
  # - Sounds: asterisk:asterisk so Asterisk can read them
  # - State: asterisk:asterisk (recents/favorites if you add persistence)
  # - Config: root:asterisk 640
  if id asterisk >/dev/null 2>&1; then
    chown -R asterisk:asterisk "$SOUNDS_DIR" || true
    chown -R asterisk:asterisk "$STATE_DIR" || true
  else
    warn "User 'asterisk' not found. Skipping chown for sounds/state dirs."
  fi
}

install_binary() {
  log "Installing binary: $BIN_SRC -> $BIN_DEST"
  install -m 0755 "$BIN_SRC" "$BIN_DEST"
}

write_config() {
  if [[ -z "$SECRET" ]]; then
    SECRET="$(rand_secret)"
    log "Generated AMI secret for '${USERNAME}': ${SECRET}"
  fi

  if [[ -f "$CFG_PATH" && "$FORCE_CONFIG" != "1" ]]; then
    log "Config exists, not overwriting: $CFG_PATH"
    return
  fi

  log "Writing config: $CFG_PATH"
  backup_file "$CFG_PATH"

  cat >"$CFG_PATH" <<EOF
host=${HOST}
port=${PORT}
username=${USERNAME}
secret=${SECRET}

# One or more sound roots (comma-separated). Must be directories on this machine.
sound_roots=/var/lib/asterisk/sounds,/usr/share/asterisk/sounds

# File extensions to index (comma-separated)
extensions=wav,ulaw,gsm,alaw,sln,sln16

# Favorites (comma-separated), Asterisk sound names (no extension)
favorites=custom/soundboard
EOF

  # Secure permissions
  if getent group asterisk >/dev/null 2>&1; then
    chown root:asterisk "$CFG_PATH" || true
    chmod 640 "$CFG_PATH" || true
  else
    chmod 600 "$CFG_PATH" || true
  fi
}

ami_block_begin="# BEGIN ASTERISK-SOUNDBOARD"
ami_block_end="# END ASTERISK-SOUNDBOARD"

print_ami_block() {
  cat <<EOF

${ami_block_begin}
[${USERNAME}]
secret = ${SECRET}
read = system,call,all
write = system,call,all
${ami_block_end}

EOF
}

ensure_manager_conf() {
  [[ -f "$MANAGER_CONF" ]] || die "manager.conf not found at: $MANAGER_CONF"

  if [[ "$NO_AMI_EDIT" == "1" ]]; then
    warn "--no-ami-edit set. Not modifying manager.conf. Please add this block manually:"
    print_ami_block
    return
  fi

  if grep -qF "$ami_block_begin" "$MANAGER_CONF"; then
    log "AMI block already present in manager.conf, leaving as-is."
    return
  fi

  log "Adding AMI user block to: $MANAGER_CONF"
  backup_file "$MANAGER_CONF"

  {
    echo ""
    print_ami_block
  } >>"$MANAGER_CONF"
}

reload_ami() {
  if ! have_cmd asterisk; then
    warn "asterisk CLI not found in PATH. Skipping manager reload."
    return
  fi

  log "Reloading AMI (manager reload)"
  if ! asterisk -rx "manager reload" >/dev/null 2>&1; then
    warn "AMI reload command failed. You may need to reload Asterisk manually."
  fi
}

install_sample_sound_placeholder() {
  # This does not include audio. It creates a placeholder file name for favorites.
  # You can add your own WAV/ULAW into this directory.
  log "Ensuring sound directory exists: $SOUNDS_DIR"
  mkdir -p "$SOUNDS_DIR"
  if id asterisk >/dev/null 2>&1; then
    chown -R asterisk:asterisk "$SOUNDS_DIR" || true
  fi

  log "Sound directory ready. Put files here, e.g.: ${SOUNDS_DIR}/airhorn.wav"
  log "They will be referenced as: custom/soundboard/airhorn"
}

doctor_checks() {
  log "Running basic checks"

  if ! have_cmd asterisk; then
    warn "asterisk CLI not found. Skipping runtime checks."
    return
  fi

  if ! asterisk -rx "core show version" >/dev/null 2>&1; then
    warn "Asterisk CLI not responding. Is Asterisk running and are you root?"
    return
  fi

  # Check AMI port listening
  if have_cmd ss; then
    if ! ss -ltn | awk '{print $4}' | grep -qE "(:${PORT}\$)"; then
      warn "AMI port ${PORT} does not appear to be listening (ss check). Verify manager.conf and bindaddr."
    else
      log "AMI port ${PORT} appears to be listening."
    fi
  fi

  # Quick ConfBridge sanity check
  if asterisk -rx "module show like confbridge" 2>/dev/null | grep -qi confbridge; then
    log "ConfBridge module appears present."
  else
    warn "ConfBridge module not detected via CLI. If you use MeetMe instead, this tool will not target it."
  fi

  log "Next, run: ${BIN_DEST} ${CFG_PATH}"
  log "If playback fails, verify AMI permissions and whether ConfbridgePlaySound is supported on your build."
}

main() {
  need_root
  parse_args "$@"
  detect_binary
  ensure_dirs
  install_binary
  write_config
  ensure_manager_conf
  reload_ami
  install_sample_sound_placeholder
  doctor_checks

  cat <<EOF

Installation complete.

1) Add sounds:
   - Copy WAV/ULAW into: ${SOUNDS_DIR}
   - Example: ${SOUNDS_DIR}/airhorn.wav
   - Then reference in the app as: custom/soundboard/airhorn

2) Run:
   ${BIN_DEST} ${CFG_PATH}

3) If you used --no-ami-edit, add this AMI block to ${MANAGER_CONF} and reload AMI:
   asterisk -rx "manager reload"

EOF
}

main "$@"
