# qvenc 레지스터 명세 (regspec)

**이 문서와 `include/qvenc_regs.h`가 HW팀과 SW팀 사이의 유일한 계약이다.**
두 파일은 항상 1:1로 대응한다. 한쪽만 바꾸지 않는다.

- 문서 개정: rev 1.1
- 대상 하드웨어 리비전: `ID.VERSION == 0x0101`
- 최종 수정: 2026-10-18

---

## 1. PCI 식별

| 항목 | 값 | 비고 |
|---|---|---|
| Vendor ID | `0x1234` | QEMU 로컬 전용 벤더 ID |
| Device ID | `0x5100` | qvenc |
| Revision ID | `0x01` | |
| Class code | `0x0400` | Multimedia / Video device |
| Interrupt pin | INTA (`0x01`) | MSI를 안 쓰면 이 라인으로 올라온다 |
| MSI | 벡터 1개, 64비트 주소 | rev 1.1부터 지원 |
| BAR0 | 4 KiB, 32-bit MMIO, non-prefetchable | 레지스터 블록 |

BAR0 외의 BAR는 구현하지 않는다.

### 1.1 Bus master

**장치가 DMA를 하려면 소프트웨어가 PCI COMMAND 레지스터의 Bus Master Enable 비트를
먼저 켜야 한다** (리눅스에서는 `pci_set_master()`). 이걸 안 켠 채로 `START`를 쓰면
장치는 DMA를 시도하지 않고 `ERR_NO_BUS_MASTER`를 보고한다.

조용히 실패하지 않게 일부러 명시적으로 검사한다. 이 항목을 빠뜨리는 건 드라이버에서
가장 흔한 실수이고, 검사가 없으면 "아무 일도 안 일어남"으로만 보여서 추적이 어렵다.

---

## 2. 접근 규칙

이 규칙을 어긴 접근은 **조용히 무시되지 않는다.** 아래 5절의 에러 보고 절차를 따른다.

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
| `0x00` | `ID` | RO | `0x51000101` | 장치 식별자 + 하드웨어 버전 |
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
| 7:0 | `VER_MINOR` | `0x01` | 하위 호환 확장이면 증가 |

드라이버는 `MAGIC`이 다르면 probe를 실패시키고, `VER_MAJOR`가 자신이 아는 값보다
크면 거부해야 한다. `VER_MINOR` 차이는 무시해도 된다.

### 3.2 `CTRL` (0x04, RW)

| 비트 | 이름 | 속성 | 설명 |
|---|---|---|---|
| 0 | `START` | W1S, self-clearing | 1을 쓰면 작업 시작. 항상 0으로 읽힌다 |
| 1 | `RESET` | W1S, self-clearing | 1을 쓰면 장치 전체 리셋. 항상 0으로 읽힌다 |
| 2 | `IRQ_EN` | RW | 1이면 작업 완료/에러 시 인터럽트를 올린다. 쓴 값 그대로 읽힌다 |
| 31:3 | reserved | — | 0을 써야 한다. 1을 쓰면 `ERR_RESERVED_BIT` |

- `START`와 `RESET`을 동시에 1로 쓰면 **`RESET`이 이기고 `START`는 무시된다.**
- `BUSY`인 동안 `START`를 쓰면 `ERR_BUSY`다. 진행 중인 작업은 영향받지 않는다.
- `RESET`은 `BUSY` 중에도 받아들인다. 진행 중 작업은 버려지고(DMA는 중단되며 이미 쓴
  내용은 그대로 남는다) 모든 레지스터가 리셋 값이 된다. 인터럽트 라인도 내려간다.

### 3.3 `STATUS` (0x08, RO)

| 비트 | 이름 | 설명 |
|---|---|---|
| 0 | `BUSY` | 1이면 작업 진행 중. `START`는 이 비트가 0일 때만 받는다 |
| 1 | `DONE` | 작업이 성공으로 끝났다. `IRQ_ACK.DONE`으로 지운다 |
| 2 | `ERROR` | 에러가 발생했다. 원인은 `ERR_CODE`. `IRQ_ACK.ERROR`로 지운다 |
| 3 | `IRQ` | 인터럽트 원인이 살아 있고 `IRQ_EN`이 켜져 있다 |
| 31:4 | reserved | 항상 0 |

`DONE`과 `ERROR`는 **sticky**다. 소프트웨어가 `IRQ_ACK`로 지우기 전까지 유지된다.
`BUSY`는 장치가 관리하며 소프트웨어가 지울 수 없다.

### 3.4 `IRQ_ACK` (0x0C, WO)

쓴 비트에 1이 있으면 해당 상태를 해제한다. 0인 비트는 아무 영향이 없다.

