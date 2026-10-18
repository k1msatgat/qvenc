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

## 현재 상태
- 단계: 1 완료 (환경) / 태그 `v1-env`
- 만든 것: 저장소 골격, `.gitignore`, README, `docs/devlog.md`,
  `scripts/{common,build_qemu,build_guest,run}.sh`, 게스트 설정 조각과 9p 오버레이
- 검증: 스크립트 문법 검사만 통과. 실제 QEMU/buildroot 빌드는 사용자 실행 대기
- 다음 할 일: 2단계 (장치 골격)

## 결정 기록
- 2026-10-18: 코덱은 JPEG baseline. H.264는 범위가 커서 제외
- 2026-10-18: 호스트 Windows+WSL2, 게스트·드라이버는 Linux만
- 2026-10-18: QEMU/buildroot 버전을 코드에 박지 않고 스크립트가 최신 stable 태그를
  조회한다. 재현이 필요하면 `QEMU_TAG` / `BR_TAG`로 고정
- 2026-10-18: 모든 빌드 산출물은 `build/` 아래로 모은다 (`QVENC_BUILD`로 변경 가능)
- 2026-10-18: 9p 마운트는 게스트 `/etc/init.d/S50qvenc9p` 오버레이로 자동화.
  run.sh의 `mount_tag=qvenc`와 짝을 이룬다

## 주의사항
- QEMU 빌드에는 glib/pixman 개발 패키지가 필요하다. 없으면 configure가 실패하므로
  `scripts/build_qemu.sh --deps`를 먼저 한 번 돌린다
- buildroot 최상위 `make`에는 `-j`를 붙이지 않는다. 병렬화는 `BR2_JLEVEL`이 처리한다
- 9p `security_model=mapped-xattr`는 xattr를 지원하는 파일시스템이 필요하다.
  저장소가 `/mnt/c` 같은 DrvFs 위에 있으면 `QVENC_9P_SECURITY=none`으로 띄운다
