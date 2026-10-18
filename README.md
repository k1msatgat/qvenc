# qvenc

QEMU 커스텀 PCI 장치를 "코덱 IP 칩"으로 쓰고, 그 위에 리눅스 커널 드라이버와
유저 공간 SDK를 올려 보는 pre-silicon 개발 연습 프로젝트. 코덱은 JPEG baseline(MJPEG).

실리콘이 나오기 전에 HW팀과 SW팀이 레지스터 명세 하나만 들고 병렬로 일하는 방식을
그대로 흉내 낸다. 그래서 디렉터리마다 주인이 정해져 있다.

| 디렉터리 | 주인 | 내용 |
|---|---|---|
| `hw/` | HW팀 | QEMU PCI 장치 모델 (`qvenc.c`) |
| `core/` | HW팀 | JPEG 인코더 코어 (장치와 레퍼런스 CLI가 공유) |
| `include/` | HW팀 | `qvenc_regs.h` — HW/SW 공통 레지스터 정의 |
| `docs/` | HW팀 | `regspec.md` (계약), `devlog.md` (작업 로그) |
| `tests/` | HW팀 | 장치 검증 도구 (드라이버 아님) |
| `scripts/` | HW팀 | QEMU·게스트 빌드, 실행, 장치 통합 |
| `driver/` | **SW팀** | 커널 드라이버 |
| `sdk/` | **SW팀** | libqvenc + CLI |

**HW와 SW 사이의 유일한 계약은 [`docs/regspec.md`](docs/regspec.md)와
[`include/qvenc_regs.h`](include/qvenc_regs.h)다.** 레지스터 동작이 바뀌면 regspec 하단
Change log에 ECO 공지처럼 남는다.

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

# 3) 게스트 빌드 — 툴체인부터 만들므로 처음엔 오래 걸린다
./scripts/build_guest.sh        # 30~90분
```

버전을 고정하고 싶으면 `QEMU_TAG=v11.1.2 ./scripts/build_qemu.sh` 처럼 쓴다.
모든 산출물은 `build/` 안에만 생기고 커밋되지 않는다.

## 실행

```sh
./scripts/run.sh
```

- 로그인: `root` (비밀번호 없음)
- 종료: 게스트에서 `poweroff`, 또는 `Ctrl-a x`
- 저장소는 9p로 게스트의 `/mnt/qvenc`에 자동 마운트된다
- 장치의 잘못된 접근 로그(guest errors)는 호스트의 `build/qemu.log`에 쌓인다

## 장치가 붙었는지 확인

게스트 안에서:

```sh
lspci -nn | grep 1234:5100
python3 /mnt/qvenc/tests/mmio_probe.py     # 레지스터 규칙
python3 /mnt/qvenc/tests/dma_probe.py      # DMA 와 인터럽트 상태 비트
```

둘 다 드라이버 없이 도는 HW 검증 도구다. 상수는 `qvenc_regs.h`에서 직접 읽으므로
테스트가 계약에서 따로 놀 수 없다.

- `mmio_probe.py` — sysfs의 `resource0`를 mmap해서 레지스터가 `docs/regspec.md`대로
  동작하는지 본다
- `dma_probe.py` — `run.sh`가 `memmap=`으로 예약해 둔 물리 메모리를 `/dev/mem`으로
  매핑해 DMA 버퍼로 쓴다. 실제 칩 bring-up에서 드라이버보다 먼저 하는 일과 같다

처리 지연을 바꿔서 `BUSY` 경로를 길게 보고 싶으면:

```sh
QVENC_LATENCY_MS=200 ./scripts/run.sh
```

## 장치 소스를 고쳤을 때

```sh
./scripts/apply_qemu.sh     # QEMU 트리에 복사 + meson/Kconfig 패치 (idempotent)
ninja -C build/qemu/build   # 재빌드
```

`build_qemu.sh`는 이 과정을 자동으로 먼저 수행한다.

## 진행 상황

[`CLAUDE.md`](CLAUDE.md)의 "현재 상태"를 본다. 자세한 작업 로그는
[`docs/devlog.md`](docs/devlog.md).
