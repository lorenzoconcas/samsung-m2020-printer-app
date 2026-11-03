#!/bin/sh
#
# Install and start the printer application as a per-user LaunchAgent on macOS.
# It starts at login, listens on port 8000 and advertises the printer via Bonjour.
#
# Usage: scripts/install-macos-agent.sh [/usr/local]     (prefix used by "make install")
#

set -eu

PREFIX=${1:-/usr/local}
LABEL=local.samsung-m2020-printer-app
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/samsung-m2020-printer-app.log"
BIN="$PREFIX/bin/samsung-m2020-printer-app"

[ -x "$BIN" ] || { echo "$BIN not found: run 'sudo make install' first."; exit 1; }

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"

cat >"$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$BIN</string>
    <string>server</string>
    <string>-o</string><string>log-file=-</string>
    <string>-o</string><string>log-level=info</string>
    <string>-o</string><string>server-port=8000</string>
  </array>
  <key>EnvironmentVariables</key>
  <dict>
    <!-- <key>M2020_ENABLE_1200DPI</key><string>1</string> -->
  </dict>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <dict>
    <key>SuccessfulExit</key>
    <false/>
  </dict>
  <key>StandardErrorPath</key>
  <string>$LOG</string>
  <key>StandardOutPath</key>
  <string>$LOG</string>
</dict>
</plist>
EOF

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"

echo "Started. Web interface: http://localhost:8000/   Log: $LOG"
echo "To remove: launchctl bootout gui/$(id -u)/$LABEL && rm '$PLIST'"
