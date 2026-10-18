/*
 * qvenc — QEMU 가상 JPEG 인코더 IP (PCI 장치 모델)
 *
 * 구조는 QEMU hw/misc/edu.c(교육용 PCI 장치)를 참고했다.
 * 레지스터 동작은 docs/regspec.md 와 include/qvenc_regs.h 가 계약이다.
 *
 * rev 1.1 (3단계, DMA 와 IRQ):
 *   - BAR0 MMIO 레지스터 파일
 *   - CTRL.START 로 SRC -> DST DMA 복사 (아직 인코딩은 하지 않는다, 4단계)
 *   - 처리 지연 모델링용 타이머. 그동안 STATUS.BUSY 가 선다
 *   - MSI(벡터 1개) 와 INTx(레벨 트리거) 인터럽트
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"

#include "hw/misc/qvenc_regs.h"

#define TYPE_PCI_QVENC_DEVICE "qvenc"
typedef struct QvencState QvencState;
DECLARE_INSTANCE_CHECKER(QvencState, QVENC, TYPE_PCI_QVENC_DEVICE)

/*
 * DMA 는 한 번에 다 읽지 않고 이 크기로 끊어서 옮긴다. 최대 프레임이 24 MiB 라
 * 통째로 들고 있을 수는 없다. 실제 DMA 엔진도 버스트 버퍼 단위로 움직인다.
 */
#define QVENC_DMA_CHUNK (64 * 1024)

struct QvencState {
    PCIDevice pdev;
    MemoryRegion mmio;

    /* 장치 속성 (레지스터 아님. 리셋해도 안 바뀐다) */
    uint32_t latency_ms;

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

    /*
     * 진행 중인 작업. START 시점에 레지스터에서 값을 떠 와서 여기에 박아 둔다.
     * BUSY 중에 소프트웨어가 레지스터를 바꿔도 돌고 있는 작업은 영향받지 않는다.
     */
    uint64_t job_src;
    uint64_t job_dst;
    uint64_t job_len;

    QEMUTimer dma_timer;
    bool irq_asserted;
    uint8_t dma_buf[QVENC_DMA_CHUNK];
};

/* ------------------------------------------------------------------ */
/* 인터럽트                                                            */
/* ------------------------------------------------------------------ */

/*
 * STATUS.IRQ 비트를 갱신하고 인터럽트 라인을 몬다.
 *
 * MSI 는 메시지를 쏘는 edge 방식이라 상태가 바뀌는 순간에만 한 번 보내야 한다.
 * 매번 보내면 인터럽트가 폭주한다. 그래서 irq_asserted 로 직전 상태를 기억해
 * 변화가 있을 때만 움직인다. INTx 는 레벨이라 내릴 때도 호출이 필요하다.
 */
static void qvenc_update_irq(QvencState *s)
{
    bool cause = (s->status & (QVENC_STATUS_DONE | QVENC_STATUS_ERROR)) != 0;
    bool pending = cause && (s->ctrl & QVENC_CTRL_IRQ_EN) != 0;

    if (pending) {
        s->status |= QVENC_STATUS_IRQ;
    } else {
        s->status &= ~QVENC_STATUS_IRQ;
    }

    if (pending == s->irq_asserted) {
        return;
    }
    s->irq_asserted = pending;

    if (msi_enabled(&s->pdev)) {
        if (pending) {
            msi_notify(&s->pdev, 0);
        }
        /* MSI 는 edge 라 내릴 것이 없다 */
    } else {
        pci_set_irq(&s->pdev, pending ? 1 : 0);
    }
}

/* ------------------------------------------------------------------ */
/* 상태 보조 함수                                                      */
/* ------------------------------------------------------------------ */

