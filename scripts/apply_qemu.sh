#!/usr/bin/env bash
#
# qvenc 장치를 QEMU 소스 트리에 통합한다.
#
#   hw/qvenc.c            -> $QEMU_SRC/hw/misc/qvenc.c
#   include/qvenc_regs.h  -> $QEMU_SRC/include/hw/misc/qvenc_regs.h
#   hw/misc/meson.build   에 qvenc.c 빌드 규칙 추가
#   hw/misc/Kconfig       에 config QVENC 추가
#
# 여러 번 실행해도 안전하다 (idempotent). 소스가 바뀌면 복사본만 갱신된다.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

[ -d "$QEMU_SRC" ] || die "QEMU 소스가 없다: $QEMU_SRC  (먼저 scripts/build_qemu.sh 실행)"
[ -f "$QEMU_SRC/hw/misc/meson.build" ] || die "QEMU 트리처럼 보이지 않는다: $QEMU_SRC"

say "장치 소스 복사"
install -D -m 0644 "$QVENC_ROOT/hw/qvenc.c"           "$QEMU_SRC/hw/misc/qvenc.c"
install -D -m 0644 "$QVENC_ROOT/include/qvenc_regs.h" "$QEMU_SRC/include/hw/misc/qvenc_regs.h"

meson_file="$QEMU_SRC/hw/misc/meson.build"
meson_line="system_ss.add(when: 'CONFIG_QVENC', if_true: files('qvenc.c'))"

if grep -qF "CONFIG_QVENC" "$meson_file"; then
    say "meson.build: 이미 패치됨"
else
    say "meson.build: qvenc.c 빌드 규칙 추가"
    printf '%s\n' "$meson_line" >> "$meson_file"
fi

kconfig_file="$QEMU_SRC/hw/misc/Kconfig"

if grep -qE '^config QVENC$' "$kconfig_file"; then
    say "Kconfig: 이미 패치됨"
else
    say "Kconfig: config QVENC 추가"
    cat >> "$kconfig_file" <<'KCONF'

config QVENC
    bool
    default y if TEST_DEVICES
    depends on PCI
KCONF
fi

say "완료. 이제 QEMU 를 (재)빌드하면 -device qvenc 를 쓸 수 있다."
