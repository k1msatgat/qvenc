/* SPDX-License-Identifier: MIT */
/*
 * qvenc — QEMU 가상 JPEG 인코더 IP 레지스터 정의
 *
 * docs/regspec.md 와 1:1로 대응한다. 한쪽만 고치지 말 것.
 * 레지스터 계약 rev 1.0 / HW rev 0x0100
 *
 * 이 헤더는 QEMU 장치 모델, 리눅스 커널 드라이버, 유저 공간 SDK가 함께 쓴다.
 * 따라서 어떤 헤더도 include 하지 않고, 타입도 쓰지 않는다 (순수 매크로).
 */

#ifndef QVENC_REGS_H
#define QVENC_REGS_H

/* ------------------------------------------------------------------ */
/* PCI 식별                                                            */
/* ------------------------------------------------------------------ */

#define QVENC_PCI_VENDOR_ID     0x1234
#define QVENC_PCI_DEVICE_ID     0x5100
#define QVENC_PCI_REVISION      0x01

/* BAR0: 4 KiB MMIO 레지스터 블록 */
#define QVENC_BAR0_SIZE         0x1000

/* ------------------------------------------------------------------ */
/* 레지스터 오프셋 (BAR0 기준, 모두 32비트 폭 / 4바이트 정렬)           */
/* ------------------------------------------------------------------ */

#define QVENC_REG_ID            0x00    /* RO */
#define QVENC_REG_CTRL          0x04    /* RW */
#define QVENC_REG_STATUS        0x08    /* RO */
#define QVENC_REG_IRQ_ACK       0x0c    /* WO */
#define QVENC_REG_ERR_CODE      0x10    /* RO */
#define QVENC_REG_SRC_LO        0x14    /* RW */
#define QVENC_REG_SRC_HI        0x18    /* RW */
#define QVENC_REG_DST_LO        0x1c    /* RW */
#define QVENC_REG_DST_HI        0x20    /* RW */
#define QVENC_REG_DST_SIZE      0x24    /* RW */
#define QVENC_REG_OUT_LEN       0x28    /* RO */
#define QVENC_REG_WIDTH         0x2c    /* RW */
#define QVENC_REG_HEIGHT        0x30    /* RW */
#define QVENC_REG_QUALITY       0x34    /* RW */

/* 정의된 마지막 레지스터 다음 오프셋. 이 위부터 BAR0 끝까지는 예약이다. */
#define QVENC_REG_END           0x38

/* ------------------------------------------------------------------ */
/* ID (0x00, RO)                                                       */
/* ------------------------------------------------------------------ */

#define QVENC_ID_MAGIC_SHIFT    16
#define QVENC_ID_MAGIC_MASK     0xffff0000u
#define QVENC_ID_MAGIC          0x5100u         /* == PCI device id */

#define QVENC_ID_VER_MAJOR_SHIFT 8
#define QVENC_ID_VER_MAJOR_MASK  0x0000ff00u
#define QVENC_ID_VER_MINOR_SHIFT 0
#define QVENC_ID_VER_MINOR_MASK  0x000000ffu

#define QVENC_ID_VER_MAJOR      0x01u
#define QVENC_ID_VER_MINOR      0x00u
#define QVENC_ID_VERSION        0x0100u         /* MAJOR:MINOR */

/* ID 레지스터의 리셋(=고정) 값 */
#define QVENC_ID_VALUE                                                  \
        (((QVENC_ID_MAGIC) << QVENC_ID_MAGIC_SHIFT) | (QVENC_ID_VERSION))

/* 읽은 ID에서 필드 뽑기 */
#define QVENC_ID_GET_MAGIC(v)                                           \
        (((v) & QVENC_ID_MAGIC_MASK) >> QVENC_ID_MAGIC_SHIFT)
#define QVENC_ID_GET_VER_MAJOR(v)                                       \
        (((v) & QVENC_ID_VER_MAJOR_MASK) >> QVENC_ID_VER_MAJOR_SHIFT)
#define QVENC_ID_GET_VER_MINOR(v)                                       \
        (((v) & QVENC_ID_VER_MINOR_MASK) >> QVENC_ID_VER_MINOR_SHIFT)

/* ------------------------------------------------------------------ */
/* CTRL (0x04, RW)                                                     */
/* ------------------------------------------------------------------ */

#define QVENC_CTRL_START        (1u << 0)       /* self-clearing, 0으로 읽힘 */
#define QVENC_CTRL_RESET        (1u << 1)       /* self-clearing, 0으로 읽힘 */
#define QVENC_CTRL_IRQ_EN       (1u << 2)       /* RW, 쓴 값 그대로 읽힘 */

