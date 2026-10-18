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

## 2026-10-18 — 3단계: DMA와 IRQ (HW팀 몫)

### 계약 변경 (regspec rev 1.0 → rev 1.1)
`docs/regspec.md` Change log에 ECO로 남겼다. 요약:

- `ID` `0x51000100` → `0x51000101` (VER_MINOR 증가, 레지스터 맵은 그대로)
- `CTRL.START`가 실동작. rev 1.1의 동작은 **SRC→DST 단순 복사**
- `STATUS.BUSY` 실동작, 인터럽트 실제 발생 (MSI 1벡터 + INTx 레벨)
- 입력 포맷 **I420 (YUV 4:2:0 planar)**, 길이 `W*H*3/2` (3.7절 신설)
- `WIDTH`/`HEIGHT` 정렬 **8 → 16의 배수** (4:2:0 MCU가 16×16)
- `ERR_NO_BUS_MASTER`(0x0A) 신설
- 장치 속성 `latency-ms` 신설 (기본 10)

### 만든 것
- `hw/qvenc.c` rev 1.1 — DMA(64 KiB 청크 분할), 지연 타이머, MSI/INTx, 버스 리셋 훅
- `tests/dma_probe.py` — 드라이버 없이 DMA를 검증한다. `memmap=`으로 예약한 물리
  메모리를 `/dev/mem`으로 매핑해 DMA 버퍼로 쓰는, 실제 bring-up과 같은 방식
- `scripts/apply_qemu.sh` — 마커(`# BEGIN qvenc` ~ `# END qvenc`) 기반 치환으로 변경.
  이전 방식은 블록이 이미 있으면 건너뛰어서 `depends on` 변경이 반영되지 않았다
- `scripts/run.sh` — RAM 2G, `memmap=16M$0x40000000`, `-device qvenc,latency-ms=N`
- `tests/mmio_probe.py` — rev 1.1에 맞춰 갱신. `START`가 더 이상 `ERR_UNIMPL`이 아니라
  bus master 상태에 따라 `ERR_NO_BUS_MASTER` / `ERR_BAD_PARAM`을 낸다

### 설계에서 신경 쓴 것
- **MSI는 edge, INTx는 level.** `irq_asserted`로 직전 상태를 기억해 상태가 바뀔 때만
  움직인다. 매번 `msi_notify()`를 부르면 인터럽트가 폭주한다
- **bus master를 명시적으로 검사한다.** QEMU에서 BME가 꺼져 있으면 `pci_dma_read()`가
  그냥 실패하는데, 그대로 두면 SW에는 원인이 안 보인다. `pci_set_master()` 누락은
  드라이버 최빈 실수라 전용 에러 코드를 붙였다
- **작업 파라미터를 START 시점에 래칭한다.** `BUSY` 중 레지스터를 바꿔도 진행 중
  작업은 안 바뀐다. 6단계 파이프라이닝의 전제가 된다
- **`START`가 이전 `DONE`을 치운다.** 안 그러면 ack 없이 연속 작업을 걸 때 완료
  판정이 섞인다. 반면 `ERROR`는 치우지 않는다 — 에러는 명시적 ack를 요구한다
- **24 MiB 프레임을 통째로 들지 않는다.** 64 KiB 청크로 끊어 옮긴다

### 확인한 것 (QEMU 11.1.2 소스 대조)
- qdev 헤더가 `include/hw/` → `include/hw/core/`로 이동했다. `include/hw` 최상위에
  남은 파일은 `elf_ops.h.inc` 하나뿐이다
- `pci_dma_read`/`pci_dma_write`는 `include/hw/pci/pci_device.h`에 있고 `MemTxResult`를
  돌려준다. `pci.h`가 선언하지 않으므로 직접 include 하는 게 맞다