| 비트 | 이름 | 지우는 것 |
|---|---|---|
| 0 | `ACK_DONE` | `STATUS.DONE` |
| 1 | `ACK_ERROR` | `STATUS.ERROR`, 그리고 `ERR_CODE`를 `ERR_NONE`으로 되돌린다 |
| 31:2 | reserved | 0을 써야 한다. 1을 쓰면 `ERR_RESERVED_BIT` |

`STATUS.DONE`과 `STATUS.ERROR`가 모두 0이 되면 `STATUS.IRQ`도 내려가고,
INTx 라인도 de-assert 된다. 이 레지스터를 **읽으면** `ERR_WO_READ`이고
`0xFFFFFFFF`가 반환된다.

### 3.5 `ERR_CODE` (0x10, RO)

5절 참고. `STATUS.ERROR`가 1인 동안에는 값이 바뀌지 않는다(first error wins).

### 3.6 `WIDTH` / `HEIGHT` / `QUALITY` (0x2C / 0x30 / 0x34, RW)

**쓰는 시점에는 값을 검사하지 않는다.** 쓴 값이 그대로 읽힌다. 유효성은 `START` 시점에
한 번에 검사하고, 어긋나면 `ERR_BAD_PARAM`이다.

| 레지스터 | 유효 범위 (START 시점 검사) |
|---|---|
| `WIDTH` | 16 ~ 4096, **16의 배수** |
| `HEIGHT` | 16 ~ 4096, **16의 배수** |
| `QUALITY` | 1 ~ 100 |

> 왜 쓰기 시점에 안 자르는가: 실제 IP도 보통 레지스터는 그냥 래치하고 기동 시점에
> 한 번 검사한다. 값을 조용히 클램프하면 소프트웨어가 자기가 뭘 잘못 썼는지 모른다.

> 왜 16의 배수인가: 4:2:0 크로마 서브샘플링에서 JPEG MCU가 16×16 픽셀이다.
> 임의 크기를 지원하려면 MCU 경계까지 패딩하고 디코더가 잘라내야 하는데,
> 교육 목적상 그 로직을 빼고 단순하게 간다.

### 3.7 프레임 포맷과 전송 길이

`SRC`가 가리키는 입력 프레임은 **I420 (YUV 4:2:0 planar)** 이다.

```
오프셋 0                    W*H              W*H*5/4         W*H*3/2
  +------------------------+----------------+----------------+
  |      Y plane (W×H)     |  Cb (W/2×H/2)  |  Cr (W/2×H/2)  |
  +------------------------+----------------+----------------+
```

입력 프레임 크기는 항상 다음과 같다. 플레인 사이 패딩이나 stride는 없다.

```
FRAME_BYTES(W, H) = W * H * 3 / 2
```

`WIDTH`/`HEIGHT`가 16의 배수이므로 이 값은 항상 정수다. 최대 크기는
4096×4096 → 24 MiB다.

### 3.8 `START` 동작 (rev 1.1)

```
START
 ├─ BUSY?                      → ERR_BUSY,          작업 안 함
 ├─ Bus Master Enable 꺼짐?     → ERR_NO_BUS_MASTER, 작업 안 함
 ├─ WIDTH/HEIGHT/QUALITY 범위 밖 → ERR_BAD_PARAM,     작업 안 함
 ├─ DST_SIZE < 출력 예상 크기    → ERR_DST_SMALL,     작업 안 함
 ├─ SRC/DST 주소 + 길이가 64비트 오버플로 → ERR_DMA_RANGE, 작업 안 함
 └─ 통과 → BUSY=1, DONE=0, 처리 지연 후 DMA 수행
            ├─ DMA 실패 → ERR_DMA_RANGE, OUT_LEN=0, BUSY=0
            └─ 성공     → OUT_LEN=쓴 바이트 수, DONE=1, BUSY=0
          그리고 IRQ_EN이면 인터럽트
```

**rev 1.1에서 장치가 하는 일은 SRC→DST 단순 복사다.** JPEG 인코딩은 4단계에서
들어간다. 따라서 지금은 출력 예상 크기 = 입력 프레임 크기 = `FRAME_BYTES(W,H)`이고,
성공하면 `OUT_LEN`도 같은 값이다. 4단계부터 `OUT_LEN`은 압축된 실제 길이가 되며
입력보다 작아진다.

에러로 끝나면 `OUT_LEN`은 0이고 **`DST` 버퍼 내용은 정의되지 않는다.** DMA가 중간까지
진행된 뒤 실패했을 수 있다.

추가로 `START`가 받아들여지는 순간:

- **`STATUS.DONE`이 지워진다.** 새 작업이 이전 완료 상태를 대체한다. 따라서 직전 작업의
  `DONE`을 ack 하지 않고 바로 다음 `START`를 걸어도 완료 판정이 섞이지 않는다.
  반면 `STATUS.ERROR`는 지워지지 않는다 — 에러는 소프트웨어가 명시적으로 ack 해야 한다.
