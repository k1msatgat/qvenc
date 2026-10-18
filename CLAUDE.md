# qvenc — QEMU 가상 비디오 인코더 IP와 그 소프트웨어 스택

## 목적
실리콘 이전(pre-silicon) 개발 방식을 재현한다. QEMU 커스텀 PCI 장치가 "코덱 IP 칩" 역할을 하고,
그 위에서 리눅스 커널 드라이버와 유저 공간 SDK를 개발한다. 코덱은 JPEG baseline(MJPEG).

## 역할 분담 (가장 중요)
- Claude = 반도체 설계팀(HW팀). 담당: hw/, core/, include/qvenc_regs.h, docs/, tests/, scripts/.
  QEMU 장치 모델, JPEG 인코더 코어, 레퍼런스 인코더, 검증 도구, 환경 스크립트를 만든다.
- 사용자 = 시스템 소프트웨어팀(SW팀). 담당: driver/, sdk/.
  - Claude는 driver/와 sdk/에 코드를 작성하거나 수정하지 않는다. 사용자가 요청해도
    전체 구현 대신 설명, 힌트, 리뷰로 응한다.
  - 설명할 때는 개념만 나열하지 않는다. 짧은 코드 조각을 보여준 뒤 각 부분을 왜 그렇게 짜는지 풀어서
    설명하고, 커널 API는 인자 하나하나의 의미를 설명한다.
  - 리뷰 요청이 오면 버그, 경쟁 조건, 자원 해제 누락을 우선 지적한다.

## 인터페이스 규칙
- HW와 SW 사이의 유일한 계약은 docs/regspec.md와 include/qvenc_regs.h다.
- 레지스터 동작을 바꿀 때는 두 파일을 먼저 수정하고, docs/regspec.md 하단 "Change log"에
  날짜, 이유, SW 영향을 남긴다. 실제 칩 개발의 ECO 공지처럼 쓴다.
- 장치는 명세에 없는 동작을 하지 않는다. 잘못된 접근(정렬 안 된 주소, 범위 초과 DMA,
  BUSY 중 START)은 STATUS.ERROR와 ERR_CODE로 보고한다. 조용히 무시하지 않는다.

## 환경
- 호스트: Windows + WSL2 Ubuntu. 모든 빌드와 실행은 WSL2 안에서 한다.
- 게스트: buildroot로 만든 최소 리눅스. x86_64를 먼저 하고, 이후 aarch64(virt 머신)를 추가한다.
  게스트와 드라이버는 Linux만 대상으로 한다 (Windows 드라이버는 범위 밖).
- QEMU는 최신 stable 태그를 소스 빌드한다. 장치 소스는 hw/qvenc.c에 두고,
  scripts/apply_qemu.sh가 QEMU 트리(hw/misc/)에 복사하고 meson.build와 Kconfig를 패치한다.
- 장치 구현은 QEMU의 hw/misc/edu.c(교육용 PCI 장치)를 구조 참고로 삼는다.
- PCI ID: vendor 0x1234, device 0x5100 (로컬 전용).
- 드라이버는 buildroot 커널 헤더로 out-of-tree 모듈 빌드. 호스트와 게스트 공유는 9p(virtfs).
- 확실하지 않은 QEMU·커널 API는 추측하지 말고 소스나 문서를 확인한 뒤 쓴다.

## 진행 방식
- 단계 단위로 작업한다. 각 단계 끝에 (1) 만든 것, (2) 사용자가 직접 실행할 검증 명령,
  (3) 기대 출력을 제시하고 멈춘다. 다음 단계는 사용자 지시가 있을 때만 진행한다.
- 단계가 끝날 때마다 커밋하고 태그를 단다 (v1-env, v2-skeleton ...).
- 빌드 산출물(build/, QEMU 트리, buildroot output)은 커밋하지 않는다.

## 문서 자동 갱신 규칙
- 세션을 시작하면 이 파일의 "현재 상태"부터 읽고, 거기서 이어서 작업한다.
- 다음 시점에는 사용자가 따로 말하지 않아도 이 파일을 갱신한다.
  - 단계(또는 하위 단계)를 마쳤을 때: "현재 상태"의 완료 단계, 태그, 검증 결과, 다음 할 일
  - 설계나 방식을 결정했을 때: "결정 기록"에 날짜, 결정, 이유를 한 줄로
  - 환경 문제를 겪고 해결했을 때: "주의사항"에 증상과 해결법을 한 줄로
  - regspec이 바뀌었을 때: "현재 상태"에 변경 요약과 regspec Change log 참조
- driver/와 sdk/의 진행 상황은 사용자가 말해준 내용만 기록한다. 코드를 보고 추측해서 적지 않는다.
- 이 파일은 200줄 이내로 유지한다. 자세한 작업 로그는 docs/devlog.md에 날짜별로 쓰고,
  여기에는 요약만 남긴다. 오래된 "현재 상태" 항목은 devlog로 옮긴다.
