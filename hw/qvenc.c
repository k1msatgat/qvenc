/*
 * qvenc — QEMU 가상 JPEG 인코더 IP (PCI 장치 모델)
 *
 * 구조는 QEMU hw/misc/edu.c(교육용 PCI 장치)를 참고했다.
 * 레지스터 동작은 docs/regspec.md 와 include/qvenc_regs.h 가 계약이다.
 *
 * rev 1.0 (2단계, 장치 골격): BAR0 MMIO 레지스터 파일만 구현한다.
 *   - DMA 없음, 인터럽트 라인 구동 없음 (3단계)
 *   - 인코딩 파이프라인 없음 (4단계) → CTRL.START 는 ERR_UNIMPL 을 보고한다
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "qom/object.h"

#include "hw/misc/qvenc_regs.h"

#define TYPE_PCI_QVENC_DEVICE "qvenc"
typedef struct QvencState QvencState;
DECLARE_INSTANCE_CHECKER(QvencState, QVENC, TYPE_PCI_QVENC_DEVICE)

struct QvencState {
    PCIDevice pdev;
    MemoryRegion mmio;

    /* 레지스터 파일. 이름은 regspec 의 레지스터 이름과 그대로 맞춘다. */
    uint32_t ctrl;      /* IRQ_EN 비트만 유지된다. START/RESET 은 self-clearing */
    uint32_t status;
    uint32_t err_code;
    uint32_t src_lo;
    uint32_t src_hi;
    uint32_t dst_lo;
    uint32_t dst_hi;
    uint32_t dst_size;
    uint32_t out_len;
    uint32_t width;
    uint32_t height;
    uint32_t quality;
};

/* ------------------------------------------------------------------ */
/* 상태 보조 함수                                                      */
/* ------------------------------------------------------------------ */

/*
 * STATUS.IRQ 는 "인터럽트 원인이 살아 있고 IRQ_EN 이 켜져 있다" 를 뜻한다.
 * 3단계에서 여기에 pci_set_irq()/msi_notify() 호출이 붙는다. 지금은 비트만 관리한다.
 */
static void qvenc_update_irq(QvencState *s)
{
    bool pending = (s->status & (QVENC_STATUS_DONE | QVENC_STATUS_ERROR)) != 0;

    if (pending && (s->ctrl & QVENC_CTRL_IRQ_EN)) {
        s->status |= QVENC_STATUS_IRQ;
    } else {
        s->status &= ~QVENC_STATUS_IRQ;
    }
}

/*
 * 에러 보고. regspec 4절:
 *   - STATUS.ERROR 가 이미 1이면 ERR_CODE 를 덮어쓰지 않는다 (first error wins).
 *   - 에러는 장치를 멈추지 않는다.
 */
static void qvenc_raise_error(QvencState *s, uint32_t code, const char *why)
{
    qemu_log_mask(LOG_GUEST_ERROR, "qvenc: %s (ERR_CODE=0x%02x)\n", why, code);

    if (s->status & QVENC_STATUS_ERROR) {
        /* 첫 번째 원인을 보존한다. 소프트웨어가 ack 하기 전까지 유지. */
        return;
    }

    s->status |= QVENC_STATUS_ERROR;
    s->err_code = code;
    qvenc_update_irq(s);
}

static void qvenc_reset_regs(QvencState *s)
{
    s->ctrl     = QVENC_RESET_CTRL;
    s->status   = QVENC_RESET_STATUS;
    s->err_code = QVENC_RESET_ERR_CODE;
    s->src_lo   = QVENC_RESET_SRC_LO;
    s->src_hi   = QVENC_RESET_SRC_HI;
    s->dst_lo   = QVENC_RESET_DST_LO;
    s->dst_hi   = QVENC_RESET_DST_HI;
    s->dst_size = QVENC_RESET_DST_SIZE;
    s->out_len  = QVENC_RESET_OUT_LEN;
    s->width    = QVENC_RESET_WIDTH;
    s->height   = QVENC_RESET_HEIGHT;
    s->quality  = QVENC_RESET_QUALITY;
}

