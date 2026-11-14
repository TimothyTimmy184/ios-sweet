#!/bin/bash
# Pack a flame (Pixel 4) XNU boot image: [CPUS=n] mkboot.sh KERNEL_DIR NAME ["extra boot-args"]
# Based on tools/sunfish/mkboot.sh. Differences for the Pixel 4:
# - BOARD=flame shim (splash at 0x9c400000, 1080x2280 nt37280 cmd-mode panel).
# - Touch and buttons are OFF by default: the Pixel 4's FTM5 is on SPI (spi@890000),
#   not I2C, and its keys are on PM8150. The sweet/sunfish drivers would probe the
#   wrong hardware. TOUCH= overrides.
# - No GPU firmware: the sunfish GPU work is Adreno 618-specific (the Pixel 4 has a 640).
# OSVER/OSPATCH must match the phone (see shim/build.sh).
# NOTE: the kernel's early console (patches 0010/0012) still draws at 0x9c000000,
# which is inside pil_slpi_region on the Pixel 4. Fix that before trusting milestone B.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$R"
K="$1/kernel.development.sweet"; NAME=$2; EXTRA=${3:-}
(cd "$1" && sha256sum -c --quiet SHA256SUMS) && echo "kernel checksum ok"
# Read-only first boot: no ufsrw/rootrw, no persistent log ring on userdata.
B="debug=0xc serial=3 cpus=${CPUS:-1} keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 ${TOUCH:-sweettouch=0 sweetfts=0 sweetkeys=0} sweetlog=${SWEETLOG:-0} $EXTRA"
[ -f shim/out/flame-hi.adt ] || python3 -I tools/mkadt.py shim/out/flame-hi.adt --highmem --flame
python3 -I tools/mkpayload.py "$K" "${ADT:-shim/out/flame-hi.adt}" shim/out/payload-$NAME.bin --cmdline "$B"
rm -f shim/out/shim-boot.img
BOARD=flame bash shim/build.sh "$R/shim/out/payload-$NAME.bin" > /tmp/shim-build.log 2>&1 || { tail -5 /tmp/shim-build.log; exit 1; }
cp shim/out/shim-boot.img shim/out/shim-boot-$NAME.img
python3 -I tools/unpack_bootv2.py shim/out/shim-boot-$NAME.img shim/out/check-$NAME >/dev/null
cmp <(head -c $(stat -c %s shim/out/payload-$NAME.bin) shim/out/check-$NAME/ramdisk) shim/out/payload-$NAME.bin && echo "payload round-trip OK"
ls -la shim/out/payload-$NAME.bin shim/out/shim-boot-$NAME.img
sha256sum shim/out/shim-boot-$NAME.img
echo "boot-args: $B"
