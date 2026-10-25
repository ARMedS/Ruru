#!/usr/bin/env bash
# ruru — controller for the local speech-to-text service.
#
# The daemon itself lives in ~/.local/share/ruru/ and is started as a systemd
# user service (ruru.service). This wrapper is what a person types, and what
# the Super-key launcher runs.
set -euo pipefail

HOME_DIR="${HOME}/.local/share/ruru"
DAEMON="${HOME_DIR}/rurud"
CLIENT="${HOME_DIR}/ruru-cli"
UNIT="ruru.service"

usage() {
  cat <<EOF
ruru — local speech-to-text service (private, offline)

  ruru status        show service, model and backend state
  ruru start         start the service (if not already running)
  ruru stop          stop the service
  ruru restart       restart the service
  ruru logs [N]      show the last N log lines (default 40)
  ruru ping          check the service answers
  ruru wav FILE      transcribe a 16 kHz mono WAV (prints the text)
  ruru pcm FILE      transcribe raw 16 kHz mono float32
  ruru check         load the model and print its identity, then exit

Everything runs on this machine. Audio never leaves it.
EOF
}

need_client() {
  [[ -x "${CLIENT}" ]] || { echo "ruru: client missing at ${CLIENT}" >&2; exit 1; }
}

ensure_running() {
  if ! systemctl --user is-active --quiet "${UNIT}"; then
    echo "ruru: service is not running — starting it"
    systemctl --user start "${UNIT}"
    for _ in $(seq 1 60); do
      [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ruru.sock" ]] && break
      sleep 0.5
    done
  fi
}

cmd="${1:-status}"
case "${cmd}" in
  status)
    echo "=== ruru service ==="
    systemctl --user --no-pager --full status "${UNIT}" 2>/dev/null | head -8 || true
    echo
    echo "=== ruru ==="
    need_client
    if out="$("${CLIENT}" status 2>/dev/null)"; then
      echo "${out}" | python3 -m json.tool 2>/dev/null || echo "${out}"
    else
      echo "not answering on ${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ruru.sock"
    fi
    ;;
  start)
    systemctl --user start "${UNIT}"
    sleep 1
    need_client
    "${CLIENT}" status 2>/dev/null || echo "started, waiting for the socket"
    ;;
  stop)
    systemctl --user stop "${UNIT}"
    echo "stopped"
    ;;
  restart)
    systemctl --user restart "${UNIT}"
    sleep 1
    need_client
    "${CLIENT}" status 2>/dev/null || true
    ;;
  logs)
    journalctl --user -u "${UNIT}" --no-pager -n "${2:-40}"
    ;;
  ping|wav|pcm)
    ensure_running
    need_client
    exec "${CLIENT}" "${cmd}" "${2:-}"
    ;;
  check)
    [[ -x "${DAEMON}" ]] || { echo "ruru: daemon missing at ${DAEMON}" >&2; exit 1; }
    RURU_ARTIFACT_DIR="${HOME_DIR}/lib" exec "${DAEMON}" --check
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    echo "ruru: unknown command '${cmd}'" >&2
    usage >&2
    exit 2
    ;;
esac
