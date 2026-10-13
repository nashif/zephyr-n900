#!/bin/sh
# Load an image into NOLO each time the phone enters NOLO's USB mode.
# Polls sysfs for 0421:0105 and only then starts 0xFFFF, so the flasher
# never talks to the OMAP boot ROM (0421:0106), which can stall it.
# Usage: autoload.sh <zephyr.bin> [count]
img=$1
count=${2:-1000}
nolo_present() {
	for d in /sys/bus/usb/devices/*; do
		[ "$(cat "$d/idVendor" 2>/dev/null)" = 0421 ] &&
		[ "$(cat "$d/idProduct" 2>/dev/null)" = 0105 ] && return 0
	done
	return 1
}
while [ "$count" -gt 0 ]; do
	until nolo_present; do sleep 0.05; done
	echo "=== $(date +%T) NOLO, loading $img"
	0xFFFF -m kernel:"$img" -l -b 2>&1 | grep -E "Done|Booting|rror" | grep -v altinterface
	echo "=== $(date +%T) loaded"
	count=$((count - 1))
	# Wait for NOLO to leave before looking again
	while nolo_present; do sleep 0.2; done
done
