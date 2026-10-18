# qvenc 개발 로그

CLAUDE.md "현재 상태"는 요약만 담는다. 날짜별 상세 작업 기록은 여기에 쓴다.

## 2026-10-18 — 1단계: 환경

### 만든 것
- 저장소 골격: `docs/ hw/ core/ include/ driver/ sdk/ tests/ scripts/`
- `.gitignore`: `build/`, `output/`, 커널 모듈 중간 산출물, 인코딩 결과물 제외
- `README.md`: 디렉터리 주인 표, 최초 빌드 순서, 실행법
- `docs/devlog.md`: 이 파일
- `driver/README.md`, `sdk/README.md`: SW팀 소유 표시 (HW팀은 코드를 넣지 않는다)
- `scripts/common.sh`: 경로·로그·최신 태그 조회 공통 함수 (source 전용)
- `scripts/build_qemu.sh`: 최신 stable 태그를 shallow clone 해서 `x86_64-softmmu`만 빌드.
  `--deps`로 apt 의존 설치. `hw/qvenc.c`가 있으면 빌드 전에 `apply_qemu.sh`를 먼저 돌린다
- `scripts/build_guest.sh`: buildroot `qemu_x86_64_defconfig` + qvenc 설정 조각
- `scripts/guest/linux.fragment`: `CONFIG_MODULES`, 9p(virtio), PCI/MSI, `CONFIG_DEVMEM`
- `scripts/guest/buildroot.fragment`: pciutils, python3, libffi, rootfs 512M, 오버레이 경로
- `scripts/guest/overlay/etc/init.d/S50qvenc9p`: 부팅 시 `/mnt/qvenc` 자동 마운트
- `scripts/run.sh`: `-device qvenc` + `-fsdev/-device virtio-9p-pci`로 저장소 공유,
  guest error 로그를 `build/qemu.log`로 분리

### 확인한 것
- QEMU 최신 stable 태그는 `v11.1.2` (조회 시점 기준)
- QEMU `configure`는 알 수 없는 `--enable-X`를 meson 옵션으로 그대로 넘긴다
  (`configure:750`). `virtfs`는 `meson_options.txt:313`의 feature 옵션이므로
  `--enable-virtfs`가 유효하다
- 이 작업 머신에는 glib/pixman 개발 패키지가 없어 실제 QEMU 빌드는 하지 않았다.
  스크립트 문법 검사(`bash -n`)만 통과

### 남은 것
- 사용자가 `build_qemu.sh` / `build_guest.sh`를 실제로 돌려 부팅까지 확인