- "목적", "역할 분담", "인터페이스 규칙", "환경", "진행 방식", "문서 자동 갱신 규칙" 섹션은
  사용자 동의 없이 수정하지 않는다. 바꿀 필요가 있으면 제안만 한다.
- 문서 갱신은 해당 작업 커밋에 함께 포함한다.

## 단계 계획
1. 환경: QEMU 빌드, buildroot x86_64 게스트 부팅, 9p 공유
2. 장치 골격: BAR0 MMIO, 레지스터 읽기/쓰기 → (SW팀) 드라이버 probe
3. DMA와 IRQ: 인코딩 없이 SRC→DST 복사 장치 → (SW팀) DMA 버퍼, 인터럽트 핸들러, ioctl
4. JPEG 코어: core/의 인코더를 장치와 레퍼런스 CLI가 공유, 바이트 단위 일치 검증
5. (SW팀) SDK libqvenc와 CLI
6. 디스크립터 링, 다중 작업, 장치 처리 지연 모델링 → (SW팀) 파이프라이닝, fps 측정
7. aarch64 게스트 추가, 드라이버 크로스컴파일
8. (선택) 장치 안에 펌웨어 역할의 명령 큐 처리부 추가 — 호스트 → 칩 내 펌웨어 → IP 2계층 구조
9. (선택) 디코더 IP 추가 — 지금 구조를 뒤집으면 재생 경로가 된다

## 현재 상태
- 단계: 3 완료 (DMA와 IRQ) / 태그 `v3-dma`. 앞선 태그: `v1-env`, `v2-skeleton`
- regspec **rev 1.1** 발행 (`ID` = `0x51000101`). 레지스터 맵은 그대로고 동작이 늘었다:
  `CTRL.START`가 실제로 SRC→DST DMA를 수행, `STATUS.BUSY` 실동작, MSI+INTx 인터럽트,
  입력 포맷 I420 확정, `WIDTH`/`HEIGHT` 정렬 8→**16**, `ERR_NO_BUS_MASTER`(0x0A) 신설.
  상세는 docs/regspec.md Change log 참조
- 만든 것: `hw/qvenc.c` rev 1.1, `tests/dma_probe.py`, `tests/qtest_probe.py`,
  `tests/host/`(호스트 단위 테스트), `scripts/test.sh`, `apply_qemu.sh` 마커 기반 패치,
  `run.sh`에 DMA 검증용 예약 영역과 `latency-ms` 추가
- 검증: **3층 전부 통과, 총 249개 검사.** 1~3단계가 실측으로 확인됐다
  - 1층 `tests/host` 89/89 — 레지스터 상태 기계, INTx 토글 횟수
  - 2층 `tests/qtest_probe.py` 55/55 — 실제 QEMU 11.1.2. PCI 열거, BAR 크기 실측,
    실제 DMA(6 KiB·384 KiB), RAM 밖 DMA, MSI capability, guest-error 로그
  - 3층 **게스트 부팅 확인** (커널 6.18.7): `lspci`에 `1234:5100` class `0400`,
    9p `/mnt/qvenc` 자동 마운트, `/proc/iomem`에 `40000000-40ffffff : Reserved`,
    `mmio_probe.py` 64/64, `dma_probe.py` 41/41
- JPEG 인코딩은 아직 없다. rev 1.1의 `START`는 입력을 그대로 복사한다
- 다음 할 일: (SW팀) 드라이버 probe → DMA 버퍼 → 인터럽트 핸들러 /
  (HW팀) 4단계 JPEG 코어 — 사용자 지시 대기

## 결정 기록
- 2026-10-18: 코덱은 JPEG baseline. H.264는 범위가 커서 제외
- 2026-10-18: 호스트 Windows+WSL2, 게스트·드라이버는 Linux만
- 2026-10-18: QEMU/buildroot 버전을 코드에 박지 않고 스크립트가 최신 stable 태그를
  조회한다. 재현이 필요하면 `QEMU_TAG` / `BR_TAG`로 고정
- 2026-10-18: 모든 빌드 산출물은 `build/` 아래로 모은다 (`QVENC_BUILD`로 변경 가능)
- 2026-10-18: 9p 마운트는 게스트 `/etc/init.d/S50qvenc9p` 오버레이로 자동화
- 2026-10-18: `MemoryRegionOps.valid`를 1바이트·비정렬까지 열었다. QEMU가 먼저
  걸러 버리면 장치가 `ERR_ALIGN`으로 보고할 기회가 없기 때문
- 2026-10-18: WIDTH/HEIGHT/QUALITY는 쓰기 시점에 검사하지 않고 `START` 시점에 검사
- 2026-10-18: `ERR_CODE`는 first error wins. 처음 원인을 ack 전까지 보존한다
- 2026-10-18: `RESET`과 `START`가 동시에 오면 `RESET`이 이긴다
- 2026-10-18: 장치 식별 상수는 `qvenc_regs.h` 하나만 쓴다. 검증 도구도 이 헤더를 파싱한다
- 2026-10-18: 최종 산출물은 JPEG 비트스트림(`.jpg` / MJPEG)이다. 디코더와 플레이어는
  범위 밖 — 재생은 기존 도구(ffplay 등)로 한다