/* 소프트웨어가 1을 써도 되는 비트 전체. 이 밖은 예약 → ERR_RESERVED_BIT */
#define QVENC_CTRL_WRITE_MASK                                           \
        (QVENC_CTRL_START | QVENC_CTRL_RESET | QVENC_CTRL_IRQ_EN)

/* 실제로 되읽히는 비트 */
#define QVENC_CTRL_READ_MASK    (QVENC_CTRL_IRQ_EN)

/* ------------------------------------------------------------------ */
/* STATUS (0x08, RO)                                                   */
/* ------------------------------------------------------------------ */

#define QVENC_STATUS_BUSY       (1u << 0)
#define QVENC_STATUS_DONE       (1u << 1)       /* sticky */
#define QVENC_STATUS_ERROR      (1u << 2)       /* sticky */
#define QVENC_STATUS_IRQ        (1u << 3)

#define QVENC_STATUS_MASK                                               \
        (QVENC_STATUS_BUSY | QVENC_STATUS_DONE |                        \
         QVENC_STATUS_ERROR | QVENC_STATUS_IRQ)

/* ------------------------------------------------------------------ */
/* IRQ_ACK (0x0c, WO) — 쓴 비트에 해당하는 상태를 해제한다             */
/* ------------------------------------------------------------------ */

#define QVENC_IRQ_ACK_DONE      (1u << 0)       /* STATUS.DONE 해제 */
#define QVENC_IRQ_ACK_ERROR     (1u << 1)       /* STATUS.ERROR + ERR_CODE 해제 */

#define QVENC_IRQ_ACK_WRITE_MASK                                        \
        (QVENC_IRQ_ACK_DONE | QVENC_IRQ_ACK_ERROR)

/* 모든 원인을 한 번에 지울 때 */
#define QVENC_IRQ_ACK_ALL       QVENC_IRQ_ACK_WRITE_MASK

/* ------------------------------------------------------------------ */
/* ERR_CODE (0x10, RO)                                                 */
/* ------------------------------------------------------------------ */

#define QVENC_ERR_NONE          0x00u
#define QVENC_ERR_ALIGN         0x01u   /* size != 4 또는 비정렬 주소 */
#define QVENC_ERR_RO_WRITE      0x02u   /* RO 레지스터에 쓰기 */
#define QVENC_ERR_WO_READ       0x03u   /* WO 레지스터를 읽기 */
#define QVENC_ERR_BAD_OFFSET    0x04u   /* 정의되지 않은 오프셋 */
#define QVENC_ERR_BUSY          0x05u   /* BUSY 중 START */
#define QVENC_ERR_RESERVED_BIT  0x06u   /* 예약 비트에 1 */
#define QVENC_ERR_BAD_PARAM     0x07u   /* WIDTH/HEIGHT/QUALITY 범위 밖 (3단계) */
#define QVENC_ERR_DMA_RANGE     0x08u   /* DMA 범위 초과 (3단계) */
#define QVENC_ERR_DST_SMALL     0x09u   /* 출력이 DST_SIZE 초과 (4단계) */
#define QVENC_ERR_UNIMPL        0x7fu   /* 이 리비전이 구현하지 않은 기능 */

/* ------------------------------------------------------------------ */
/* 파라미터 유효 범위 — START 시점에 검사된다 (쓰기 시점 아님)          */
/* ------------------------------------------------------------------ */

#define QVENC_WIDTH_MIN         16u
#define QVENC_WIDTH_MAX         4096u
#define QVENC_HEIGHT_MIN        16u
#define QVENC_HEIGHT_MAX        4096u
#define QVENC_DIM_ALIGN         8u      /* JPEG MCU 경계 */

#define QVENC_QUALITY_MIN       1u
#define QVENC_QUALITY_MAX       100u

/* ------------------------------------------------------------------ */
/* 리셋 값                                                             */
/* ------------------------------------------------------------------ */

#define QVENC_RESET_CTRL        0x00000000u
#define QVENC_RESET_STATUS      0x00000000u
#define QVENC_RESET_ERR_CODE    QVENC_ERR_NONE
#define QVENC_RESET_SRC_LO      0x00000000u
#define QVENC_RESET_SRC_HI      0x00000000u
#define QVENC_RESET_DST_LO      0x00000000u
#define QVENC_RESET_DST_HI      0x00000000u
#define QVENC_RESET_DST_SIZE    0x00000000u
#define QVENC_RESET_OUT_LEN     0x00000000u
#define QVENC_RESET_WIDTH       0x00000000u
#define QVENC_RESET_HEIGHT      0x00000000u
#define QVENC_RESET_QUALITY     75u

#endif /* QVENC_REGS_H */
