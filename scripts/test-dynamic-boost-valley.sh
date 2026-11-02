#!/bin/bash
# Verify NVIDIA Dynamic Boost reaches ~85 W under Valley load on the A16 CWH.
# Run as root: sudo bash scripts/test-dynamic-boost-valley.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SYS=/sys/devices/platform/aorus_laptop
LOG=/tmp/dynamic-boost-valley-power.csv
: "${MIN_DRAW:=75}"
: "${MIN_LIMIT:=80}"
: "${VALLEY_DIR:=/home/jose-lopez/Downloads/Unigine_Valley-1.0}"
VALLEY_USER="${SUDO_USER:-${USER}}"

if [[ -z "${DISPLAY:-}" ]]; then
	export DISPLAY=:0
fi

echo "== Max performance + dynamic boost (order matters: perf → gpu_boost → dynamic) =="
echo 2 > "$SYS/perf_mode"
sleep 1
echo 1 > "$SYS/gpu_boost"
sleep 1
echo 1 > "$SYS/dynamic_boost"
sleep 2
systemctl try-restart nvidia-powerd 2>/dev/null || true
sleep 2

echo "dynamic_boost=$(cat "$SYS/dynamic_boost") DBAC=$(echo 0xE7 > "$SYS/debug_method"; cat "$SYS/debug_method")"
echo "limits: $(nvidia-smi --query-gpu=power.limit,enforced.power.limit,power.max_limit --format=csv,noheader | tr -d ' ')"

echo "t,power_draw_W,enforced_limit_W,gpu_temp_C" > "$LOG"
rm -f /tmp/dynamic-boost-valley-done

monitor() {
	local start end draw limit gtemp
	start=$(date +%s)
	while [[ ! -f /tmp/dynamic-boost-valley-done ]]; do
		end=$(date +%s)
		read -r draw limit gtemp < <(nvidia-smi \
			--query-gpu=power.draw,enforced.power.limit,temperature.gpu \
			--format=csv,noheader,nounits 2>/dev/null | head -1 \
			| awk -F, '{gsub(/ /, "", $1); gsub(/ /, "", $2); gsub(/ /, "", $3); print $1, $2, $3}')
		echo "$((end - start)),${draw:-0},${limit:-0},${gtemp:-0}" >> "$LOG"
		sleep 1
	done
}

echo
echo "== Running Valley (target peak draw >= ${MIN_DRAW} W) =="
monitor &
MON_PID=$!

sudo -u "$VALLEY_USER" -H env DISPLAY="$DISPLAY" \
	__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia \
	VALLEY_DIR="$VALLEY_DIR" bash scripts/run-valley-headless.sh >/tmp/dynamic-boost-valley-bench.log 2>&1

touch /tmp/dynamic-boost-valley-done
wait "$MON_PID" 2>/dev/null || true

echo
RESULT=0
awk -F, -v min_draw="$MIN_DRAW" -v min_limit="$MIN_LIMIT" '
NR > 1 {
	d = $2 + 0; l = $3 + 0
	if (d > maxd) maxd = d
	if (l > 0) {
		if (minl == 0 || l < minl) minl = l
		if (l > maxl) maxl = l
	}
	n++
}
END {
	printf "samples=%d max_draw=%.1f W enforced_limit=%.1f-%.1f W\n", n, maxd, minl, maxl
	if (maxd < min_draw) {
		printf "FAIL: peak draw %.1f W < %d W (dynamic boost not engaging)\n", maxd, min_draw
		exit 2
	}
	if (maxl < min_limit) {
		printf "FAIL: enforced limit %.1f W < %d W under load\n", maxl, min_limit
		exit 1
	}
	printf "PASS: dynamic boost reached %.1f W draw at %.0f W limit\n", maxd, maxl
}' "$LOG" || RESULT=$?

echo
echo "Peak samples:"
awk -F, 'NR > 1' "$LOG" | sort -t, -k2 -nr | head -6 | while IFS=, read -r t d l g; do
	echo "  t=${t}s draw=${d}W limit=${l}W gpu=${g}C"
done

exit "$RESULT"