- **`OUT_LEN`이 0이 된다.** 작업이 끝나야 값이 채워진다.
- **작업 파라미터(`SRC`, `DST`, `WIDTH`, `HEIGHT`, `QUALITY`)가 장치 내부로 복사된다.**
  `BUSY`인 동안 소프트웨어가 이 레지스터들을 바꿔도 **진행 중인 작업은 영향받지 않는다.**
  다음 `START`부터 새 값이 쓰인다. 6단계에서 디스크립터 링이 들어오면 이 성질이
  파이프라이닝의 전제가 된다.

### 3.9 장치 속성 (QEMU 커맨드라인)

레지스터가 아니라 장치 생성 시 지정하는 모델 파라미터다.

| 속성 | 기본값 | 설명 |
|---|---|---|
| `latency-ms` | 10 | `START`부터 완료까지의 모델링된 처리 지연 (밀리초) |

```sh
-device qvenc,latency-ms=50
```

이 지연이 있어야 `BUSY`가 관측 가능해지고 소프트웨어가 대기 경로를 제대로 짜게 된다.
프레임 크기에 비례하는 제대로 된 지연 모델은 6단계에서 넣는다.

---

## 4. 인터럽트

- `CTRL.IRQ_EN`이 1이고 `STATUS.DONE` 또는 `STATUS.ERROR`가 서면 인터럽트를 올린다.
- 장치는 **MSI(벡터 1개)** 와 **INTx(INTA)** 를 모두 지원한다. 소프트웨어가 MSI를
  켜 두었으면 MSI로, 아니면 INTA 라인으로 간다. 리눅스에서는
  `pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES)`를 쓰면 양쪽이 투명하게 처리된다.
- INTx는 **레벨 트리거**다. `IRQ_ACK`로 모든 원인을 지우기 전까지 라인이 내려가지 않는다.
  핸들러에서 ack를 빼먹으면 인터럽트 폭풍이 난다.
- 인터럽트 핸들러는 `STATUS`를 읽어 원인을 판별하고, `ERROR`면 `ERR_CODE`를 읽은 뒤,
  해당 비트를 `IRQ_ACK`에 써서 지워야 한다.
- `CTRL.IRQ_EN`을 0으로 내리면 라인도 함께 내려간다. 원인 비트(`DONE`/`ERROR`)는 남는다.

---

## 5. 에러 보고

1. 에러가 생기면 장치는 `STATUS.ERROR = 1`로 올리고 `ERR_CODE`에 원인을 넣는다.
2. `STATUS.ERROR`가 이미 1이면 `ERR_CODE`는 **덮어쓰지 않는다.** 처음 원인이 보존된다.
3. `CTRL.IRQ_EN`이 1이면 인터럽트도 올라간다.
4. 소프트웨어는 `IRQ_ACK.ACK_ERROR`를 써서 `STATUS.ERROR`와 `ERR_CODE`를 함께 지운다.
5. 에러는 장치를 멈추지 않는다. 레지스터 접근은 계속 동작한다.

### 5.1 에러 코드 표

| 코드 | 이름 | 발생 조건 | rev 1.1 구현 |
|---|---|---|---|
| `0x00` | `ERR_NONE` | 에러 없음 | ✅ |
| `0x01` | `ERR_ALIGN` | `size != 4` 이거나 주소가 4바이트 정렬이 아님 | ✅ |
| `0x02` | `ERR_RO_WRITE` | 읽기 전용 레지스터에 쓰기 | ✅ |
| `0x03` | `ERR_WO_READ` | 쓰기 전용 레지스터를 읽기 | ✅ |
| `0x04` | `ERR_BAD_OFFSET` | BAR0 안의 정의되지 않은 오프셋 접근 | ✅ |
| `0x05` | `ERR_BUSY` | `STATUS.BUSY`가 1인데 `CTRL.START` | ✅ |
| `0x06` | `ERR_RESERVED_BIT` | 예약 비트에 1을 씀 | ✅ |
| `0x07` | `ERR_BAD_PARAM` | `START` 시점 `WIDTH`/`HEIGHT`/`QUALITY`가 범위 밖 | ✅ |
| `0x08` | `ERR_DMA_RANGE` | DMA 주소/길이가 잘못됨, 또는 접근 실패 | ✅ |
| `0x09` | `ERR_DST_SMALL` | 출력이 `DST_SIZE`보다 큼 | ✅ |
| `0x0A` | `ERR_NO_BUS_MASTER` | Bus Master Enable이 꺼진 채 `START` | ✅ |
| `0x7F` | `ERR_UNIMPL` | 이 리비전이 구현하지 않은 기능 요청 | 현재 발생 안 함 |

`0x0B`~`0x7E`는 예약이다. 소프트웨어는 모르는 코드를 만나면 "알 수 없는 장치 에러"로
처리하고, 숫자를 그대로 로그에 남겨야 한다.

