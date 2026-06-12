#!/bin/bash
# Validation script for the aorus-laptop driver on the GIGABYTE GAMING A16 CWH.
# Run as root: sudo bash scripts/test-a16-cwh.sh
set -u
cd "$(dirname "$0")/.."

SYS=/sys/devices/platform/aorus_laptop

echo "== Reloading module =="
rmmod aorus_laptop 2>/dev/null
insmod ./aorus-laptop.ko || { echo "insmod failed"; exit 1; }
dmesg | grep aorus_laptop | tail -3

echo
echo "== Read-only sensor checks =="
HWMON=$(ls -d "$SYS"/hwmon/hwmon* | head -1)
echo "CPU temp:  $(cat "$HWMON/temp1_input") m°C"
echo "GPU temp:  $(cat "$HWMON/temp2_input") m°C"
echo "Fan 1 RPM: $(cat "$HWMON/fan1_input")"
echo "Fan 2 RPM: $(cat "$HWMON/fan2_input")"
echo "fan_mode:      $(cat "$SYS/fan_mode")"
echo "charge_mode:   $(cat "$SYS/charge_mode")"
echo "charge_limit:  $(cat "$SYS/charge_limit")"
echo "fan_turbo:         $(cat "$SYS/fan_turbo")"
echo "fan_gaming_boost:  $(cat "$SYS/fan_gaming_boost")"
echo "dynamic_boost:     $(cat "$SYS/dynamic_boost")"

echo
echo "== fan_turbo test (10 seconds) =="
echo 1 > "$SYS/fan_turbo" && echo "write 1: OK"
sleep 10
echo "fan_turbo: $(cat "$SYS/fan_turbo"), RPMs: $(cat "$HWMON/fan1_input") / $(cat "$HWMON/fan2_input")"
echo 0 > "$SYS/fan_turbo" && echo "write 0: OK"

echo
echo "== fan_gaming_boost (requires fan_mode=2) =="
echo 2 > "$SYS/fan_mode"
echo 1 > "$SYS/fan_gaming_boost" && echo "write 1: OK"
sleep 3
echo "fan_gaming_boost: $(cat "$SYS/fan_gaming_boost"), fan_mode: $(cat "$SYS/fan_mode")"
echo 0 > "$SYS/fan_gaming_boost" && echo "write 0: OK"

echo
echo "== Max performance: perf_mode=2 + gpu_boost=1 + dynamic_boost=1 =="
echo 2 > "$SYS/perf_mode"
sleep 1
echo 1 > "$SYS/gpu_boost"
sleep 1
echo 1 > "$SYS/dynamic_boost"
sleep 3
nvidia-smi -q -d POWER | grep -E "Current Power Limit|Max Power Limit" | head -3

echo
echo "Done. Expected: Current Power Limit 70W (vs 55W stock); dynamic boost"
echo "adds up to +15W under GPU load (85W max). Monitor under load with:"
echo "  nvidia-smi --query-gpu=power.draw,power.limit --format=csv -l 1"
