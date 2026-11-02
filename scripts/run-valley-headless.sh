#!/bin/bash
# Run Unigine Valley benchmark without the launcher GUI (benchmark mode).
#
# Uses NVIDIA dGPU via PRIME offload when available. Requires an X display
# (DISPLAY is usually :0 on a desktop session). For machines without a
# physical session, install xvfb and set USE_XVFB=1.
#
# Usage:
#   bash scripts/run-valley-headless.sh
#   VALLEY_DIR=/path/to/Unigine_Valley-1.0 QUALITY=ULTRA bash scripts/run-valley-headless.sh
#   LOG_FILE=/tmp/valley.log bash scripts/run-valley-headless.sh
#
# NVIDIA offload (default on):
#   __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia bash scripts/run-valley-headless.sh
set -euo pipefail

VALLEY_DIR="${VALLEY_DIR:-/home/jose-lopez/Downloads/Unigine_Valley-1.0}"
QUALITY="${QUALITY:-ULTRA}"          # LOW, MEDIUM, HIGH, ULTRA
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FULLSCREEN="${FULLSCREEN:-0}"        # 0 = windowed, 1 = fullscreen
MULTISAMPLE="${MULTISAMPLE:-0}"
LOG_FILE="${LOG_FILE:-}"
USE_XVFB="${USE_XVFB:-0}"
NV_OFFLOAD="${NV_OFFLOAD:-1}"

if [[ ! -d "$VALLEY_DIR/bin" ]]; then
	echo "Valley install not found: $VALLEY_DIR" >&2
	exit 1
fi

ARCH=$(uname -m)
if [[ "$ARCH" == x86_64 ]]; then
	VALLEY_BIN="$VALLEY_DIR/bin/valley_x64"
	VALLEY_LIB="$VALLEY_DIR/bin/x64"
else
	VALLEY_BIN="$VALLEY_DIR/bin/valley_x86"
	VALLEY_LIB="$VALLEY_DIR/bin/x86"
fi

if [[ ! -x "$VALLEY_BIN" ]]; then
	echo "Valley binary not found: $VALLEY_BIN" >&2
	exit 1
fi

export LD_LIBRARY_PATH="$VALLEY_DIR/bin:$VALLEY_LIB:${LD_LIBRARY_PATH:-}"

if [[ "$NV_OFFLOAD" == 1 ]]; then
	export __NV_PRIME_RENDER_OFFLOAD=1
	export __GLX_VENDOR_LIBRARY_NAME=nvidia
fi

VALLEY_ARGS=(
	-project_name Valley
	-video_app opengl
	-data_path ../
	-sound_app null
	-engine_config ../data/valley_1.0.cfg
	-system_script valley/unigine.cpp
	-video_mode -1
	-video_fullscreen "$FULLSCREEN"
	-video_width "$WIDTH"
	-video_height "$HEIGHT"
	-video_multisample "$MULTISAMPLE"
	-extern_define "BENCHMARK,RELEASE,LANGUAGE_EN,QUALITY_${QUALITY}"
)

echo "Valley: $VALLEY_BIN"
echo "Mode:   ${WIDTH}x${HEIGHT} quality=${QUALITY} fullscreen=${FULLSCREEN}"
if [[ -n "${DISPLAY:-}" ]]; then
	echo "Display: $DISPLAY"
else
	echo "Display: (not set)"
fi

cd "$VALLEY_DIR/bin"

if [[ "$USE_XVFB" == 1 ]]; then
	if ! command -v xvfb-run >/dev/null; then
		echo "xvfb-run not found; install xvfb or unset USE_XVFB" >&2
		exit 1
	fi
	echo "Using xvfb-run"
	RUNNER=(xvfb-run -a)
elif [[ -z "${DISPLAY:-}" ]]; then
	echo "DISPLAY is not set. Export DISPLAY=:0 or set USE_XVFB=1." >&2
	exit 1
else
	RUNNER=()
fi

if [[ -n "$LOG_FILE" ]]; then
	echo "Log: $LOG_FILE"
	"${RUNNER[@]}" "$VALLEY_BIN" "${VALLEY_ARGS[@]}" 2>&1 | tee "$LOG_FILE"
else
	"${RUNNER[@]}" "$VALLEY_BIN" "${VALLEY_ARGS[@]}"
fi
