#!/bin/bash
# Installs the ReacJack native macOS stack: reacjackd, reacjackctl, and the
# ReacJack.driver CoreAudio HAL plug-in. With -i, also installs a launchd
# daemon so REAC capture starts at boot and restarts on failure.
#
# Path overrides for testing: REACJACK_BIN_DIR, REACJACK_HAL_DIR,
# REACJACK_LAUNCHD_DIR, REACJACK_NO_LAUNCHCTL=1.
set -euo pipefail

BIN_DIR="${REACJACK_BIN_DIR:-/usr/local/bin}"
HAL_DIR="${REACJACK_HAL_DIR:-/Library/Audio/Plug-Ins/HAL}"
LAUNCHD_DIR="${REACJACK_LAUNCHD_DIR:-/Library/LaunchDaemons}"
LABEL="com.reacjack.reacjackd"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
  echo "usage: sudo $0 [-i interface]" >&2
  echo "  -i  Ethernet interface connected to REAC; installs a launchd" >&2
  echo "      daemon that captures from it at boot" >&2
}

IFACE=""
while getopts i:h opt; do
  case "$opt" in
    i) IFACE="$OPTARG" ;;
    h) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

for artifact in reacjackd reacjackctl "ReacJack.driver/Contents/MacOS/ReacJack"; do
  if [ ! -e "$ROOT/$artifact" ]; then
    echo "Missing $artifact. Run make first." >&2
    exit 1
  fi
done

if [ "$BIN_DIR" = "/usr/local/bin" ] && [ "$(id -u)" -ne 0 ]; then
  echo "Installing to system paths requires root. Re-run with sudo." >&2
  exit 1
fi

mkdir -p "$BIN_DIR" "$HAL_DIR"
install -m 755 "$ROOT/reacjackd" "$ROOT/reacjackctl" "$BIN_DIR/"
rm -rf "$HAL_DIR/ReacJack.driver"
cp -R "$ROOT/ReacJack.driver" "$HAL_DIR/"
echo "Installed reacjackd and reacjackctl to $BIN_DIR"
echo "Installed ReacJack.driver to $HAL_DIR"

if [ -n "$IFACE" ]; then
  mkdir -p "$LAUNCHD_DIR"
  cat > "$LAUNCHD_DIR/$LABEL.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>$BIN_DIR/reacjackd</string>
		<string>-i</string>
		<string>$IFACE</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>StandardOutPath</key>
	<string>/var/log/reacjackd.log</string>
	<key>StandardErrorPath</key>
	<string>/var/log/reacjackd.log</string>
</dict>
</plist>
PLIST
  echo "Installed launchd daemon $LABEL (interface $IFACE)"
  if [ -z "${REACJACK_NO_LAUNCHCTL:-}" ]; then
    launchctl bootout "system/$LABEL" 2>/dev/null || true
    launchctl bootstrap system "$LAUNCHD_DIR/$LABEL.plist"
    echo "Daemon started; it logs to /var/log/reacjackd.log"
  fi
fi

if [ -z "${REACJACK_NO_LAUNCHCTL:-}" ]; then
  echo "Restarting coreaudiod to load the driver (system audio blips briefly)"
  launchctl kickstart -kp system/com.apple.audio.coreaudiod
fi

echo
echo "Next steps:"
echo "  - \"ReacJack REAC\" should appear in Audio MIDI Setup"
if [ -z "$IFACE" ]; then
  echo "  - start capture manually: sudo $BIN_DIR/reacjackd -i <interface>"
fi
echo "  - watch ring health: $BIN_DIR/reacjackctl -w"
echo "  - record the device in any CoreAudio application"
