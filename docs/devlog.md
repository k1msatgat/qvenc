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

## 2026-10-18 — 2단계: 장치 골격 (HW팀 몫)

### 만든 것
- `docs/regspec.md` rev 1.0 — HW/SW 계약. PCI 식별, 접근 규칙, 레지스터 맵,
  비트 정의, 리셋 값, 에러 코드 표, 전형적 사용 순서, Change log
- `include/qvenc_regs.h` — regspec과 1:1. 헤더를 하나도 include 하지 않는 순수 매크로라
  QEMU 장치·커널 드라이버·유저 공간 SDK가 그대로 같이 쓸 수 있다
- `hw/qvenc.c` — BAR0 MMIO 레지스터 파일만. DMA·IRQ·인코딩 없음
- `scripts/apply_qemu.sh` — `hw/misc/qvenc.c`, `include/hw/misc/qvenc_regs.h` 복사 +
  `hw/misc/meson.build`, `hw/misc/Kconfig` 패치. 여러 번 돌려도 안전
- `tests/mmio_probe.py` — 게스트에서 `resource0`를 mmap해 레지스터를 검증하는 도구

### 설계에서 신경 쓴 것
- **잘못된 접근을 QEMU가 먼저 삼키지 않게 했다.** `MemoryRegionOps.valid`를
  `min_access_size=1`, `unaligned=true`로 열고 `impl`도 같이 넓혔다. 좁게 두면 QEMU가
  접근을 쪼개거나 거부해서 장치가 `ERR_ALIGN`을 보고할 수 없다
- **`ERR_CODE`는 first error wins.** 이미 `STATUS.ERROR`가 서 있으면 덮어쓰지 않는다.
  실제 IP가 그렇고, 그래야 SW가 최초 원인을 본다
- **예약 비트에 1을 쓰면 그 쓰기를 통째로 버린다.** 일부만 적용하면 SW가 더 헷갈린다
- **파라미터는 쓰기 시점에 검사하지 않는다.** 그래서 왕복 테스트가 성립하고,
  범위 검사는 `START` 한 곳에 모인다
- **`START`는 `ERR_UNIMPL`.** 조용한 무시도, 성공한 척(`DONE`)도 하지 않는다

### 확인한 것 (QEMU 11.1.2 소스 대조)
- `class_init` 시그니처가 `(ObjectClass *, const void *)`다. 예전 `void *`가 아니다
- 타입 등록은 `DEFINE_TYPES(...)` 배열 형태
- `PCI_VENDOR_ID_QEMU`는 `0x1234`로 `include/hw/pci/pci.h:60`에 있다
  (`pci_ids.h` 아님). 헤더에는 숫자를 직접 적어 커널·SDK에서도 쓰게 했다
- `PCI_CLASS_MULTIMEDIA_VIDEO`는 `0x0400`, `include/hw/pci/pci_ids.h:49`
- `hw/misc/meson.build`의 등록 형태는
  `system_ss.add(when: 'CONFIG_X', if_true: files('x.c'))`

### 검증
- `include/qvenc_regs.h`: `_Static_assert`로 `QVENC_ID_VALUE == 0x51000100`,
  `QVENC_REG_END == 0x38` 확인
- `tests/mmio_probe.py`의 헤더 파서: 상수 74개 전부 해석, 미해석 0개
- `hw/qvenc.c`: QEMU 헤더를 최소 스텁으로 대체해 호스트에서 컴파일·실행.
  `-Wall -Wextra` 경고 없음, 검사 81개 전부 통과. 확인 항목은 ID/리셋 값,
  RW 왕복과 비트 패턴, 파라미터 무검사 래치, CTRL 비트(START self-clear,
  RESET 우선), RO 쓰기/WO 읽기/미정의 오프셋/정렬 위반, 예약 비트 쓰기 거부,
  first-error-wins, 선택적 ack, `STATUS.IRQ`와 `IRQ_EN` 연동, class_init 배선
- `tests/mmio_probe.py`: `hw/qvenc.c`를 스텁과 함께 공유 라이브러리로 묶고 ctypes 로
  불러, probe 의 `Bar` 자리에 실제 장치 로직을 끼워 끝까지 돌렸다. 60/60 PASS.
  이 과정에서 probe 의 버그를 하나 잡았다 — RO 레지스터 쓰기 후 STATUS 를 바로
  되읽어 비교했는데, STATUS 는 거부된 쓰기가 아니라 `ERROR` 플래그 때문에 값이
  달라진다. 에러를 ack 한 뒤에 비교하도록 고쳤다
- 실제 QEMU 빌드는 이 머신에 glib/pixman 개발 패키지가 없어 하지 않았다

### 남은 것
- 사용자가 빌드 → 부팅 → `lspci` → `mmio_probe.py` 순으로 확인
- 3단계에서 `qvenc_start()`가 DMA + 타이머로, `qvenc_update_irq()`가
  `pci_set_irq()`/`msi_notify()` 호출로 바뀐다. 그때 regspec Change log에 ECO를 남긴다

