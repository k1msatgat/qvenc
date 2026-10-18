# qvenc

QEMU 커스텀 PCI 장치를 "코덱 IP 칩"으로 쓰고, 그 위에 리눅스 커널 드라이버와
유저 공간 SDK를 올려 보는 pre-silicon 개발 연습 프로젝트. 코덱은 JPEG baseline(MJPEG).

실리콘이 나오기 전에 HW팀과 SW팀이 레지스터 명세 하나만 들고 병렬로 일하는 방식을
그대로 흉내 낸다. 그래서 디렉터리마다 주인이 정해져 있다.

| 디렉터리 | 주인 | 내용 |
|---|---|---|
| `hw/` | HW팀 | QEMU PCI 장치 모델 (`qvenc.c`) |
| `core/` | HW팀 | JPEG 인코더 코어 (장치와 레퍼런스 CLI가 공유) — 4단계 |
| `include/` | HW팀 | `qvenc_regs.h` — HW/SW 공통 레지스터 정의 |
| `docs/` | HW팀 | `regspec.md` (계약), `devlog.md` (작업 로그) |
| `tests/` | HW팀 | 장치 검증 도구 (드라이버 아님) |
| `scripts/` | HW팀 | QEMU·게스트 빌드, 실행, 장치 통합 |
| `driver/` | **SW팀** | 커널 드라이버 |
| `sdk/` | **SW팀** | libqvenc + CLI |

**HW와 SW 사이의 유일한 계약은 [`docs/regspec.md`](docs/regspec.md)와
[`include/qvenc_regs.h`](include/qvenc_regs.h)다.** 레지스터 동작이 바뀌면 regspec 하단
Change log에 ECO 공지처럼 남는다.

구현 언어는 C만 쓴다. 검증 도구는 Python 3, 빌드는 Bash + Meson/Kconfig/Kbuild.

## 환경

호스트는 Windows + WSL2 Ubuntu. 빌드와 실행은 전부 WSL2 안에서 한다.
게스트는 buildroot로 만든 최소 리눅스(x86_64 먼저, aarch64는 7단계).

## 처음 한 번

```sh
# 1) 의존 패키지 (sudo 필요, 한 번만)
./scripts/build_qemu.sh --deps
./scripts/build_guest.sh --deps

# 2) QEMU 빌드 — 최신 stable 태그를 받아 x86_64-softmmu 만 빌드한다
./scripts/build_qemu.sh         # 10~30분

# 3) 게스트 빌드 — 크로스 툴체인부터 만들므로 처음엔 오래 걸린다
./scripts/build_guest.sh        # 30~90분
```

버전을 고정하고 싶으면 `QEMU_TAG=v11.1.2 ./scripts/build_qemu.sh` 처럼 쓴다.
모든 산출물은 `build/` 안에만 생기고 커밋되지 않는다.

의존 패키지가 빠져 있으면 `build_qemu.sh`가 **무엇이 없는지 짚어서** 알려준 뒤 멈춘다.
configure가 한참 뒤에 알아보기 어려운 메시지로 죽는 것보다 낫다.

## 실행

```sh
./scripts/run.sh
```

- 로그인: `root` (비밀번호 없음)
- 종료: 게스트에서 `poweroff`, 또는 `Ctrl-a x`
- 저장소는 9p로 게스트의 `/mnt/qvenc`에 자동 마운트된다
- 장치의 잘못된 접근 로그(guest errors)는 호스트의 `build/qemu.log`에 쌓인다

환경 변수로 조절한다: `QVENC_MEM`, `QVENC_LATENCY_MS`, `QVENC_9P_SECURITY`.
`BUSY` 경로를 길게 보고 싶으면 `QVENC_LATENCY_MS=200 ./scripts/run.sh`.

## 검증

검증은 세 층이다. 층마다 잡는 것이 다르므로 **한 층만 믿지 않는다.**

| 층 | 명령 | 필요한 것 | 걸리는 시간 | 잡는 것 |
|---|---|---|---|---|
| 1 | `make -C tests/host check` | 없음 | 1초 | 레지스터 상태 기계, 인터럽트 라인 토글 횟수 |
| 2 | `python3 tests/qtest_probe.py` | QEMU | 몇 초 | 실제 PCI 열거, MMIO, 실제 DMA, MSI |
| 3 | 게스트 안에서 실행 | QEMU + 게스트 | 부팅 시간 | sysfs, `/dev/mem`, 9p, 실제 커널 경로 |

1·2층은 한 번에 돌릴 수 있다:

```sh
./scripts/test.sh
```

3층은 게스트를 띄운 뒤 그 안에서:

```sh
lspci -nn | grep 5100
python3 /mnt/qvenc/tests/mmio_probe.py     # 레지스터 규칙
python3 /mnt/qvenc/tests/dma_probe.py      # DMA 와 인터럽트 상태 비트
```

네 도구 모두 상수를 `include/qvenc_regs.h`에서 **직접 읽는다.** 테스트가 계약에서
따로 놀 수 없게 하려는 것이다.

- **`tests/host/`** — `hw/qvenc.c`를 가짜 QEMU 헤더와 함께 호스트 프로그램으로
  컴파일해 돌린다. QEMU 빌드가 필요 없어서 장치를 고치는 동안 되먹임이 가장 빠르다.
  자세한 설명과 한계는 [`tests/host/README.md`](tests/host/README.md).
- **`tests/qtest_probe.py`** — QEMU의 qtest accel로 띄워 소켓으로 MMIO·PCI config
  space·게스트 RAM을 직접 두드린다. 게스트 OS가 없다. 가상 클럭을 직접 돌리므로
  지연 타이머가 결정적으로 재현되고, BAR를 손으로 배치하거나 bus master를 껐다 켜는
  것처럼 게스트 안에서는 번거로운 일도 쉽다.
- **`tests/mmio_probe.py`** — 게스트에서 sysfs의 `resource0`를 mmap해 레지스터가
  `docs/regspec.md`대로 동작하는지 본다.
- **`tests/dma_probe.py`** — `run.sh`가 `memmap=`으로 예약해 둔 물리 메모리를
  `/dev/mem`으로 매핑해 DMA 버퍼로 쓴다. 실제 칩 bring-up에서 드라이버보다 먼저
  하는 일과 같다.

전부 드라이버가 아니다. 드라이버 없이 "칩이 제대로 붙어 있는지"를 먼저 못 박는 용도다.

## 장치 소스를 고쳤을 때

```sh
make -C tests/host check                        # 먼저 1초짜리로 거른다
./scripts/apply_qemu.sh                         # QEMU 트리에 복사 + meson/Kconfig 패치
ninja -C build/qemu/build qemu-system-x86_64    # 재빌드
python3 tests/qtest_probe.py                    # 실제 QEMU 로 확인
```

`build_qemu.sh`는 `apply_qemu.sh`를 자동으로 먼저 수행한다.
`apply_qemu.sh`는 마커 기반이라 여러 번 돌려도 안전하다.

## 진행 상황

[`CLAUDE.md`](CLAUDE.md)의 "현재 상태"를 본다. 자세한 작업 로그는
[`docs/devlog.md`](docs/devlog.md), 레지스터 변경 이력은
[`docs/regspec.md`](docs/regspec.md)의 Change log.
