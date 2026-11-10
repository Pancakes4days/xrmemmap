#!/usr/bin/env bash
# verify_platform.sh — Week 1 read-only platform checks for xrmemmap.
#
# Bundles the handoff's "immediate priorities" verification steps that need the
# Quest connected. Nothing here modifies the device; it only reads state. Run
# it once the headset is awake and `adb devices` lists 1PASH9AMUN9435.
#
#   bash scripts/verify_platform.sh
#
# Override the adb path or device serial if they differ:
#   ADB=/path/to/adb.exe SERIAL=1PASH9AMUN9435 bash scripts/verify_platform.sh

set -u

ADB="${ADB:-/c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe}"
SERIAL="${SERIAL:-}"
A=( "$ADB" )
[ -n "$SERIAL" ] && A+=( -s "$SERIAL" )

pass=0; warn=0
ok()   { echo "  [OK]   $*"; }
note() { echo "  [NOTE] $*"; warn=$((warn+1)); }

echo "== xrmemmap platform verification =="
if [ ! -x "$ADB" ]; then
  echo "adb not found/executable at: $ADB" >&2
  echo "set ADB=/path/to/adb.exe and retry" >&2
  exit 2
fi

echo
echo "-- device reachable --"
devs="$("${A[@]}" devices | awk 'NR>1 && $2=="device"{print $1}')"
if [ -z "$devs" ]; then
  echo "  No device in 'device' state. Wake the Quest (proximity sensor) and"
  echo "  re-check: $ADB devices" >&2
  exit 1
fi
ok "device(s): $devs"

echo
echo "-- bootloader unlocked (expect 0) --"
locked="$("${A[@]}" shell getprop ro.boot.flash.locked | tr -d '\r')"
[ "$locked" = "0" ] && ok "ro.boot.flash.locked=0" || note "ro.boot.flash.locked=$locked (expected 0)"

echo
echo "-- root available (Magisk / su) --"
whoami_su="$("${A[@]}" shell su -c id 2>&1 | tr -d '\r')"
if echo "$whoami_su" | grep -q 'uid=0'; then
  ok "su works: $whoami_su"
else
  note "su did not return uid=0 — pagemap PFNs will be hidden until Magisk is installed"
  echo "         got: $whoami_su"
fi

echo
echo "-- ftrace present --"
if "${A[@]}" shell "test -d /sys/kernel/debug/tracing" >/dev/null 2>&1; then
  ok "/sys/kernel/debug/tracing exists"
else
  note "tracing dir missing (may need: su -c 'mount -t debugfs none /sys/kernel/debug')"
fi

echo
echo "-- Priority #7: kgsl_iommu_map ftrace-visible --"
# tracefs is root-only on this device; the shell user gets EACCES. Gate the
# check on root so an absent `su` reports "needs root", not a false negative.
if echo "$whoami_su" | grep -q 'uid=0'; then
  kgsl="$("${A[@]}" shell su -c 'grep -i kgsl_iommu /sys/kernel/debug/tracing/available_filter_functions 2>/dev/null' | tr -d '\r')"
  if [ -n "$kgsl" ]; then
    ok "kgsl_iommu functions traceable:"
    echo "$kgsl" | sed 's/^/           /'
  else
    note "root OK but no kgsl_iommu entries — Component 2 (Weeks 2-3) design may need to change"
  fi
else
  note "skipped: tracefs is root-only here; install Magisk, then re-run (Priority #7)"
fi

echo
echo "-- KGSL device node present --"
"${A[@]}" shell "ls -l /dev/kgsl-3d0" 2>&1 | sed 's/^/  /'

echo
echo "== summary: ${warn} note(s) to review =="
echo "Run the Vulkan extension/KGSL-open checks after the test program builds."
