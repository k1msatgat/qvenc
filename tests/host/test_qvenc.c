#include "../../hw/qvenc.c"

/* 스텁 전역 */
int stub_log_count;
int64_t sim_now_ms;
bool sim_msi_on;
int sim_msi_notify_count;
const Property *sim_props; size_t sim_nprops;
DeviceReset sim_reset_fn;
uint8_t *sim_ram;
uint64_t sim_ram_base = 0x40000000ULL, sim_ram_size = 4u << 20;
int sim_irq_level, sim_irq_transitions;

static int fails, checks;
#define CK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static QvencState dev;
static uint32_t RD(hwaddr a) { return (uint32_t)qvenc_mmio_read(&dev, a, 4); }
static void WR(hwaddr a, uint32_t v) { qvenc_mmio_write(&dev, a, v, 4); }
static uint32_t ST(void) { return RD(QVENC_REG_STATUS); }
static uint32_t EC(void) { return RD(QVENC_REG_ERR_CODE); }
static void ACK(void) { WR(QVENC_REG_IRQ_ACK, QVENC_IRQ_ACK_ALL); }

static void set_master(bool on)
{
    uint16_t c = pci_get_word(dev.pdev.config + PCI_COMMAND);
    c = on ? (c | PCI_COMMAND_MASTER) : (c & ~PCI_COMMAND_MASTER);
    pci_set_word(dev.pdev.config + PCI_COMMAND, c);
}

static void expect_err(const char *name, uint32_t want)
{
    CK(ST() & QVENC_STATUS_ERROR, "%s: ERROR not set (STATUS=0x%x)", name, ST());
    CK(EC() == want, "%s: ERR_CODE=0x%02x want 0x%02x", name, EC(), want);
    ACK();
}

#define W 64u
#define H 64u
#define FRAME (W * H * 3 / 2)           /* 6144 */
#define SRC (sim_ram_base)
#define DST (sim_ram_base + 0x10000)

static void program_job(uint32_t w, uint32_t h, uint32_t q,
                        uint64_t src, uint64_t dst, uint32_t dsz)
{
    WR(QVENC_REG_WIDTH, w);  WR(QVENC_REG_HEIGHT, h); WR(QVENC_REG_QUALITY, q);
    WR(QVENC_REG_SRC_LO, (uint32_t)src); WR(QVENC_REG_SRC_HI, (uint32_t)(src >> 32));
    WR(QVENC_REG_DST_LO, (uint32_t)dst); WR(QVENC_REG_DST_HI, (uint32_t)(dst >> 32));
    WR(QVENC_REG_DST_SIZE, dsz);
}
static void fill_src(void)
{
    for (unsigned i = 0; i < FRAME; i++)
        sim_ram[i] = (uint8_t)(i * 31 + 7);
    memset(sim_ram + (DST - sim_ram_base), 0, FRAME);
}
static void START(void) { WR(QVENC_REG_CTRL, RD(QVENC_REG_CTRL) | QVENC_CTRL_START); }

