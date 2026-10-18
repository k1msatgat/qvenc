# qvenc 레지스터 명세 (regspec)

**이 문서와 `include/qvenc_regs.h`가 HW팀과 SW팀 사이의 유일한 계약이다.**
두 파일은 항상 1:1로 대응한다. 한쪽만 바꾸지 않는다.

- 문서 개정: rev 1.0
- 대상 하드웨어 리비전: `ID.VERSION == 0x0100`
- 최종 수정: 2026-10-18

---

## 1. PCI 식별

| 항목 | 값 | 비고 |
|---|---|---|
| Vendor ID | `0x1234` | QEMU 로컬 전용 벤더 ID |
| Device ID | `0x5100` | qvenc |
| Revision ID | `0x01` | |
| Class code | `0x0400` | Multimedia / Video device |
| Interrupt pin | INTA (`0x01`) | 3단계 전까지 장치는 인터럽트를 올리지 않는다 |
| BAR0 | 4 KiB, 32-bit MMIO, non-prefetchable | 레지스터 블록 |

BAR0 외의 BAR는 구현하지 않는다. MSI/MSI-X는 3단계에서 추가한다.

---

## 2. 접근 규칙

이 규칙을 어긴 접근은 **조용히 무시되지 않는다.** 아래 4절의 에러 보고 절차를 따른다.

1. 모든 레지스터는 **32비트 폭**이다. `size != 4`인 접근은 `ERR_ALIGN`이다.
2. 모든 접근은 **4바이트 정렬**이어야 한다. `addr & 0x3 != 0`이면 `ERR_ALIGN`이다.
3. 정의되지 않은 오프셋(0x38 ~ 0xFFF) 접근은 `ERR_BAD_OFFSET`이다.
4. RO 레지스터에 쓰면 `ERR_RO_WRITE`, WO 레지스터를 읽으면 `ERR_WO_READ`다.
5. 규칙 위반 **읽기**는 `0xFFFFFFFF`를 반환한다. 규칙 위반 **쓰기**는 상태를 바꾸지 않는다.
6. 예약 비트(reserved)에는 0을 쓴다. 1을 쓰면 `ERR_RESERVED_BIT`이고 그 쓰기는 통째로 버려진다.
   예약 비트는 읽으면 항상 0이다.

> 구현 메모: QEMU `MemoryRegionOps`에서 `valid.unaligned = true`, `min_access_size = 1`로
> 두어 잘못된 접근도 장치 핸들러까지 도달하게 한다. QEMU가 먼저 걸러 버리면 장치가
> `ERR_CODE`로 보고할 기회가 없기 때문이다.

---

## 3. 레지스터 맵

BAR0 기준 오프셋. 접근 속성: RO = 읽기 전용, RW = 읽기/쓰기, WO = 쓰기 전용.

| 오프셋 | 이름 | 속성 | 리셋 값 | 설명 |
|---|---|---|---|---|
| `0x00` | `ID` | RO | `0x51000100` | 장치 식별자 + 하드웨어 버전 |
| `0x04` | `CTRL` | RW | `0x00000000` | 기동/리셋/인터럽트 enable |
| `0x08` | `STATUS` | RO | `0x00000000` | 동작 상태와 에러 플래그 |
| `0x0C` | `IRQ_ACK` | WO | — | 쓴 비트에 해당하는 상태를 해제 |
| `0x10` | `ERR_CODE` | RO | `0x00000000` | 처음 발생한 에러의 원인 코드 |
| `0x14` | `SRC_LO` | RW | `0x00000000` | 입력 프레임 물리주소 하위 32비트 |
| `0x18` | `SRC_HI` | RW | `0x00000000` | 입력 프레임 물리주소 상위 32비트 |
| `0x1C` | `DST_LO` | RW | `0x00000000` | 출력 버퍼 물리주소 하위 32비트 |
| `0x20` | `DST_HI` | RW | `0x00000000` | 출력 버퍼 물리주소 상위 32비트 |
| `0x24` | `DST_SIZE` | RW | `0x00000000` | 출력 버퍼 크기 (바이트) |
| `0x28` | `OUT_LEN` | RO | `0x00000000` | 직전 작업이 실제로 쓴 바이트 수 |
| `0x2C` | `WIDTH` | RW | `0x00000000` | 프레임 가로 (픽셀) |
| `0x30` | `HEIGHT` | RW | `0x00000000` | 프레임 세로 (픽셀) |
| `0x34` | `QUALITY` | RW | `0x0000004B` | JPEG 품질 1~100 (리셋 값 75) |
| `0x38`~`0xFFF` | — | — | — | 예약. 접근 시 `ERR_BAD_OFFSET` |