---

## 6. 전형적인 사용 순서

```
 1. ID 읽기 → MAGIC/VER 확인
 2. pci_set_master()            ← 빠뜨리면 ERR_NO_BUS_MASTER
 3. CTRL.RESET = 1
 4. WIDTH / HEIGHT / QUALITY 설정   (16의 배수, 1~100)
 5. SRC_LO/HI = 입력 DMA 주소
    DST_LO/HI = 출력 DMA 주소
    DST_SIZE  = 출력 버퍼 크기
 6. CTRL.IRQ_EN = 1
 7. CTRL.START = 1          → STATUS.BUSY = 1
 8. 인터럽트 대기 (또는 STATUS.BUSY 폴링)
 9. STATUS 읽기
      ERROR=1 → ERR_CODE 읽고 처리
      DONE=1  → OUT_LEN 읽고 DST 버퍼 사용
10. IRQ_ACK 에 해당 비트 쓰기   ← 빼먹으면 INTx 폭풍
```

---

## Change log

### 2026-10-18 — rev 1.1 (3단계: DMA와 IRQ)
- **변경**
  - `ID.VER_MINOR` `0x00` → `0x01`. `ID` 값이 `0x51000100` → `0x51000101`
  - `CTRL.START`가 실제로 동작한다. 더 이상 `ERR_UNIMPL`을 돌려주지 않는다.
    rev 1.1의 동작은 **SRC→DST 단순 복사**이고 JPEG 인코딩은 4단계다
  - `STATUS.BUSY`가 실제로 사용된다. `latency-ms` 동안 1로 유지된다
  - 인터럽트를 실제로 올린다. MSI(벡터 1개)와 INTx(레벨 트리거) 모두 지원
  - 입력 프레임 포맷을 **I420 (YUV 4:2:0 planar)** 로 확정. 전송 길이는
    `FRAME_BYTES(W,H) = W*H*3/2` (3.7절 신설)
  - `WIDTH`/`HEIGHT` 정렬 제약을 **8의 배수 → 16의 배수**로 강화.
    4:2:0에서 JPEG MCU가 16×16이기 때문이다
  - 에러 코드 `0x0A ERR_NO_BUS_MASTER` 신설
  - `ERR_BUSY`, `ERR_BAD_PARAM`, `ERR_DMA_RANGE`, `ERR_DST_SMALL`을 실제로 구현
  - 장치 속성 `latency-ms` 신설 (기본 10, 3.9절)
- **이유**: 3단계 착수. 인코딩 없이 DMA와 인터럽트 경로만 먼저 세워서 SW팀이
  DMA 버퍼·인터럽트 핸들러·ioctl을 짤 수 있게 한다
- **SW 영향**
  - ⚠️ **`pci_set_master()`를 반드시 불러야 한다.** 안 부르면 `START`가
    `ERR_NO_BUS_MASTER`(0x0A)로 실패한다
  - ⚠️ **`WIDTH`/`HEIGHT`가 16의 배수여야 한다.** 8의 배수를 가정한 코드가 있다면
    고쳐야 한다. 단, rev 1.0에서는 `START` 자체가 동작하지 않았으므로 실제로
    깨지는 기존 코드는 없을 것이다
  - ⚠️ **INTx는 레벨 트리거다.** 핸들러에서 `IRQ_ACK`를 반드시 써야 한다
  - `ID` 값이 바뀌었으나 `VER_MAJOR`는 그대로다. `MAGIC`과 `VER_MAJOR`만 보는
    probe는 수정 없이 동작한다
  - 입력 버퍼는 `W*H*3/2` 바이트의 I420이어야 한다. 출력 버퍼는 최소 같은 크기
    (4단계부터는 압축되므로 더 작아도 되지만, 그때 다시 공지한다)


실제 칩 개발의 ECO 공지처럼 쓴다. 날짜 / 이유 / SW 영향.

### 2026-10-18 — rev 1.0 최초 발행
- **변경**: 레지스터 맵 전체 신규 정의. `ID`, `CTRL`, `STATUS`, `IRQ_ACK`, `ERR_CODE`,
  `SRC_LO/HI`, `DST_LO/HI`, `DST_SIZE`, `OUT_LEN`, `WIDTH`, `HEIGHT`, `QUALITY`.
  에러 코드 표 `0x00`~`0x09`, `0x7F`.
- **이유**: 2단계(장치 골격) 착수. HW/SW 계약을 먼저 고정해야 양쪽이 병렬로 움직인다.
- **SW 영향**: 신규 — 영향받는 기존 코드 없음. 드라이버 probe는 `ID`만 읽으면 된다.
  `CTRL.START`는 아직 `ERR_UNIMPL`을 돌려주므로 probe 경로에서 호출하지 말 것.
