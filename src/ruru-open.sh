#!/usr/bin/env bash
# ruru-open — Super-key launcher for Ruru.
#
# Makes sure the local speech service is up, then shows its state in a small
# KDE dialog. No terminal window.
set -euo pipefail

CTL="${HOME}/.local/bin/ruru"
CLIENT="${HOME}/.local/share/ruru/ruru-cli"

# Start the service if it is not already running.
systemctl --user is-active --quiet ruru.service || systemctl --user start ruru.service 2>/dev/null || true

# Give the socket a moment to appear.
for _ in $(seq 1 40); do
  [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ruru.sock" ]] && break
  sleep 0.25
done

body="$("${CLIENT}" status 2>/dev/null | sed 's/^"//; s/"$//' || echo "not answering yet")"

kdialog --title "Ruru — local speech to text" \
  --icon ruru \
  --msgbox "Service status:\n\n${body}\n\nAudio never leaves this computer.\n\nUse 'ruru' in a terminal for wav/pcm transcription." 2>/dev/null \
  || "${CTL}" status