`SRC_HI`/`DST_HI`는 32비트 게스트에서 0으로 둔다. 장치는 `{HI, LO}`를 64비트 주소로 합쳐 쓴다.

### 3.1 `ID` (0x00, RO)

| 비트 | 이름 | 값 | 설명 |
|---|---|---|---|
| 31:16 | `MAGIC` | `0x5100` | PCI Device ID와 같다. 드라이버 probe의 1차 확인용 |
| 15:8 | `VER_MAJOR` | `0x01` | 레지스터 계약이 깨지면 증가 |
| 7:0 | `VER_MINOR` | `0x00` | 하위 호환 확장이면 증가 |

드라이버는 `MAGIC`이 다르면 probe를 실패시키고, `VER_MAJOR`가 자신이 아는 값보다
크면 거부해야 한다. `VER_MINOR` 차이는 무시해도 된다.

### 3.2 `CTRL` (0x04, RW)

| 비트 | 이름 | 속성 | 설명 |
|---|---|---|---|
| 0 | `START` | W1S, self-clearing | 1을 쓰면 인코딩 작업 시작. 항상 0으로 읽힌다 |
| 1 | `RESET` | W1S, self-clearing | 1을 쓰면 장치 전체 리셋. 항상 0으로 읽힌다 |
| 2 | `IRQ_EN` | RW | 1이면 작업 완료/에러 시 인터럽트를 올린다. 쓴 값 그대로 읽힌다 |
| 31:3 | reserved | — | 0을 써야 한다. 1을 쓰면 `ERR_RESERVED_BIT` |

- `START`와 `RESET`을 동시에 1로 쓰면 **`RESET`이 이기고 `START`는 무시된다.**
- `BUSY`인 동안 `START`를 쓰면 `ERR_BUSY`다. 진행 중인 작업은 영향받지 않는다.
- `RESET`은 `BUSY` 중에도 받아들인다. 진행 중 작업은 버려지고 모든 레지스터가 리셋 값이 된다.
- **현재 리비전(0x0100)에서 `START`는 `ERR_UNIMPL`을 보고한다.** 인코딩 파이프라인은
  3~4단계에서 구현한다. 아직 없는 기능을 성공한 척하지 않는다.

### 3.3 `STATUS` (0x08, RO)

| 비트 | 이름 | 설명 |
|---|---|---|
| 0 | `BUSY` | 1이면 작업 진행 중. `START`는 이 비트가 0일 때만 받는다 |
| 1 | `DONE` | 작업이 성공으로 끝났다. `IRQ_ACK.DONE`으로 지운다 |
| 2 | `ERROR` | 에러가 발생했다. 원인은 `ERR_CODE`. `IRQ_ACK.ERROR`로 지운다 |
| 3 | `IRQ` | 인터럽트 라인이 활성이다. `IRQ_ACK`로 모든 원인을 지우면 내려간다 |
| 31:4 | reserved | 항상 0 |

`DONE`과 `ERROR`는 **sticky**다. 소프트웨어가 `IRQ_ACK`로 지우기 전까지 유지된다.

### 3.4 `IRQ_ACK` (0x0C, WO)

쓴 비트에 1이 있으면 해당 상태를 해제한다. 0인 비트는 아무 영향이 없다.

| 비트 | 이름 | 지우는 것 |
|---|---|---|
| 0 | `ACK_DONE` | `STATUS.DONE` |
| 1 | `ACK_ERROR` | `STATUS.ERROR`, 그리고 `ERR_CODE`를 `ERR_NONE`으로 되돌린다 |
| 31:2 | reserved | 0을 써야 한다. 1을 쓰면 `ERR_RESERVED_BIT` |

`STATUS.DONE`과 `STATUS.ERROR`가 모두 0이 되면 `STATUS.IRQ`도 함께 내려간다.
이 레지스터를 **읽으면** `ERR_WO_READ`이고 `0xFFFFFFFF`가 반환된다.

### 3.5 `ERR_CODE` (0x10, RO)

4절 참고. `STATUS.ERROR`가 1인 동안에는 값이 바뀌지 않는다(first error wins).

### 3.6 `WIDTH` / `HEIGHT` / `QUALITY` (0x2C / 0x30 / 0x34, RW)

**쓰는 시점에는 값을 검사하지 않는다.** 쓴 값이 그대로 읽힌다. 유효성은 `START` 시점에
한 번에 검사하고, 어긋나면 `ERR_BAD_PARAM`이다.

| 레지스터 | 유효 범위 (START 시점 검사) |
|---|---|
| `WIDTH` | 16 ~ 4096, 8의 배수 (JPEG MCU 경계) |
| `HEIGHT` | 16 ~ 4096, 8의 배수 |
| `QUALITY` | 1 ~ 100 |