- `device_class_set_props`는 `ARRAY_SIZE`로 개수를 세고, 배열 마지막 항목이 NULL이면
  `qemu_build_not_reached()`로 **컴파일을 실패시킨다.** 종료자를 넣으면 안 된다
  (`hw/misc/pci-testdev.c`가 같은 형태를 쓴다)
- `device_class_set_legacy_reset`은 `include/hw/core/qdev.h:975`에 있다
- `msi_init(dev, offset, nr_vectors, msi64bit, msi_per_vector_mask, errp)`
- `PCI_COMMAND_MASTER`는 `0x4` (`include/standard-headers/linux/pci_regs.h`)
- `msi_init()`을 쓰므로 Kconfig에 `depends on PCI && MSI_NONBROKEN`이 필요하다 (edu와 동일)

### 검증
- 호스트 스텁 harness를 DMA·타이머·MSI·속성·리셋까지 확장했다. 게스트 RAM은 배열로,
  타이머는 테스트가 직접 만기를 돌리는 방식으로 흉내 낸다
- `hw/qvenc.c`: 검사 **89개 전부 통과**, `-Wall -Wextra` 경고 없음. 확인 항목은
  bus master 거절, 파라미터 검증 5종, `DST_SIZE` 부족, 주소 오버플로, 정상 복사,
  64 KiB 청크를 넘는 384 KiB 전송, RAM 밖 DMA, `BUSY` 중 `START`, 파라미터 래칭,
  INTx 레벨 동작과 전이 횟수, MSI 1회 발생, 이전 `DONE` 제거, `BUSY` 중 `RESET`,
  버스 리셋 경로, 그리고 rev 1.0 레지스터 규칙 회귀
- 같은 장치 로직에 검증 도구를 ctypes로 물려 끝까지 실행:
  `mmio_probe.py` **64/64**, `dma_probe.py` **41/41**
- `apply_qemu.sh`는 가짜 QEMU 트리에 두 번 적용해 블록이 하나만 남는 것을 확인
- 실제 QEMU 빌드는 이 머신에 glib/pixman 개발 패키지가 없어 하지 않았다

### 남은 것
- 사용자가 빌드 → 부팅 → `mmio_probe.py` → `dma_probe.py` 순으로 확인
- 4단계에서 `qvenc_dma_timer()`의 read와 write 사이에 JPEG 인코딩이 들어간다.
  그때 `OUT_LEN`이 입력보다 작아지고 `DST_SIZE` 요구 조건이 완화된다 (ECO 예정)

## 2026-10-18 — 실제 QEMU 빌드와 검증 (1~3단계 통합 확인)

지금까지는 스텁 harness로만 검증했다. 실제로 빌드해서 돌렸다.

### 환경 우회 (이 개발 머신 한정)
이 머신은 `sudo`에 비밀번호가 필요해 apt 설치를 할 수 없었다. root 없이 우회했다:

- `apt-get download`로 .deb를 받아 `dpkg-deb -x`로 로컬 prefix에 풀었다
  (glib/pixman/pcre2/ffi/zlib/mount/blkid/selinux/sepol/uuid/cap-ng/attr의 -dev,
  python3-pip, python3-setuptools, cpio)
- `PKG_CONFIG_SYSROOT_DIR` + `PKG_CONFIG_LIBDIR`로 pkg-config가 그 prefix를 보게 했다
- 런타임 `.so`는 이미 시스템에 있어서, -dev 패키지의 끊어진 심볼릭 링크를
  시스템 경로로 재연결했다
- QEMU configure가 `python3 -m venv` 중 `-Im ensurepip`를 부르는데 isolated 모드라
  `PYTHONPATH`가 안 먹는다. `mkvenv.py`는 `pip`와 `setuptools`가 `find_spec`로
  보이면 ensurepip를 건너뛰므로, 두 패키지를 `PYTHONPATH`로 제공해 통과시켰다
- 일부 `.pc`가 `libdir=/usr/lib`(멀티아크 아님)을 써서 `-lselinux`를 못 찾았다.
  멀티아크 디렉터리의 `.so`를 `usr/lib`에도 링크해 해결