/*
 * CTRL.START 처리.
 *
 * rev 1.0 에는 인코딩 파이프라인이 없다. 성공한 척(DONE 올리기)도, 조용한 무시도 하지
 * 않고 ERR_UNIMPL 을 보고한다. 3단계에서 이 함수가 DMA 와 타이머로 바뀐다.
 */
static void qvenc_start(QvencState *s)
{
    if (s->status & QVENC_STATUS_BUSY) {
        qvenc_raise_error(s, QVENC_ERR_BUSY, "START while BUSY");
        return;
    }

    qvenc_raise_error(s, QVENC_ERR_UNIMPL,
                      "START: encode pipeline not implemented in rev 1.0");
}

static void qvenc_ctrl_write(QvencState *s, uint32_t val)
{
    if (val & ~QVENC_CTRL_WRITE_MASK) {
        /* regspec 2절 규칙 6: 예약 비트에 1 → 쓰기 전체를 버린다. */
        qvenc_raise_error(s, QVENC_ERR_RESERVED_BIT,
                          "write to reserved CTRL bits");
        return;
    }

    /* regspec 3.2: RESET 과 START 가 동시에 오면 RESET 이 이긴다. */
    if (val & QVENC_CTRL_RESET) {
        qvenc_reset_regs(s);
        return;
    }

    /* IRQ_EN 만 래치된다. START/RESET 은 self-clearing 이라 저장하지 않는다. */
    s->ctrl = val & QVENC_CTRL_IRQ_EN;
    qvenc_update_irq(s);

    if (val & QVENC_CTRL_START) {
        qvenc_start(s);
    }
}

static void qvenc_irq_ack_write(QvencState *s, uint32_t val)
{
    if (val & ~QVENC_IRQ_ACK_WRITE_MASK) {
        qvenc_raise_error(s, QVENC_ERR_RESERVED_BIT,
                          "write to reserved IRQ_ACK bits");
        return;
    }

    if (val & QVENC_IRQ_ACK_DONE) {
        s->status &= ~QVENC_STATUS_DONE;
    }

    if (val & QVENC_IRQ_ACK_ERROR) {
        s->status &= ~QVENC_STATUS_ERROR;
        s->err_code = QVENC_ERR_NONE;
    }

    qvenc_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

/*
 * 접근 폭/정렬 검사.
 *
 * MemoryRegionOps 에 min_access_size=1, unaligned=true 로 열어 두었기 때문에
 * 잘못된 접근도 여기까지 도달한다. 일부러 그렇게 했다 — QEMU 가 먼저 걸러 버리면
 * 장치가 ERR_ALIGN 으로 보고할 기회가 없고, 그러면 "조용히 무시" 가 된다.
 */
static bool qvenc_access_ok(QvencState *s, hwaddr addr, unsigned size,
                            const char *what)
{
    if (size != 4 || (addr & 0x3) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qvenc: bad %s access addr=0x%" HWADDR_PRIx " size=%u"
                      " (32-bit aligned only)\n", what, addr, size);
        qvenc_raise_error(s, QVENC_ERR_ALIGN, "unaligned or non-32-bit access");
        return false;
    }

    return true;
}

static uint64_t qvenc_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    QvencState *s = opaque;

    if (!qvenc_access_ok(s, addr, size, "read")) {
        return 0xffffffffULL;   /* regspec 2절 규칙 5 */
    }

    switch (addr) {
    case QVENC_REG_ID:
        return QVENC_ID_VALUE;
    case QVENC_REG_CTRL:
        return s->ctrl & QVENC_CTRL_READ_MASK;
    case QVENC_REG_STATUS:
        return s->status & QVENC_STATUS_MASK;
    case QVENC_REG_ERR_CODE:
        return s->err_code;
    case QVENC_REG_SRC_LO:
        return s->src_lo;
    case QVENC_REG_SRC_HI:
        return s->src_hi;
    case QVENC_REG_DST_LO:
        return s->dst_lo;
    case QVENC_REG_DST_HI:
        return s->dst_hi;
    case QVENC_REG_DST_SIZE:
        return s->dst_size;
    case QVENC_REG_OUT_LEN:
        return s->out_len;
    case QVENC_REG_WIDTH:
        return s->width;
    case QVENC_REG_HEIGHT:
        return s->height;
    case QVENC_REG_QUALITY:
        return s->quality;

    case QVENC_REG_IRQ_ACK:
        qvenc_raise_error(s, QVENC_ERR_WO_READ, "read of write-only IRQ_ACK");
        return 0xffffffffULL;

    default:
        qvenc_raise_error(s, QVENC_ERR_BAD_OFFSET, "read of undefined offset");
        return 0xffffffffULL;
    }
}