> 왜 쓰기 시점에 안 자르는가: 실제 IP도 보통 레지스터는 그냥 래치하고 기동 시점에
> 한 번 검사한다. 값을 조용히 클램프하면 소프트웨어가 자기가 뭘 잘못 썼는지 모른다.

---

## 4. 에러 보고

1. 에러가 생기면 장치는 `STATUS.ERROR = 1`로 올리고 `ERR_CODE`에 원인을 넣는다.
2. `STATUS.ERROR`가 이미 1이면 `ERR_CODE`는 **덮어쓰지 않는다.** 처음 원인이 보존된다.
3. `CTRL.IRQ_EN`이 1이면 `STATUS.IRQ`도 올라간다 (3단계부터 실제 인터럽트 발생).
4. 소프트웨어는 `IRQ_ACK.ACK_ERROR`를 써서 `STATUS.ERROR`와 `ERR_CODE`를 함께 지운다.
5. 에러는 장치를 멈추지 않는다. 레지스터 접근은 계속 동작한다.

### 4.1 에러 코드 표

| 코드 | 이름 | 발생 조건 | rev 1.0 구현 |
|---|---|---|---|
| `0x00` | `ERR_NONE` | 에러 없음 | ✅ |
| `0x01` | `ERR_ALIGN` | `size != 4` 이거나 주소가 4바이트 정렬이 아님 | ✅ |
| `0x02` | `ERR_RO_WRITE` | 읽기 전용 레지스터에 쓰기 | ✅ |
| `0x03` | `ERR_WO_READ` | 쓰기 전용 레지스터를 읽기 | ✅ |
| `0x04` | `ERR_BAD_OFFSET` | BAR0 안의 정의되지 않은 오프셋 접근 | ✅ |
| `0x05` | `ERR_BUSY` | `STATUS.BUSY`가 1인데 `CTRL.START` | ✅ (BUSY가 될 일이 아직 없음) |
| `0x06` | `ERR_RESERVED_BIT` | 예약 비트에 1을 씀 | ✅ |
| `0x07` | `ERR_BAD_PARAM` | `START` 시점 `WIDTH`/`HEIGHT`/`QUALITY`가 범위 밖 | 3단계 |
| `0x08` | `ERR_DMA_RANGE` | SRC/DST DMA가 게스트 RAM 범위를 벗어남 | 3단계 |
| `0x09` | `ERR_DST_SMALL` | 출력이 `DST_SIZE`보다 큼 | 4단계 |
| `0x7F` | `ERR_UNIMPL` | 이 리비전이 구현하지 않은 기능 요청 | ✅ (`START`가 여기 해당) |

`0x0A`~`0x7E`는 예약이다. 소프트웨어는 모르는 코드를 만나면 "알 수 없는 장치 에러"로
처리하고, 숫자를 그대로 로그에 남겨야 한다.

---

## 5. 전형적인 사용 순서 (3단계 이후 기준, 참고용)

```
1. ID 읽기 → MAGIC/VER 확인
2. CTRL.RESET = 1
3. WIDTH / HEIGHT / QUALITY 설정
4. SRC_LO/HI = 입력 DMA 주소, DST_LO/HI = 출력 DMA 주소, DST_SIZE = 출력 버퍼 크기
5. CTRL.IRQ_EN = 1
6. CTRL.START = 1          → STATUS.BUSY = 1
7. 인터럽트 대기
8. STATUS 읽기
     ERROR=1 → ERR_CODE 읽고 처리
     DONE=1  → OUT_LEN 읽고 결과 사용
9. IRQ_ACK에 해당 비트 쓰기
```

rev 1.0에서는 6번에서 `ERR_UNIMPL`이 난다. 2단계 드라이버는 1~5번과 8~9번까지만 쓴다.

---

## Change log

실제 칩 개발의 ECO 공지처럼 쓴다. 날짜 / 이유 / SW 영향.

### 2026-10-18 — rev 1.0 최초 발행
- **변경**: 레지스터 맵 전체 신규 정의. `ID`, `CTRL`, `STATUS`, `IRQ_ACK`, `ERR_CODE`,
  `SRC_LO/HI`, `DST_LO/HI`, `DST_SIZE`, `OUT_LEN`, `WIDTH`, `HEIGHT`, `QUALITY`.
  에러 코드 표 `0x00`~`0x09`, `0x7F`.
- **이유**: 2단계(장치 골격) 착수. HW/SW 계약을 먼저 고정해야 양쪽이 병렬로 움직인다.
- **SW 영향**: 신규 — 영향받는 기존 코드 없음. 드라이버 probe는 `ID`만 읽으면 된다.
  `CTRL.START`는 아직 `ERR_UNIMPL`을 돌려주므로 probe 경로에서 호출하지 말 것.