**사용자 머신에서는 이 과정이 전부 불필요하다.** `scripts/build_qemu.sh --deps`
한 번이면 된다. 위 내용은 이 세션의 작업 기록일 뿐이다.

### 고친 것
- `scripts/build_guest.sh`: **WSL2의 윈도우 PATH 주입 대응.** `/mnt/c/Program Files/...`
  처럼 공백이 든 PATH 항목이 있으면 buildroot가
  `Your PATH contains spaces, TABs, and/or newline characters`로 거부한다.
  공백 든 항목을 걸러내도록 했다. 사용자 환경에서도 그대로 발생할 문제다
- `tests/qtest_probe.py` 신규

### 결과
- **QEMU 11.1.2 빌드 성공.** `-device help`에
  `name "qvenc", bus PCI, desc "qvenc virtual JPEG encoder IP"` 등록 확인.
  `-device qvenc,help`에 `latency-ms=<uint32> (default: 10)` 노출 확인
- `apply_qemu.sh`가 진짜 QEMU 트리에서 정상 동작. `MSI_NONBROKEN`(hw/pci/Kconfig:14),
  `TEST_DEVICES`(hw/Kconfig:75) 모두 실재하고 x86이 `imply TEST_DEVICES` 한다
- `hw/qvenc.c`가 **진짜 QEMU 헤더로 컴파일된다.** 스텁이 현실과 어긋나지 않았다
- `tests/qtest_probe.py`: **55/55 PASS**
- 심볼 전수 교차 검증: `qvenc.c`가 쓰는 QEMU 심볼 50개 전부 실재 확인,
  `#include` 9개 전부 존재

### qtest 로 확인한 것
게스트 OS 없이 QEMU 안에서 직접 확인했다. 가상 클럭을 `clock_step`으로 돌리므로
타이머 동작이 결정적이다.

- PCI 열거: vendor:device `1234:5100`, class `0x0400`, revision `0x01`
- BAR0: 전부 1을 써서 되읽은 크기가 정확히 4 KiB, 32비트 non-prefetchable
- 레지스터: `ID` = `0x51000101`, `QUALITY` 리셋 75, 왕복, 쓰기 시 무검사
- 접근 규칙 위반 6종이 전부 올바른 `ERR_CODE`로 보고됨.
  **16비트/64비트 접근이 실제로 장치까지 도달해 `ERR_ALIGN`이 난다** —
  게스트 테스트에서는 CPU가 쪼갤 수 있어 INFO로 남겼던 항목이 여기서는 확정됐다
- bus master: PCI COMMAND로 실제로 껐다 켜며 `ERR_NO_BUS_MASTER` 확인
- **실제 DMA**: 6 KiB와 384 KiB(64 KiB 청크를 넘김) 전송 후 게스트 RAM 바이트 비교 일치.
  5 ms 시점엔 `BUSY`, 15 ms 시점엔 `DONE` — `latency-ms=10`이 정확히 동작
- RAM 밖 주소(508 GiB) DMA가 `ERR_DMA_RANGE`로 보고됨.
  QEMU가 `MEMTX_OK`로 조용히 넘기지 않는다는 것을 실측으로 확인했다
- `BUSY` 중 `START` → `ERR_BUSY`, 원래 작업은 영향 없이 완료
- 파라미터 래칭, `BUSY` 중 `RESET`(타이머 취소 포함) 확인
- MSI capability가 config space offset `0x40`에 있고 64비트 주소, 벡터 1개
- 장치의 guest-error 로그 11줄이 `-d guest_errors`로 실제 출력됨

### 남은 것
- buildroot 게스트 빌드 진행 중. 끝나면 부팅 → `mmio_probe.py` → `dma_probe.py`로
  게스트 환경(sysfs, `/dev/mem`, 9p)까지 확인한다

