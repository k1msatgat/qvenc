#!/usr/bin/env bash
#
# 게스트를 부팅한다. qvenc 장치를 붙이고, 저장소를 9p 로 /mnt/qvenc 에 공유한다.
#
#   scripts/run.sh                 # 부팅
#   scripts/run.sh -s -S           # gdb 대기 상태로 부팅 (추가 인자는 그대로 전달)
#
# 환경 변수: QVENC_MEM, QVENC_LATENCY_MS, QVENC_9P_SECURITY, QVENC_DMA_BASE
#
# 로그인: root (비밀번호 없음)
# 종료:   게스트에서 'poweroff', 또는 Ctrl-a x
# 호스트 공유: /mnt/qvenc  (부팅 시 S50qvenc9p 가 자동 마운트)
#
# QEMU 가 찍는 guest error 로그(잘못된 MMIO 접근 등)는 build/qemu.log 에 쌓인다.
# 시리얼 콘솔과 섞이지 않게 일부러 파일로 뺐다.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

MACHINE="${QVENC_MACHINE:-pc}"
MEM="${QVENC_MEM:-2G}"
SMP="${QVENC_SMP:-2}"
SEC_MODEL="${QVENC_9P_SECURITY:-mapped-xattr}"
QEMU_LOG="$QVENC_BUILD/qemu.log"

[ -x "$QEMU_BIN" ]              || die "QEMU 가 없다: $QEMU_BIN  (scripts/build_qemu.sh 먼저)"
[ -f "$BR_IMAGES/bzImage" ]     || die "커널이 없다: $BR_IMAGES/bzImage  (scripts/build_guest.sh 먼저)"
[ -f "$BR_IMAGES/rootfs.ext2" ] || die "rootfs 가 없다: $BR_IMAGES/rootfs.ext2"

if ! "$QEMU_BIN" -device help 2>&1 | grep -q '"qvenc"'; then
    die "이 QEMU 에 qvenc 장치가 없다. scripts/apply_qemu.sh 실행 후 다시 빌드할 것."
fi

say "machine=$MACHINE mem=$MEM 9p=$SEC_MODEL latency-ms=$QVENC_LATENCY_MS"
say "DMA 검증용 예약 영역: ${QVENC_DMA_SIZE_MB}MiB @ $QVENC_DMA_BASE"
say "guest error 로그: $QEMU_LOG"
say "종료는 게스트에서 poweroff, 또는 Ctrl-a x"

exec "$QEMU_BIN" \
    -M "$MACHINE" \
    -m "$MEM" \
    -smp "$SMP" \
    -nographic \
    -kernel "$BR_IMAGES/bzImage" \
    -drive "file=$BR_IMAGES/rootfs.ext2,if=virtio,format=raw" \
    -append "root=/dev/vda rw rootwait console=ttyS0 memmap=${QVENC_DMA_SIZE_MB}M\$${QVENC_DMA_BASE}" \
    -device "qvenc,latency-ms=$QVENC_LATENCY_MS" \
    -fsdev "local,id=qvenc_fsdev,path=$QVENC_ROOT,security_model=$SEC_MODEL" \
    -device virtio-9p-pci,fsdev=qvenc_fsdev,mount_tag=qvenc \
    -d guest_errors \
    -D "$QEMU_LOG" \
    "$@"
