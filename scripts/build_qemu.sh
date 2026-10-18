#!/usr/bin/env bash
#
# QEMU 를 소스에서 빌드한다 (x86_64-softmmu 타깃만).
#
#   scripts/build_qemu.sh            # 최신 stable 태그로 빌드
#   scripts/build_qemu.sh --deps     # 먼저 apt 의존 패키지 설치 (sudo 필요)
#   QEMU_TAG=v11.1.2 scripts/build_qemu.sh   # 태그 고정
#
# 산출물: build/qemu/build/qemu-system-x86_64
# qvenc 장치 소스가 있으면 빌드 전에 scripts/apply_qemu.sh 로 트리에 넣는다.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

APT_PKGS=(
    git build-essential ninja-build pkg-config python3 python3-venv python3-pip
    meson flex bison
    libglib2.0-dev libpixman-1-dev zlib1g-dev
    libcap-ng-dev libattr1-dev        # 9p(virtfs) local 백엔드에 필요
)

install_deps=0
for arg in "$@"; do
    case "$arg" in
        --deps) install_deps=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) die "알 수 없는 옵션: $arg" ;;
    esac
done

if [ "$install_deps" = 1 ]; then
    say "apt 의존 패키지 설치"
    sudo apt-get update
    sudo apt-get install -y "${APT_PKGS[@]}"
fi

need git
need python3

missing=()
for c in ninja pkg-config flex bison; do
    command -v "$c" >/dev/null 2>&1 || missing+=("$c")
done
if [ "${#missing[@]}" -gt 0 ]; then
    warn "없는 명령: ${missing[*]}"
    die "먼저 'scripts/build_qemu.sh --deps' 를 실행하거나 직접 설치:
  sudo apt-get install -y ${APT_PKGS[*]}"
fi

if [ -z "$QEMU_TAG" ]; then
    say "최신 QEMU stable 태그 조회 중..."
    QEMU_TAG="$(latest_tag "$QEMU_GIT" '^v[0-9]+\.[0-9]+\.[0-9]+$')"
fi
say "QEMU 태그: $QEMU_TAG"

mkdir -p "$QVENC_BUILD"

if [ ! -d "$QEMU_SRC/.git" ]; then
    say "QEMU 소스 받는 중 (shallow clone, 몇 분 걸린다)"
    git clone --depth 1 --branch "$QEMU_TAG" "$QEMU_GIT" "$QEMU_SRC"
else
    current="$(git -C "$QEMU_SRC" describe --tags --exact-match 2>/dev/null || echo unknown)"
    if [ "$current" != "$QEMU_TAG" ]; then
        say "태그 전환: $current -> $QEMU_TAG"
        git -C "$QEMU_SRC" fetch --depth 1 origin "refs/tags/$QEMU_TAG:refs/tags/$QEMU_TAG"
        git -C "$QEMU_SRC" checkout --force "$QEMU_TAG"
        rm -rf "$QEMU_BUILDDIR"     # 태그가 바뀌면 설정을 다시 한다
    else
        say "QEMU 소스 이미 $QEMU_TAG"
    fi
fi

# qvenc 장치를 트리에 넣는다 (2단계부터 존재)
if [ -f "$QVENC_ROOT/hw/qvenc.c" ]; then
    "$QVENC_ROOT/scripts/apply_qemu.sh"
else
    say "hw/qvenc.c 없음 — 장치 통합 건너뜀 (1단계)"
fi

if [ ! -f "$QEMU_BUILDDIR/build.ninja" ]; then
    say "configure 실행"
    mkdir -p "$QEMU_BUILDDIR"
    (
        cd "$QEMU_BUILDDIR"
        ../configure \
            --target-list=x86_64-softmmu \
            --enable-virtfs \
            --disable-docs \
            --disable-gtk \
            --disable-sdl \
            --disable-werror
    )
else
    say "이미 configure 됨 (다시 하려면 rm -rf $QEMU_BUILDDIR)"
fi

say "빌드 (ninja -j$(nproc))"
ninja -C "$QEMU_BUILDDIR" -j"$(nproc)"

[ -x "$QEMU_BIN" ] || die "빌드는 끝났는데 $QEMU_BIN 이 없다"

say "완료: $QEMU_BIN"
"$QEMU_BIN" --version | head -1

if "$QEMU_BIN" -device help 2>&1 | grep -q '"qvenc"'; then
    say "장치 등록 확인: qvenc  ✅"
else
    warn "장치 목록에 qvenc 가 없다. scripts/apply_qemu.sh 결과와 Kconfig 를 확인할 것."
fi
