# qvenc 스크립트 공통 설정. 실행 파일이 아니라 source 전용이다.
#
# 모든 빌드 산출물은 $QVENC_BUILD 아래에만 생긴다. 이 디렉터리는 .gitignore 대상이다.

set -euo pipefail

QVENC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QVENC_BUILD="${QVENC_BUILD:-$QVENC_ROOT/build}"

# QEMU
QEMU_TAG="${QEMU_TAG:-}"                       # 비우면 최신 stable 태그를 조회한다
QEMU_GIT="${QEMU_GIT:-https://gitlab.com/qemu-project/qemu.git}"
QEMU_SRC="$QVENC_BUILD/qemu"
QEMU_BUILDDIR="$QEMU_SRC/build"
QEMU_BIN="$QEMU_BUILDDIR/qemu-system-x86_64"

# buildroot
BR_TAG="${BR_TAG:-}"                           # 비우면 최신 stable 태그를 조회한다
BR_GIT="${BR_GIT:-https://gitlab.com/buildroot.org/buildroot.git}"
BR_SRC="$QVENC_BUILD/buildroot"
BR_IMAGES="$BR_SRC/output/images"
BR_KDIR_GLOB="$BR_SRC/output/build/linux-*"

# 장치 모델 파라미터
QVENC_LATENCY_MS="${QVENC_LATENCY_MS:-10}"

# HW 검증용 DMA 영역.
# run.sh 가 커널에 memmap= 로 이 구간을 예약시키고, tests/dma_probe.py 가
# /dev/mem 으로 같은 물리주소를 매핑한다. 드라이버 없이 DMA 를 시험하기 위한 것이다.
QVENC_DMA_BASE="${QVENC_DMA_BASE:-0x40000000}"
QVENC_DMA_SIZE_MB="${QVENC_DMA_SIZE_MB:-16}"

say()  { printf '\033[1;34m[qvenc]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[qvenc]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[qvenc]\033[0m %s\n' "$*" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "필요한 명령이 없다: $1"
}

# 원격 저장소에서 가장 최신 stable 태그를 찾는다.
#   $1 = git URL, $2 = 태그 정규식
latest_tag() {
    local url="$1" re="$2" tag
    tag="$(git ls-remote --tags --refs "$url" \
           | awk -F/ '{print $NF}' \
           | grep -E "$re" \
           | sort -V \
           | tail -1)"
    [ -n "$tag" ] || die "$url 에서 태그를 찾지 못했다 (네트워크 확인)"
    printf '%s' "$tag"
}
