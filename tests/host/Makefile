# qvenc 장치 로직 호스트 단위 테스트.
#
#   make check
#
# QEMU 를 빌드하지 않고 hw/qvenc.c 의 레지스터 상태 기계를 직접 돌린다.
# 자세한 설명은 README.md 참고.

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wno-unused-parameter -O1 -g
STUBS   := $(wildcard stub/qemu/*.h stub/qom/*.h stub/hw/pci/*.h stub/hw/core/*.h)

test_qvenc: test_qvenc.c ../../hw/qvenc.c ../../include/qvenc_regs.h $(STUBS)
	$(CC) $(CFLAGS) -Istub -o $@ test_qvenc.c

check: test_qvenc
	./test_qvenc

clean:
	rm -f test_qvenc

.PHONY: check clean
