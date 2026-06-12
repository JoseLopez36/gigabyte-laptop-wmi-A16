#!/bin/bash
# Install the aorus-laptop driver via DKMS, autoload it at boot, and
# re-apply the GPU TGP unlock on every boot (A16 CWH).
# Run as root: sudo bash scripts/install-dkms.sh
set -eu
cd "$(dirname "$0")/.."

VERSION=0.1.0
SRC=/usr/src/aorus-laptop-$VERSION

echo "== Installing source to $SRC =="
mkdir -p "$SRC"
cp Makefile aorus-laptop.c "$SRC/"
sed "s/@PKGVER@/$VERSION/" dkms.conf > "$SRC/dkms.conf"

echo "== Registering with DKMS =="
dkms remove "aorus-laptop/$VERSION" --all 2>/dev/null || true
dkms add "aorus-laptop/$VERSION"
dkms install "aorus-laptop/$VERSION"

echo "== Enabling autoload at boot =="
install -m 644 aorus-laptop.conf /etc/modules-load.d/aorus-laptop.conf

echo "== Installing TGP unlock service =="
cat > /etc/systemd/system/aorus-tgp-unlock.service <<'EOF'
[Unit]
Description=Unlock GPU TGP on GIGABYTE GAMING A16 CWH
After=systemd-modules-load.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c '\
  for i in $(seq 1 10); do [ -e /sys/devices/platform/aorus_laptop/perf_mode ] && break; sleep 1; done; \
  echo 2 > /sys/devices/platform/aorus_laptop/perf_mode; \
  echo 1 > /sys/devices/platform/aorus_laptop/gpu_boost; \
  echo 1 > /sys/devices/platform/aorus_laptop/dynamic_boost'

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable aorus-tgp-unlock.service

echo "== Loading module now =="
rmmod aorus_laptop 2>/dev/null || true
modprobe aorus-laptop
systemctl start aorus-tgp-unlock.service

echo
dkms status aorus-laptop
echo "perf_mode: $(cat /sys/devices/platform/aorus_laptop/perf_mode)"
nvidia-smi -q -d POWER | grep "Current Power Limit" | head -1
echo "Done. The module will rebuild automatically on kernel updates."