/*
 * 에러 보고. regspec 5절:
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

    s->job_src = 0;
    s->job_dst = 0;
    s->job_len = 0;
}

/* 레지스터 리셋 + 진행 중 작업 취소 + 인터럽트 라인 내리기 */
static void qvenc_device_reset(QvencState *s)
{
    timer_del(&s->dma_timer);
    qvenc_reset_regs(s);

    if (s->irq_asserted) {
        s->irq_asserted = false;
        if (!msi_enabled(&s->pdev)) {
            pci_set_irq(&s->pdev, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 작업 실행                                                           */
/* ------------------------------------------------------------------ */

static bool qvenc_dim_ok(uint32_t v, uint32_t min, uint32_t max)
{
    return v >= min && v <= max && (v % QVENC_DIM_ALIGN) == 0;
}

/*
 * 지연 시간이 지나면 호출된다. 여기서 실제 DMA 가 일어난다.
 *
 * QEMU_CLOCK_VIRTUAL 타이머 콜백은 메인 루프에서 BQL 을 쥔 채 불린다.
 * 그래서 별도 락 없이 장치 상태를 만져도 된다. (edu.c 가 bql_lock() 을 쓰는 건
 * 거기엔 워커 스레드가 있기 때문이고, 우리는 스레드를 쓰지 않는다.)
 */
static void qvenc_dma_timer(void *opaque)
{
    QvencState *s = opaque;
    uint64_t done = 0;

    while (done < s->job_len) {
        uint64_t left = s->job_len - done;
        uint32_t chunk = left > QVENC_DMA_CHUNK ? QVENC_DMA_CHUNK : (uint32_t)left;

        if (pci_dma_read(&s->pdev, s->job_src + done, s->dma_buf, chunk)
            != MEMTX_OK) {
            goto dma_failed;
        }

        /*
         * rev 1.1 은 그대로 되쓴다. 4단계에서 이 사이에 JPEG 인코딩이 들어가고,
         * 쓰는 쪽 길이가 읽는 쪽보다 작아진다.
         */
        if (pci_dma_write(&s->pdev, s->job_dst + done, s->dma_buf, chunk)
            != MEMTX_OK) {
            goto dma_failed;
        }

        done += chunk;
    }

    s->out_len = (uint32_t)s->job_len;
    s->status &= ~QVENC_STATUS_BUSY;
    s->status |= QVENC_STATUS_DONE;
    qvenc_update_irq(s);
    return;

dma_failed:
    /*
     * 중간까지 옮기다 실패했을 수 있다. regspec 3.8: 에러로 끝나면 OUT_LEN 은 0이고
     * DST 내용은 정의되지 않는다.
     */
    s->out_len = 0;
    s->status &= ~QVENC_STATUS_BUSY;
    qvenc_raise_error(s, QVENC_ERR_DMA_RANGE,
                      "DMA access failed (bad address or outside guest RAM)");
}

/* CTRL.START 처리. 검사 순서는 regspec 3.8 과 같다. */
static void qvenc_start(QvencState *s)
{
    uint64_t src, dst, len;
    uint16_t cmd;

    if (s->status & QVENC_STATUS_BUSY) {
        qvenc_raise_error(s, QVENC_ERR_BUSY, "START while BUSY");
        return;
    }

    /*
     * Bus master 가 꺼져 있으면 DMA 가 그냥 실패한다. 그대로 두면 소프트웨어에는
     * "아무 일도 안 일어남"으로 보여서 추적이 어렵다. 그래서 미리 명시적으로 본다.
     * 리눅스 드라이버라면 pci_set_master() 를 안 부른 경우다.
     */
    cmd = pci_get_word(s->pdev.config + PCI_COMMAND);
    if (!(cmd & PCI_COMMAND_MASTER)) {
        qvenc_raise_error(s, QVENC_ERR_NO_BUS_MASTER,
                          "START with bus mastering disabled "
                          "(driver forgot pci_set_master?)");
        return;
    }

    if (!qvenc_dim_ok(s->width, QVENC_WIDTH_MIN, QVENC_WIDTH_MAX) ||
        !qvenc_dim_ok(s->height, QVENC_HEIGHT_MIN, QVENC_HEIGHT_MAX) ||
        s->quality < QVENC_QUALITY_MIN || s->quality > QVENC_QUALITY_MAX) {
        qvenc_raise_error(s, QVENC_ERR_BAD_PARAM,
                          "START with WIDTH/HEIGHT/QUALITY out of range");
        return;
    }

    /* I420: W*H (Y) + 2 * (W/2 * H/2) (Cb, Cr) = W*H*3/2 */
    len = (uint64_t)s->width * s->height * 3 / 2;

    if (s->dst_size < len) {
        qvenc_raise_error(s, QVENC_ERR_DST_SMALL,
                          "DST_SIZE smaller than the expected output");
        return;
    }

    src = ((uint64_t)s->src_hi << 32) | s->src_lo;
    dst = ((uint64_t)s->dst_hi << 32) | s->dst_lo;

    if (src + len < src || dst + len < dst) {
        qvenc_raise_error(s, QVENC_ERR_DMA_RANGE, "DMA address range overflows");
        return;
    }

    /* 작업 파라미터를 지금 떠 둔다. 이후 레지스터가 바뀌어도 이 작업은 안 바뀐다. */
    s->job_src = src;
    s->job_dst = dst;
    s->job_len = len;

    /* 새 작업이 이전 완료 상태를 대체한다 (regspec 3.8) */
    s->status &= ~QVENC_STATUS_DONE;
    s->status |= QVENC_STATUS_BUSY;
    s->out_len = 0;
    qvenc_update_irq(s);

    timer_mod(&s->dma_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->latency_ms);
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
        qvenc_device_reset(s);
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

    /* MSI 를 안 쓰는 소프트웨어를 위해 INTA 를 남겨 둔다 */
    pci_config_set_interrupt_pin(pci_conf, 1);

    /* 벡터 1개, 64비트 주소, per-vector masking 없음 */
    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    timer_init_ms(&s->dma_timer, QEMU_CLOCK_VIRTUAL, qvenc_dma_timer, s);

    s->irq_asserted = false;
    qvenc_reset_regs(s);

    memory_region_init_io(&s->mmio, OBJECT(s), &qvenc_mmio_ops, s,
                          "qvenc-mmio", QVENC_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void pci_qvenc_uninit(PCIDevice *pdev)
{
    QvencState *s = QVENC(pdev);

    timer_del(&s->dma_timer);
    msi_uninit(pdev);
}

/* 버스/시스템 리셋에서 불린다 */
static void qvenc_reset(DeviceState *dev)
{
    qvenc_device_reset(QVENC(dev));
}

static const Property qvenc_properties[] = {
    DEFINE_PROP_UINT32("latency-ms", QvencState, latency_ms,
                       QVENC_LATENCY_MS_DEFAULT),
};

static void qvenc_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_qvenc_realize;
    k->exit = pci_qvenc_uninit;
    k->vendor_id = QVENC_PCI_VENDOR_ID;     /* 0x1234 == PCI_VENDOR_ID_QEMU */
    k->device_id = QVENC_PCI_DEVICE_ID;     /* 0x5100 */
    k->revision = QVENC_PCI_REVISION;
    k->class_id = PCI_CLASS_MULTIMEDIA_VIDEO;

    dc->desc = "qvenc virtual JPEG encoder IP";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, qvenc_properties);
    device_class_set_legacy_reset(dc, qvenc_reset);
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
