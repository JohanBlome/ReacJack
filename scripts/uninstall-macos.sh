#!/bin/bash
# Removes everything scripts/install-macos.sh installed: the launchd daemon,
# the binaries, and the CoreAudio HAL plug-in. The shared memory ring is not
# a file; it disappears when the daemon stops.
#
# Path overrides for testing: REACJACK_BIN_DIR, REACJACK_HAL_DIR,
# REACJACK_LAUNCHD_DIR, REACJACK_NO_LAUNCHCTL=1.
set -euo pipefail

BIN_DIR="${REACJACK_BIN_DIR:-/usr/local/bin}"
HAL_DIR="${REACJACK_HAL_DIR:-/Library/Audio/Plug-Ins/HAL}"
LAUNCHD_DIR="${REACJACK_LAUNCHD_DIR:-/Library/LaunchDaemons}"
LABEL="com.reacjack.reacjackd"

if [ "$BIN_DIR" = "/usr/local/bin" ] && [ "$(id -u)" -ne 0 ]; then
  echo "Removing from system paths requires root. Re-run with sudo." >&2
  exit 1
fi

if [ -e "$LAUNCHD_DIR/$LABEL.plist" ]; then
  if [ -z "${REACJACK_NO_LAUNCHCTL:-}" ]; then
    launchctl bootout "system/$LABEL" 2>/dev/null || true
  fi
  rm -f "$LAUNCHD_DIR/$LABEL.plist"
  echo "Removed launchd daemon $LABEL"
fi

rm -f "$BIN_DIR/reacjackd" "$BIN_DIR/reacjackctl"
rm -rf "$HAL_DIR/ReacJack.driver"
echo "Removed binaries from $BIN_DIR and ReacJack.driver from $HAL_DIR"

if [ -z "${REACJACK_NO_LAUNCHCTL:-}" ]; then
  echo "Restarting coreaudiod to unload the driver (system audio blips briefly)"
  launchctl kickstart -kp system/com.apple.audio.coreaudiod
fi

echo "ReacJack uninstalled."
