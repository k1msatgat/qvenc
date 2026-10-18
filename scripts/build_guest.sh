#!/usr/bin/env bash
#
# buildroot 로 x86_64 최소 게스트를 만든다.
#
#   scripts/build_guest.sh           # 최신 stable 태그로 빌드
#   scripts/build_guest.sh --deps    # 먼저 apt 의존 패키지 설치 (sudo 필요)
#   BR_TAG=2025.02.3 scripts/build_guest.sh
#
# 포함되는 것: 모듈 로드 가능 커널, lspci(pciutils), busybox devmem, python3,
#              9p(virtio) 지원, 부팅 시 /mnt/qvenc 자동 마운트
# 보존되는 것: out-of-tree 드라이버 빌드용 커널 트리 (build/buildroot/output/build/linux-*)
#
# 산출물: build/buildroot/output/images/{bzImage,rootfs.ext2}
#
# 주의: buildroot 최상위 make 는 -j 를 쓰지 않는다. 병렬화는 BR2_JLEVEL 이 알아서 한다.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

APT_PKGS=(
    git build-essential bc rsync cpio unzip file wget
    python3 libncurses-dev perl
)

install_deps=0
for arg in "$@"; do
    case "$arg" in
        --deps) install_deps=1 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) die "알 수 없는 옵션: $arg" ;;
    esac
done

if [ "$install_deps" = 1 ]; then
    say "apt 의존 패키지 설치"
    sudo apt-get update
    sudo apt-get install -y "${APT_PKGS[@]}"
fi

need git
need make

# WSL2 는 윈도우의 PATH 를 그대로 합쳐 넣는다. 거기엔 "/mnt/c/Program Files/..." 처럼
# 공백이 들어간 항목이 있고, buildroot 는 공백이 있는 PATH 를 아예 거부한다
# ("Your PATH contains spaces, TABs, and/or newline characters").
# 빌드에 필요 없는 항목들이므로 걸러낸다.
sanitize_path() {
    local cleaned="" entry
    local OLD_IFS="$IFS"
    IFS=:
    for entry in $PATH; do
        case "$entry" in
            ''|*[[:space:]]*) continue ;;
        esac
        cleaned="${cleaned:+$cleaned:}$entry"
    done
    IFS="$OLD_IFS"
    printf '%s' "$cleaned"
}

clean_path="$(sanitize_path)"
if [ "$clean_path" != "$PATH" ]; then
    say "PATH 에 공백이 든 항목이 있어 걸러낸다 (WSL2 의 윈도우 PATH 주입)"
    export PATH="$clean_path"
fi
need gcc

if [ -z "$BR_TAG" ]; then
    say "최신 buildroot stable 태그 조회 중..."
    BR_TAG="$(latest_tag "$BR_GIT" '^[0-9]{4}\.[0-9]{2}\.[0-9]+$')"
fi
say "buildroot 태그: $BR_TAG"

mkdir -p "$QVENC_BUILD"

if [ ! -d "$BR_SRC/.git" ]; then
    say "buildroot 소스 받는 중"
    git clone --depth 1 --branch "$BR_TAG" "$BR_GIT" "$BR_SRC"
else
    current="$(git -C "$BR_SRC" describe --tags --exact-match 2>/dev/null || echo unknown)"
    if [ "$current" != "$BR_TAG" ]; then
        say "태그 전환: $current -> $BR_TAG"
        git -C "$BR_SRC" fetch --depth 1 origin "refs/tags/$BR_TAG:refs/tags/$BR_TAG"
        git -C "$BR_SRC" checkout --force "$BR_TAG"
        rm -f "$BR_SRC/.config"
    else
        say "buildroot 소스 이미 $BR_TAG"
    fi
fi

if [ ! -f "$BR_SRC/.config" ]; then
    say "qemu_x86_64_defconfig 적용"
    make -C "$BR_SRC" qemu_x86_64_defconfig
fi

# 설정 추가분을 .config 뒤에 붙이고 olddefconfig 로 정규화한다.
# 같은 심볼이 두 번 나오면 뒤에 오는(= 우리 값) 쪽이 이긴다.
say "qvenc 설정 추가분 머지"
fragment="$QVENC_BUILD/buildroot.fragment.generated"
sed "s|@QVENC_ROOT@|$QVENC_ROOT|g" \
    "$QVENC_ROOT/scripts/guest/buildroot.fragment" > "$fragment"
cat "$fragment" >> "$BR_SRC/.config"
make -C "$BR_SRC" olddefconfig

say "설정 확인"
for sym in BR2_PACKAGE_PCIUTILS BR2_PACKAGE_PYTHON3 BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES; do
    grep -E "^$sym=" "$BR_SRC/.config" || warn "$sym 이 .config 에 없다"
done

say "빌드 시작. 처음이면 30분~1시간 넘게 걸린다 (툴체인부터 만든다)."
make -C "$BR_SRC"

[ -f "$BR_IMAGES/bzImage" ]     || die "bzImage 가 없다"
[ -f "$BR_IMAGES/rootfs.ext2" ] || die "rootfs.ext2 가 없다"

say "완료:"
ls -lh "$BR_IMAGES/bzImage" "$BR_IMAGES/rootfs.ext2"

# out-of-tree 모듈을 빌드하려면 "설정되고 빌드까지 끝난" 커널 트리가 필요하다.
# output/build 에는 linux-<ver> 과 linux-headers-<ver> 이 둘 다 있는데, 후자는
# 헤더만 있는 패키지라 모듈을 빌드할 수 없다. .config 와 Module.symvers 가
# 있는 쪽만 고른다.
kdir=""
for d in $BR_KDIR_GLOB; do
    [ -d "$d" ] || continue
    case "$(basename "$d")" in linux-headers-*) continue ;; esac
    if [ -f "$d/.config" ] && [ -f "$d/Module.symvers" ]; then
        kdir="$d"
    fi
done

if [ -n "$kdir" ]; then
    say "드라이버 out-of-tree 빌드용 커널 트리 (SW팀용):"
    echo "    KDIR=$kdir"
    echo "    CROSS_COMPILE=$BR_SRC/output/host/bin/x86_64-buildroot-linux-gnu-"
else
    warn "모듈 빌드가 가능한 커널 트리를 못 찾았다 ($BR_KDIR_GLOB)"
fi
