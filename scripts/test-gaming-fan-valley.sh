#!/bin/bash
# Gaming fan mode GPU power test using Unigine Valley on the NVIDIA dGPU.
#
# Verifies that under sustained GPU load:
#   - power draw approaches the 70 W TGP cap (perf_mode=2 + gpu_boost)
#   - enforced power limit does not drop below 68 W (thermal/fan throttling)
#
# Run as root:
#   sudo bash scripts/test-gaming-fan-valley.sh
#
# Optional env:
#   MIN_DRAW=55   minimum peak draw (W) to consider GPU loaded
#   MIN_LIMIT=68  fail if enforced limit falls below this while draw > 35 W
#   VALLEY_DIR=   path to Unigine Valley install
set -euo pipefail
cd "$(dirname "$0")/.."

SYS=/sys/devices/platform/aorus_laptop
HWMON=$(ls -d "$SYS"/hwmon/hwmon* | head -1)
LOG=/tmp/gaming-fan-valley-power.csv
VALLEY_LOG=/tmp/gaming-fan-valley-bench.log
: "${MIN_DRAW:=55}"
: "${MIN_LIMIT:=68}"
: "${VALLEY_DIR:=/home/jose-lopez/Downloads/Unigine_Valley-1.0}"

if [[ ! -d "$SYS" ]]; then
	echo "aorus_laptop sysfs not found; is the driver loaded?" >&2
	exit 1
fi

if [[ ! -x "$VALLEY_DIR/bin/valley_x64" && ! -x "$VALLEY_DIR/bin/valley_x86" ]]; then
	echo "Unigine Valley not found at $VALLEY_DIR" >&2
	exit 1
fi

# Valley needs the desktop user's session (DISPLAY, NVIDIA offload).
VALLEY_USER="${SUDO_USER:-${USER}}"
if [[ -z "${DISPLAY:-}" ]]; then
	export DISPLAY=:0
fi

echo "== Configuring max performance + gaming fan mode =="
echo 2 > "$SYS/fan_mode"
echo 2 > "$SYS/perf_mode"
echo 1 > "$SYS/gpu_boost"
echo 1 > "$SYS/dynamic_boost"
sleep 2

echo "fan_mode=$(cat "$SYS/fan_mode") perf_mode=$(cat "$SYS/perf_mode") gpu_boost=$(cat "$SYS/gpu_boost")"
echo "enforced limit=$(nvidia-smi --query-gpu=enforced.power.limit --format=csv,noheader,nounits | tr -d ' ') W"
echo "idle RPMs=$(cat "$HWMON/fan1_input")/$(cat "$HWMON/fan2_input")"
echo "Valley user: $VALLEY_USER  DISPLAY=$DISPLAY"

nvidia-smi -lgc 2800,3090 2>/dev/null || true
cleanup() {
	nvidia-smi -rgc 2>/dev/null || true
}
trap cleanup EXIT

echo "t,power_draw_W,enforced_limit_W,gpu_temp_C,cpu_temp_mC,fan1_rpm,fan2_rpm" > "$LOG"

monitor() {
	local start end draw limit gtemp
	start=$(date +%s)
	while [[ ! -f /tmp/gaming-fan-valley-done ]]; do
		end=$(date +%s)
		read -r draw limit gtemp < <(nvidia-smi \
			--query-gpu=power.draw,enforced.power.limit,temperature.gpu \
			--format=csv,noheader,nounits 2>/dev/null | head -1 \
			| awk -F, '{gsub(/ /, "", $1); gsub(/ /, "", $2); gsub(/ /, "", $3); print $1, $2, $3}')
		draw=${draw:-0}
		limit=${limit:-0}
		gtemp=${gtemp:-0}
		echo "$((end - start)),$draw,$limit,$gtemp,$(cat "$HWMON/temp1_input"),$(cat "$HWMON/fan1_input"),$(cat "$HWMON/fan2_input")" >> "$LOG"
		sleep 1
	done
}

rm -f /tmp/gaming-fan-valley-done
monitor &
MON_PID=$!

echo
echo "== Running Unigine Valley (NVIDIA dGPU) =="
echo "targets: peak_draw>=${MIN_DRAW} W, limit>=${MIN_LIMIT} W under load"

RUN_VALLEY=(sudo -u "$VALLEY_USER" -H env DISPLAY="$DISPLAY"
	__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia
	VALLEY_DIR="$VALLEY_DIR" QUALITY=ULTRA WIDTH=1920 HEIGHT=1080
	LOG_FILE="$VALLEY_LOG" bash scripts/run-valley-headless.sh)

if ! "${RUN_VALLEY[@]}"; then
	touch /tmp/gaming-fan-valley-done
	wait "$MON_PID" 2>/dev/null || true
	echo "FAIL: Valley benchmark exited with an error (see $VALLEY_LOG)" >&2
	exit 1
fi

touch /tmp/gaming-fan-valley-done
wait "$MON_PID" 2>/dev/null || true

echo
echo "== Results =="
RESULT=0
awk -F, -v min_draw="$MIN_DRAW" -v min_limit="$MIN_LIMIT" '
NR > 1 {
	d = $2 + 0; l = $3 + 0
	if (d > maxd) maxd = d
	if (l > 0) {
		if (minl == 0 || l < minl) minl = l
		if (l > maxl) maxl = l
		if (d > 35 && l < min_limit) drops++
	}
	n++
}
END {
	printf "samples=%d max_draw=%.1f W limit_range=%.1f-%.1f W\n", n, maxd, minl, maxl
	printf "limit_drops_below_%dW_under_load=%d\n", min_limit, drops + 0
	if (drops > 0) {
		printf "FAIL: power limit dropped below %d W while GPU was loaded\n", min_limit
		exit 1
	}
	if (maxd < min_draw) {
		printf "FAIL: peak draw %.1f W < %d W target (GPU may not have loaded)\n", maxd, min_draw
		exit 2
	}
	printf "PASS: limit held at %.0f W, peak draw %.1f W\n", maxl, maxd
}' "$LOG" || RESULT=$?

echo
echo "Peak draw samples:"
awk -F, 'NR > 1' "$LOG" | sort -t, -k2 -nr | head -8 | while IFS=, read -r t d l g c f1 f2; do
	echo "  t=${t}s draw=${d}W limit=${l}W gpu=${g}C fans=${f1}/${f2}"
done

if awk -F, -v ml="$MIN_LIMIT" 'NR > 1 && $2+0 > 35 && $3+0 > 0 && $3+0 < ml' "$LOG" | grep -q .; then
	echo
	echo "Limit drop events:"
	awk -F, -v ml="$MIN_LIMIT" 'NR > 1 && $2+0 > 35 && $3+0 > 0 && $3+0 < ml {
		printf "  t=%ss draw=%sW limit=%sW gpu=%sC fans=%s/%s\n", $1, $2, $3, $4, $6, $7
	}' "$LOG"
fi

echo
echo "Post-run: enforced limit=$(nvidia-smi --query-gpu=enforced.power.limit --format=csv,noheader,nounits | tr -d ' ') W"
echo "RPMs=$(cat "$HWMON/fan1_input")/$(cat "$HWMON/fan2_input")"
echo "Valley log: $VALLEY_LOG"
echo "Power log:  $LOG"

exit "$RESULT"
