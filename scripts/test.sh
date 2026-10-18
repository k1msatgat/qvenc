#!/usr/bin/env bash
#
# 호스트에서 돌릴 수 있는 검증을 전부 돌린다.
#
#   scripts/test.sh
#
# 1) tests/host      — 가짜 QEMU 헤더로 장치 로직 단위 테스트 (QEMU 불필요, 1초)
# 2) tests/qtest_probe.py — 실제 QEMU 안에서 PCI/MMIO/DMA 검증 (게스트 OS 불필요)
#
# 게스트 안에서 도는 tests/mmio_probe.py 와 tests/dma_probe.py 는 여기서 못 돌린다.
# scripts/run.sh 로 부팅한 뒤 게스트에서 직접 실행할 것.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

fail=0

say "1/2  호스트 단위 테스트 (QEMU 불필요)"
if make -C "$QVENC_ROOT/tests/host" check; then
    say "  통과"
else
    warn "  실패"
    fail=1
fi

echo
if [ -x "$QEMU_BIN" ]; then
    say "2/2  qtest — 실제 QEMU"
    if python3 "$QVENC_ROOT/tests/qtest_probe.py" "$QEMU_BIN"; then
        say "  통과"
    else
        warn "  실패"
        fail=1
    fi
else
    warn "2/2  건너뜀 — QEMU 가 아직 없다 ($QEMU_BIN)"
    warn "     scripts/build_qemu.sh 를 먼저 돌릴 것"
fi

echo
if [ "$fail" = 0 ]; then
    say "호스트 검증 전부 통과"
    say "게스트 검증은 scripts/run.sh 로 부팅한 뒤:"
    echo "    python3 /mnt/qvenc/tests/mmio_probe.py"
    echo "    python3 /mnt/qvenc/tests/dma_probe.py"
else
    die "실패한 검증이 있다"
fi
