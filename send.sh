#!/usr/bin/env bash
# send.sh — push dlc_dump.elf to a jailbroken PS5's ELF loader (port 9021).
#
#   ./send.sh                      # uses 192.168.1.6:9021
#   ./send.sh 192.168.1.20         # different console IP
#   ./send.sh 192.168.1.6 9021     # explicit port
#
# Works on macOS and Linux. Uses socat if present, falls back to nc.
# NOTE: macOS/BSD netcat has no -q flag, which is why socat is preferred.

set -uo pipefail

HOST="${1:-192.168.1.6}"
PORT="${2:-9021}"
ELF="${ELF:-$(dirname "$0")/dlc_dump.elf}"

if [[ ! -f "$ELF" ]]; then
  echo "ERROR: payload not found: $ELF"
  echo "       Build it first:  export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk && make"
  exit 1
fi

SIZE=$(wc -c < "$ELF" | tr -d ' ')
echo "Payload : $ELF (${SIZE} bytes)"
echo "Target  : ${HOST}:${PORT}"
echo

# NOTE: deliberately NO "is the port open?" pre-check here.
# Any TCP connect to the ELF loader is treated by it as a payload upload, so a
# probe that connects and disconnects would hand elfldr a zero-byte ELF.
# We just send, and interpret the failure afterwards.

echo "Sending ..."
if command -v socat >/dev/null 2>&1; then
  socat -t 99999999 - "TCP:${HOST}:${PORT}" < "$ELF"
  RC=$?
elif command -v nc >/dev/null 2>&1; then
  # BSD nc (macOS) has no -q; GNU nc does. Try GNU form, fall back.
  if nc -h 2>&1 | grep -q '\-q'; then
    nc -q0 "$HOST" "$PORT" < "$ELF"; RC=$?
  else
    nc "$HOST" "$PORT" < "$ELF"; RC=$?
  fi
else
  echo "ERROR: neither socat nor nc is installed."
  echo "       macOS:  brew install socat"
  echo "       Debian: sudo apt-get install socat"
  exit 1
fi

echo
if [[ $RC -eq 0 ]]; then
  echo "Payload sent."
  echo
  echo "Watch for a toast notification on the console: 'DLC Dump: started'."
  echo "When it finishes you'll get a second toast with file count and size."
  echo "Full log lands at:  <usb>/PS5_DLC_DUMP/dump.log"
else
  echo "FAILED (exit $RC). Most likely causes, in order:"
  echo "  1. Nothing listening on ${HOST}:${PORT} — etaHEN not running, or the"
  echo "     jailbreak dropped. Re-run the exploit and reload etaHEN."
  echo "  2. Wrong IP. Check Settings > Network > Connection Status."
  echo "  3. Mac and PS5 on different subnets / VLANs / guest Wi-Fi."
fi
exit $RC