static void qvenc_mmio_write(void *opaque, hwaddr addr, uint64_t val64,
                             unsigned size)
{
    QvencState *s = opaque;
    uint32_t val = (uint32_t)val64;

    if (!qvenc_access_ok(s, addr, size, "write")) {
        return;             /* regspec 2절 규칙 5: 상태를 바꾸지 않는다 */
    }

    switch (addr) {
    case QVENC_REG_CTRL:
        qvenc_ctrl_write(s, val);
        break;
    case QVENC_REG_IRQ_ACK:
        qvenc_irq_ack_write(s, val);
        break;

    /*
     * 파라미터 레지스터는 쓰는 값을 검사하지 않고 그대로 래치한다.
     * 유효성은 START 시점에 한 번 검사한다 (regspec 3.6).
     */
    case QVENC_REG_SRC_LO:
        s->src_lo = val;
        break;
    case QVENC_REG_SRC_HI:
        s->src_hi = val;
        break;
    case QVENC_REG_DST_LO:
        s->dst_lo = val;
        break;
    case QVENC_REG_DST_HI:
        s->dst_hi = val;
        break;
    case QVENC_REG_DST_SIZE:
        s->dst_size = val;
        break;
    case QVENC_REG_WIDTH:
        s->width = val;
        break;
    case QVENC_REG_HEIGHT:
        s->height = val;
        break;
    case QVENC_REG_QUALITY:
        s->quality = val;
        break;

    case QVENC_REG_ID:
    case QVENC_REG_STATUS:
    case QVENC_REG_OUT_LEN:
        qvenc_raise_error(s, QVENC_ERR_RO_WRITE,
                          "write to read-only register");
        break;

    default:
        qvenc_raise_error(s, QVENC_ERR_BAD_OFFSET,
                          "write to undefined offset");
        break;
    }
}

static const MemoryRegionOps qvenc_mmio_ops = {
    .read = qvenc_mmio_read,
    .write = qvenc_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * valid 를 넓게 열어 잘못된 접근이 핸들러까지 오도록 한다 (위 주석 참고).
     * impl 도 같이 넓혀야 QEMU 가 접근을 4바이트 단위로 쪼개거나 합치지 않고
     * 게스트가 낸 주소/폭 그대로 전달한다.
     */
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

/* ------------------------------------------------------------------ */
/* QOM                                                                 */
/* ------------------------------------------------------------------ */

static void pci_qvenc_realize(PCIDevice *pdev, Error **errp)
{
    QvencState *s = QVENC(pdev);
    uint8_t *pci_conf = pdev->config;

    /* INTA. 3단계에서 실제로 라인을 구동한다. */
    pci_config_set_interrupt_pin(pci_conf, 1);

    qvenc_reset_regs(s);

    memory_region_init_io(&s->mmio, OBJECT(s), &qvenc_mmio_ops, s,
                          "qvenc-mmio", QVENC_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void qvenc_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_qvenc_realize;
    k->vendor_id = QVENC_PCI_VENDOR_ID;     /* 0x1234 == PCI_VENDOR_ID_QEMU */
    k->device_id = QVENC_PCI_DEVICE_ID;     /* 0x5100 */
    k->revision = QVENC_PCI_REVISION;
    k->class_id = PCI_CLASS_MULTIMEDIA_VIDEO;

    dc->desc = "qvenc virtual JPEG encoder IP";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo qvenc_types[] = {
    {
        .name          = TYPE_PCI_QVENC_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(QvencState),
        .class_init    = qvenc_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(qvenc_types)
