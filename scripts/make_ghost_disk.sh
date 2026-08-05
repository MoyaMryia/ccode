#!/bin/bash
# make_ghost_disk.sh - build the BasicLinux "ghost" disk image (one command).
#
# Layout (MBR, GRUB4DOS, 3 partitions):
#   p1: FAT16  64MB  - grldr + menu.lst + zimage (grub4dos boot)
#   p2: ext2  355MB  - BasicLinux root filesystem (fs.img with tweaked rc)
#   p3: swap   64MB  - activated by /etc/rc (swapon /dev/hda3)
#
# Inputs (env overridable):
#   FS_IMG=      root fs image to clone (default: ~/BasicLinux/fs.img)
#   ZIMAGE=      kernel (default: ~/ccode/zimage)
#   GRUB4DOS=    grub4dos dir with grldr + grldr.mbr
#   OUT=         output disk image (default: vm/bl3-disk.img)
#   DISK_MB=     disk size in MB (default 480)
#
# The fs image is customized in place (a copy under vm/fs7.img):
#   - /etc/rc:   fsck disabled, hda1-swap probe disabled, hda3 swap added,
#                trailing RC-COMPLETE marker
#   - /etc/inittab: askfirst -> respawn (no keypress needed)
set -eu

CC=/home/moyamryia/ccode
FS_IMG=${FS_IMG:-$CC/vm/fs7.img}
BASE_FS=${BASE_FS:-$HOME/BasicLinux/fs.img}
ZIMAGE=${ZIMAGE:-$CC/zimage}
GRUB4DOS=${GRUB4DOS:-$HOME/BasicLinux/grub4dos-0.4.5}
OUT=${OUT:-$CC/vm/bl3-disk.img}
DISK_MB=${DISK_MB:-480}