- 2026-10-18: 입력 프레임 포맷은 I420(YUV 4:2:0 planar), 길이는 `W*H*3/2`. stride 없음
- 2026-10-18: `WIDTH`/`HEIGHT`를 16의 배수로 제한. 4:2:0의 JPEG MCU가 16×16이라
  패딩·크롭 로직을 빼기 위해서다 (rev 1.0의 8의 배수에서 강화, ECO 발행)
- 2026-10-18: bus master가 꺼진 채 `START`하면 `ERR_NO_BUS_MASTER`로 명시 보고한다.
  드라이버에서 가장 흔한 실수인데 안 잡으면 "아무 일도 안 일어남"으로만 보인다
- 2026-10-18: 작업 파라미터는 `START` 시점에 장치 내부로 래칭. `BUSY` 중 레지스터를
  바꿔도 진행 중 작업은 안 바뀐다. 6단계 파이프라이닝의 전제
- 2026-10-18: 처리 지연은 `latency-ms` 장치 속성으로 모델링(기본 10ms).
  프레임 크기에 비례하는 제대로 된 모델은 6단계
- 2026-10-18: 구현 언어는 **C만** 쓴다. C++는 쓰지 않는다. hw/·core/·driver/는 원래
  선택의 여지가 없고(QEMU와 커널이 C), 유일하게 열려 있던 sdk/도 C로 간다 — 사용자 결정.
  검증 도구는 Python 3, 빌드는 Bash + Meson/Kconfig/Kbuild
- 2026-10-18: DMA 검증은 드라이버 없이 한다. `memmap=`으로 물리 메모리를 예약하고
  `/dev/mem`으로 매핑하는, 실제 칩 bring-up과 같은 방식
- 2026-10-18: 검증을 세 층으로 나눈다 — ① `tests/host`(스텁, QEMU 불필요, 1초)
  ② `tests/qtest_probe.py`(실제 QEMU, 게스트 불필요) ③ 게스트 안의 probe 두 개.
  층마다 잡는 게 달라서 한 층만으로는 부족하다. `scripts/test.sh`가 ①②를 묶어 돌린다
- 2026-10-18: `build_qemu.sh`는 `--disable-tools`로 qemu-img/nbd/io를 빼고 빌드한다.
  `run.sh`가 rootfs.ext2를 raw로 바로 붙여서 이미지 변환 도구가 필요 없다

## 주의사항
- QEMU 빌드에는 glib/pixman 개발 패키지가 필요하다. 없으면 configure가 실패하므로
  `scripts/build_qemu.sh --deps`를 먼저 한 번 돌린다
- buildroot 최상위 `make`에는 `-j`를 붙이지 않는다. 병렬화는 `BR2_JLEVEL`이 처리한다
- 9p `security_model=mapped-xattr`는 xattr를 지원하는 파일시스템이 필요하다.
  저장소가 `/mnt/c` 같은 DrvFs 위에 있으면 `QVENC_9P_SECURITY=none`으로 띄운다
- 드라이버가 붙지 않은 PCI 장치는 메모리 디코딩이 꺼져 있을 수 있다. 그러면 MMIO가
  전부 `0xffffffff`로 보인다. 검증 도구는 sysfs `enable`로 이를 직접 켠다
- MMIO는 ctypes로 접근한다. `mmap` 슬라이싱은 memcpy를 타서 바이트 단위로 쪼개질 수
  있고, 그러면 장치가 `ERR_ALIGN`을 올린다
- **QEMU 11에서 qdev 헤더가 `include/hw/`에서 `include/hw/core/`로 옮겨졌다.**
  `hw/core/qdev-properties.h`가 맞는 경로다. DMA 헬퍼는 `hw/pci/pci_device.h`에 있다
- **`Property` 배열에 종료자를 넣으면 컴파일 에러다.** `device_class_set_props`가
  `ARRAY_SIZE`로 개수를 세고 마지막 항목이 NULL이면 빌드를 실패시킨다
- `class_init` 시그니처는 `(ObjectClass *, const void *)`다. 옛 `void *`가 아니다
- `run.sh`는 게스트 RAM을 2G로 띄우고 `0x40000000`에 16 MiB를 `memmap=`으로 예약한다.
  `dma_probe.py`가 이 구간을 `/dev/mem`으로 매핑해 DMA 버퍼로 쓴다
- **WSL2는 윈도우 PATH를 그대로 합쳐 넣는데 거기엔 `/mnt/c/Program Files/...`처럼 공백이
  든 항목이 있다. buildroot는 공백이 있는 PATH를 거부한다**
  (`Your PATH contains spaces, TABs, and/or newline characters`).
  `build_guest.sh`가 공백 든 항목을 걸러내므로 따로 할 일은 없다
- 장치만 고쳤을 때는 게스트를 띄울 필요 없이 QEMU만 재빌드하고
  `python3 tests/qtest_probe.py`를 돌리는 게 가장 빠르다 (몇 초)