int main(void)
{
    sim_ram = calloc(1, sim_ram_size);
    printf("qvenc rev1.1 DMA/IRQ test (host, stubbed QEMU)\n");

    /* 속성 기본값은 QOM 이 realize 전에 넣어 준다. 스텁에서는 직접 적용한다. */
    {
        PCIDeviceClass k; DeviceClass *dc = (DeviceClass *)&k;
        memset(&k, 0, sizeof(k));
        qvenc_class_init((ObjectClass *)&k, NULL);
        CK(sim_nprops == 1, "property count = %zu", sim_nprops);
        CK(strcmp(sim_props[0].name, "latency-ms") == 0, "property name");
        CK(sim_props[0].defval == QVENC_LATENCY_MS_DEFAULT, "latency default=%llu",
           (unsigned long long)sim_props[0].defval);
        CK(sim_reset_fn != NULL, "legacy reset not wired");
        CK(dc->desc != NULL, "desc not set");
        *(uint32_t *)((uint8_t *)&dev + sim_props[0].offset) =
            (uint32_t)sim_props[0].defval;
    }

    { Error *e = NULL; pci_qvenc_realize(&dev.pdev, &e); }
    CK(dev.latency_ms == QVENC_LATENCY_MS_DEFAULT, "latency_ms=%u", dev.latency_ms);
    CK(dev.pdev.config[0x3d] == 1, "interrupt pin not set");
    CK(RD(QVENC_REG_ID) == 0x51000101u, "ID=0x%08x", RD(QVENC_REG_ID));

    /* 1. bus master 꺼짐 */
    printf("[1] bus master\n");
    set_master(false);
    program_job(W, H, 75, SRC, DST, FRAME);
    START();
    CK(!(ST() & QVENC_STATUS_BUSY), "must not go BUSY without bus master");
    CK(!sim_timer_pending(&dev.dma_timer), "timer must not be armed");
    expect_err("no bus master", QVENC_ERR_NO_BUS_MASTER);
    set_master(true);

    /* 2. 파라미터 검증 */
    printf("[2] parameter validation\n");
    program_job(0, H, 75, SRC, DST, FRAME);   START();
    expect_err("WIDTH=0", QVENC_ERR_BAD_PARAM);
    program_job(24, H, 75, SRC, DST, FRAME);  START();   /* 16의 배수 아님 */
    expect_err("WIDTH=24", QVENC_ERR_BAD_PARAM);
    program_job(W, H, 0, SRC, DST, FRAME);    START();
    expect_err("QUALITY=0", QVENC_ERR_BAD_PARAM);
    program_job(W, H, 101, SRC, DST, FRAME);  START();
    expect_err("QUALITY=101", QVENC_ERR_BAD_PARAM);
    program_job(4112, H, 75, SRC, DST, FRAME); START();  /* > 4096 */
    expect_err("WIDTH>max", QVENC_ERR_BAD_PARAM);
    CK(!(ST() & QVENC_STATUS_BUSY), "param errors must not go BUSY");

    /* 3. DST_SIZE 부족 */
    printf("[3] DST_SIZE\n");
    program_job(W, H, 75, SRC, DST, FRAME - 1); START();
    expect_err("DST too small", QVENC_ERR_DST_SMALL);

    /* 4. 주소 오버플로 */
    printf("[4] address overflow\n");
    program_job(W, H, 75, 0xffffffffffffff00ULL, DST, FRAME); START();
    expect_err("src overflow", QVENC_ERR_DMA_RANGE);
    CK(!(ST() & QVENC_STATUS_BUSY), "overflow must be caught before BUSY");

    /* 5. 정상 경로 */
    printf("[5] happy path\n");
    fill_src();
    program_job(W, H, 75, SRC, DST, FRAME);
    START();
    CK(ST() & QVENC_STATUS_BUSY, "BUSY must be set after START");
    CK(!(ST() & QVENC_STATUS_DONE), "DONE must not be set while BUSY");
    CK(RD(QVENC_REG_OUT_LEN) == 0, "OUT_LEN must be 0 while BUSY");
    CK(sim_timer_pending(&dev.dma_timer), "timer must be armed");
    CK(dev.dma_timer.expire == (int64_t)QVENC_LATENCY_MS_DEFAULT,
       "expire=%lld", (long long)dev.dma_timer.expire);
    sim_timer_fire(&dev.dma_timer);
    CK(!(ST() & QVENC_STATUS_BUSY), "BUSY must clear");
    CK(ST() & QVENC_STATUS_DONE, "DONE must be set");
    CK(!(ST() & QVENC_STATUS_ERROR), "no error expected");
    CK(RD(QVENC_REG_OUT_LEN) == FRAME, "OUT_LEN=%u want %u",
       RD(QVENC_REG_OUT_LEN), FRAME);
    CK(memcmp(sim_ram, sim_ram + (DST - sim_ram_base), FRAME) == 0,
       "DST content != SRC content");
    ACK();

    /* 6. chunk 경계를 넘는 큰 프레임 (64 KiB chunk) */
    printf("[6] multi-chunk frame\n");
    {
        const uint32_t w2 = 512, h2 = 512, f2 = 512 * 512 * 3 / 2;  /* 393216 */
        for (uint32_t i = 0; i < f2; i++) sim_ram[i] = (uint8_t)(i ^ (i >> 8));
        uint64_t dst2 = sim_ram_base + 0x100000;
        memset(sim_ram + (dst2 - sim_ram_base), 0, f2);
        program_job(w2, h2, 75, SRC, dst2, f2);
        START();
        sim_timer_fire(&dev.dma_timer);
        CK(ST() & QVENC_STATUS_DONE, "multi-chunk DONE");
        CK(RD(QVENC_REG_OUT_LEN) == f2, "multi-chunk OUT_LEN=%u want %u",
           RD(QVENC_REG_OUT_LEN), f2);
        CK(memcmp(sim_ram, sim_ram + (dst2 - sim_ram_base), f2) == 0,
           "multi-chunk content mismatch");
        ACK();
    }

    /* 7. RAM 밖 DMA */
    printf("[7] DMA outside RAM\n");
    program_job(W, H, 75, sim_ram_base + sim_ram_size, DST, FRAME);
    START();
    CK(ST() & QVENC_STATUS_BUSY, "BUSY until the transfer is attempted");
    sim_timer_fire(&dev.dma_timer);
    CK(!(ST() & QVENC_STATUS_BUSY), "BUSY must clear on DMA failure");
    CK(RD(QVENC_REG_OUT_LEN) == 0, "OUT_LEN must be 0 on failure");
    expect_err("DMA out of range", QVENC_ERR_DMA_RANGE);

    /* 8. BUSY 중 START */
    printf("[8] START while BUSY\n");
    fill_src();
    program_job(W, H, 75, SRC, DST, FRAME);
    START();
    CK(ST() & QVENC_STATUS_BUSY, "BUSY");
    START();
    CK(EC() == QVENC_ERR_BUSY, "second START ERR_CODE=0x%02x", EC());
    WR(QVENC_REG_IRQ_ACK, QVENC_IRQ_ACK_ERROR);
    CK(ST() & QVENC_STATUS_BUSY, "running job must survive the rejected START");
    sim_timer_fire(&dev.dma_timer);
    CK(ST() & QVENC_STATUS_DONE, "original job must still complete");
    CK(RD(QVENC_REG_OUT_LEN) == FRAME, "original job OUT_LEN");
    ACK();

    /* 9. 작업 파라미터 래칭 */
    printf("[9] job parameter latching\n");
    fill_src();
    program_job(W, H, 75, SRC, DST, FRAME);
    START();
    WR(QVENC_REG_WIDTH, 4096);                    /* BUSY 중 변경 */
    WR(QVENC_REG_SRC_LO, (uint32_t)(SRC + 0x1000));
    sim_timer_fire(&dev.dma_timer);
    CK(RD(QVENC_REG_OUT_LEN) == FRAME,
       "latched job must ignore mid-flight register writes (OUT_LEN=%u)",
       RD(QVENC_REG_OUT_LEN));
    CK(memcmp(sim_ram, sim_ram + (DST - sim_ram_base), FRAME) == 0,
       "latched SRC must be used");
    ACK();

    /* 10. INTx 인터럽트 */
    printf("[10] INTx\n");
    sim_msi_on = false; sim_irq_level = 0; sim_irq_transitions = 0;
    fill_src();
    program_job(W, H, 75, SRC, DST, FRAME);
    WR(QVENC_REG_CTRL, QVENC_CTRL_IRQ_EN);
    START();
    CK(sim_irq_level == 0, "no IRQ while BUSY");
    sim_timer_fire(&dev.dma_timer);
    CK(sim_irq_level == 1, "INTx must assert on DONE");
    CK(ST() & QVENC_STATUS_IRQ, "STATUS.IRQ must be set");
    WR(QVENC_REG_IRQ_ACK, QVENC_IRQ_ACK_DONE);
    CK(sim_irq_level == 0, "INTx must de-assert after ack");
    CK(!(ST() & QVENC_STATUS_IRQ), "STATUS.IRQ must clear");
    CK(sim_irq_transitions == 2, "expected exactly 2 line transitions, got %d",
       sim_irq_transitions);

    /* IRQ_EN 을 내리면 라인도 내려간다 */
    fill_src(); program_job(W, H, 75, SRC, DST, FRAME); START();
    sim_timer_fire(&dev.dma_timer);
    CK(sim_irq_level == 1, "asserted");
    WR(QVENC_REG_CTRL, 0);
    CK(sim_irq_level == 0, "clearing IRQ_EN must lower the line");
    CK(ST() & QVENC_STATUS_DONE, "DONE must survive IRQ_EN=0");
    ACK();

    /* IRQ_EN=0 이면 애초에 안 올라간다 */
    fill_src(); program_job(W, H, 75, SRC, DST, FRAME); START();
    sim_timer_fire(&dev.dma_timer);
    CK(sim_irq_level == 0, "IRQ_EN=0 must not assert");
    ACK();

    /* 11. MSI */
    printf("[11] MSI\n");
    sim_msi_on = true; sim_msi_notify_count = 0;
    sim_irq_transitions = 0;
    fill_src(); program_job(W, H, 75, SRC, DST, FRAME);
    WR(QVENC_REG_CTRL, QVENC_CTRL_IRQ_EN);
    START();
    sim_timer_fire(&dev.dma_timer);
    CK(sim_msi_notify_count == 1, "exactly one MSI, got %d", sim_msi_notify_count);
    CK(sim_irq_transitions == 0, "MSI mode must not drive INTx");
    ACK();
    CK(sim_msi_notify_count == 1, "ack must not send another MSI");
    sim_msi_on = false;

    /* 12. START 는 이전 DONE 을 치운다 */
    printf("[12] START clears stale DONE\n");
    fill_src(); program_job(W, H, 75, SRC, DST, FRAME);
    WR(QVENC_REG_CTRL, 0);
    START(); sim_timer_fire(&dev.dma_timer);
    CK(ST() & QVENC_STATUS_DONE, "first job DONE");
    START();                                  /* ack 없이 바로 다음 작업 */
    CK(!(ST() & QVENC_STATUS_DONE), "START must clear stale DONE");
    CK(ST() & QVENC_STATUS_BUSY, "second job BUSY");
    sim_timer_fire(&dev.dma_timer);
    CK(ST() & QVENC_STATUS_DONE, "second job DONE");
    ACK();

    /* 13. BUSY 중 RESET */
    printf("[13] RESET while BUSY\n");
    fill_src(); program_job(W, H, 75, SRC, DST, FRAME);
    WR(QVENC_REG_CTRL, QVENC_CTRL_IRQ_EN);
    START();
    CK(sim_timer_pending(&dev.dma_timer), "timer armed");
    WR(QVENC_REG_CTRL, QVENC_CTRL_RESET);
    CK(!sim_timer_pending(&dev.dma_timer), "RESET must cancel the timer");
    CK(ST() == 0, "RESET must clear STATUS (got 0x%x)", ST());
    CK(RD(QVENC_REG_WIDTH) == 0, "RESET must clear WIDTH");
    CK(RD(QVENC_REG_QUALITY) == QVENC_RESET_QUALITY, "RESET restores QUALITY");
    CK(sim_irq_level == 0, "RESET must lower IRQ");

    /* 리셋 후 인터럽트가 걸린 상태에서 다시 리셋 */
    fill_src(); program_job(W, H, 75, SRC, DST, FRAME);
    WR(QVENC_REG_CTRL, QVENC_CTRL_IRQ_EN); START();
    sim_timer_fire(&dev.dma_timer);
    CK(sim_irq_level == 1, "asserted before reset");
    sim_reset_fn((DeviceState *)&dev);        /* 버스 리셋 경로 */
    CK(sim_irq_level == 0, "bus reset must lower IRQ");
    CK(ST() == 0, "bus reset must clear STATUS");

    /* 14. rev1.0 회귀: 레지스터 규칙은 그대로인가 */
    printf("[14] rev1.0 register rules still hold\n");
    WR(QVENC_REG_ID, 1);        expect_err("RO write", QVENC_ERR_RO_WRITE);
    RD(QVENC_REG_IRQ_ACK);      expect_err("WO read", QVENC_ERR_WO_READ);
    RD(0x100);                  expect_err("bad offset", QVENC_ERR_BAD_OFFSET);
    qvenc_mmio_read(&dev, 0x02, 4); expect_err("unaligned", QVENC_ERR_ALIGN);
    WR(QVENC_REG_CTRL, 1u << 31); expect_err("reserved bit", QVENC_ERR_RESERVED_BIT);

    printf("%d checks, %d failures, %d guest-error logs\n",
           checks, fails, stub_log_count);
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    free(sim_ram);
    return fails ? 1 : 0;
}