fail() { echo "make_ghost_disk: $1" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing tool: $1"; }
need sfdisk; need dd; need mkfs.fat; need mcopy; need mkswap; need debugfs

P1_SECTORS=131072      # 64MB
P2_START=$((2048 + P1_SECTORS))   # 133120
SWAP_SECTORS=131072    # 64MB
P2_SECTORS=$((DISK_MB * 2048 - P2_START - SWAP_SECTORS - 1))
P3_START=$((P2_START + P2_SECTORS))
TOTAL=$((DISK_MB * 2048))

echo "== build $OUT (${DISK_MB}MB, p1 FAT@2048, p2 ext2@$P2_START, p3 swap@$P3_START)"

# 1. customize the root fs if needed
if [ ! -f "$FS_IMG" ]; then
    echo "-- customizing root fs from $BASE_FS"
    cp "$BASE_FS" "$FS_IMG"
    chmod 644 "$FS_IMG"
    debugfs -R 'cat /etc/rc' "$FS_IMG" > /tmp/rc_work.sh 2>/dev/null || fail "cannot read rc"
    python3 - "$CC" << 'EOF'
import sys
cc = sys.argv[1]
src = open('/tmp/rc_work.sh').read()
src = src.replace("test $SWAPOFF || mount -t msdos /dev/hda1 /hda1 >/dev/null 2>&1 && [ -e /hda1/baslin/swap.img ] && swapon /hda1/baslin/swap.img",
                  "# hda1 swap probing disabled (no FAT swap on hda1)")
src = src.replace('[ -e /DOS/baslin/swap.img ] && swapon /DOS/baslin/swap.img',
                  '[ -e /DOS/baslin/swap.img ] && swapon /DOS/baslin/swap.img\n[ -e /dev/hda3 ] && swapon /dev/hda3 && echo "swap on /dev/hda3"')
src = src.replace("test $NOFSCK || e2fsck -pf /dev/loop0 2>/dev/null",
                  "echo \"Checking root filesystem (fsck disabled for retro QEMU)\"")
src = src.rstrip('\n') + '\n\necho RC-COMPLETE\n'
open('/tmp/rc_work.sh', 'w').write(src)
EOF
    debugfs -w -R 'rm /etc/rc' "$FS_IMG" >/dev/null 2>&1 || true
    debugfs -w -R 'write /tmp/rc_work.sh /etc/rc' "$FS_IMG" >/dev/null 2>&1 || fail "write rc"
    debugfs -w -R 'sif /etc/rc mode 0100755' "$FS_IMG" >/dev/null 2>&1 || true
    printf '%s\n' '::sysinit:/etc/rc' 'tty1::respawn:-/bin/sh' \
        'tty2::respawn:-/bin/sh' 'tty3::respawn:-/bin/sh' '' \
        '::ctrlaltdel:/sbin/reboot' '::shutdown:/sbin/swapoff -a' \
        '::shutdown:/sbin/umount -a -r 2>/dev/null ' '::restart:/sbin/init' > /tmp/inittab.r
    debugfs -w -R 'rm /etc/inittab' "$FS_IMG" >/dev/null 2>&1 || true
    debugfs -w -R 'write /tmp/inittab.r /etc/inittab' "$FS_IMG" >/dev/null 2>&1 || fail "write inittab"
fi

# 2. fresh disk
dd if=/dev/zero of="$OUT" bs=1M count=$DISK_MB status=none
printf 'start=2048, size=%d, type=6\nstart=%d, size=%d, type=83\nstart=%d, size=%d, type=82\n' \
    "$P1_SECTORS" "$P2_START" "$P2_SECTORS" "$P3_START" "$SWAP_SECTORS" \
    | sfdisk -X dos "$OUT" >/dev/null 2>&1 || fail "sfdisk"

# 3. partition 1: FAT16 boot (grldr + menu.lst + zimage)
dd if="$OUT" of=/tmp/p1.img bs=512 skip=2048 count=$P1_SECTORS status=none
mkfs.fat -F 16 -n BASLIN /tmp/p1.img >/dev/null 2>&1
mcopy -i /tmp/p1.img "$GRUB4DOS/grldr" ::/grldr
printf '%s\n' 'timeout 2' 'default 0' \
    'title BasicLinux 3.5.1 (ghost disk)' \
    '  root (hd0,0)' '  kernel /zimage root=/dev/hda2 rw' > /tmp/menu.lst
mcopy -i /tmp/p1.img /tmp/menu.lst ::/menu.lst
mcopy -i /tmp/p1.img "$ZIMAGE" ::/zimage
dd if=/tmp/p1.img of="$OUT" bs=512 seek=2048 conv=notrunc status=none

# 4. partition 2: ext2 root
FS_SIZE=$(stat -c%s "$FS_IMG")
[ "$FS_SIZE" -le $((P2_SECTORS * 512)) ] || fail "fs image too big for p2"
dd if="$FS_IMG" of="$OUT" bs=512 seek=$P2_START conv=notrunc status=none

# 5. partition 3: swap
dd if="$OUT" of=/tmp/p3.img bs=512 skip=$P3_START count=$SWAP_SECTORS status=none
mkswap /tmp/p3.img >/dev/null 2>&1 || true
dd if=/tmp/p3.img of="$OUT" bs=512 seek=$P3_START conv=notrunc status=none

# 6. GRUB4DOS MBR (18 sectors) then rebuild partition table (grldr.mbr has a
#    template table that would clobber ours)
dd if="$GRUB4DOS/grldr.mbr" of="$OUT" bs=512 count=18 conv=notrunc status=none
printf 'start=2048, size=%d, type=6\nstart=%d, size=%d, type=83\nstart=%d, size=%d, type=82\n' \
    "$P1_SECTORS" "$P2_START" "$P2_SECTORS" "$P3_START" "$SWAP_SECTORS" \
    | sfdisk -X dos "$OUT" >/dev/null 2>&1 || fail "sfdisk rebuild"

rm -f /tmp/p1.img /tmp/p3.img /tmp/rc_work.sh /tmp/menu.lst /tmp/inittab.r
echo "== done: $OUT"
