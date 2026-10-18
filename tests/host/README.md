# 호스트 단위 테스트 — QEMU 없이 장치 로직 돌리기

```sh
make -C tests/host check
```

`hw/qvenc.c` 를 **가짜 QEMU 헤더**(`stub/`)와 함께 호스트 프로그램으로 컴파일해서
레지스터 상태 기계를 직접 돌린다. 1초면 끝나고 QEMU 빌드가 필요 없다.

## 왜 있나

장치를 고치는 동안 가장 빠른 되먹임 고리다. QEMU 를 재빌드하면 수십 초에서 몇 분이
걸리는데, 레지스터 로직만 건드렸을 때는 그럴 필요가 없다.

그리고 **여기서만 볼 수 있는 것**이 있다. 스텁이 `pci_set_irq()` 호출을 세기 때문에
"INTx 라인이 정확히 두 번 토글됐는가"(올라갈 때 한 번, ack 후 내려갈 때 한 번) 같은 걸
검사할 수 있다. 인터럽트를 중복으로 쏘는 버그는 동작만 봐서는 안 드러난다.

## 어떻게 동작하나

- `test_qvenc.c` 가 `hw/qvenc.c` 를 통째로 `#include` 한다. `static` 함수까지 직접 부르려고.
- `stub/` 에 `qemu/osdep.h`, `hw/pci/pci.h` 같은 헤더를 최소한으로 흉내 낸 것이 있다.
- 게스트 RAM 은 `calloc` 한 배열이고, `pci_dma_read/write` 는 그 배열에 대한 memcpy 다.
  bus master 비트가 꺼져 있으면 실패를 돌려주는 것까지 흉내 낸다.
- 타이머는 자동으로 돌지 않는다. 테스트가 `sim_timer_fire()` 로 직접 만기를 돌린다.
  그래서 "BUSY 인 동안" 과 "완료 후" 를 정확히 겨냥할 수 있다.
- `stub/hw/misc/qvenc_regs.h` 는 사본이 아니라 진짜 `include/qvenc_regs.h` 를 가리킨다.
  계약 헤더는 하나뿐이어야 한다.

## 한계 — 이것만 믿으면 안 된다

**스텁이 진짜 QEMU 와 다르면 거짓 통과가 난다.** 실제로 개발 중에 스텁이
`DeviceClass` 를 `PCIDeviceClass` 의 첫 멤버로 두지 않아 거짓 FAIL 이 난 적이 있다.

그래서 이 테스트는 `tests/qtest_probe.py`(실제 QEMU) 와 짝으로 쓴다.
장치를 고쳤으면 둘 다 돌릴 것:

```sh
make -C tests/host check                       # 1초, QEMU 불필요
ninja -C build/qemu/build qemu-system-x86_64   # 수십 초
python3 tests/qtest_probe.py                   # 몇 초, 실제 QEMU
```

`scripts/test.sh` 가 이 순서를 한 번에 돌려준다.

## QEMU 를 업그레이드했는데 컴파일이 깨진다면

스텁이 낡은 것이다. 진짜 헤더를 보고 맞추면 된다. 지금까지 실제로 바뀐 것들:

- QEMU 11: qdev 헤더가 `include/hw/` → `include/hw/core/` 로 이동
- QEMU 10: `class_init` 시그니처가 `(ObjectClass *, void *)` → `(ObjectClass *, const void *)`
- `Property` 배열에서 종료자(`DEFINE_PROP_END_OF_LIST`)가 제거됨
