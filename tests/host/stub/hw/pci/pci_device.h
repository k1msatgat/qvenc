#ifndef STUB_PCI_DEVICE_H
#define STUB_PCI_DEVICE_H
#include "hw/pci/pci.h"
typedef uint64_t dma_addr_t;
typedef enum { MEMTX_OK = 0, MEMTX_ERROR = 1, MEMTX_DECODE_ERROR = 2 } MemTxResult;

#define PCI_COMMAND        0x04
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4

static inline uint16_t pci_get_word(const uint8_t *c)
{ return (uint16_t)(c[0] | (c[1] << 8)); }
static inline void pci_set_word(uint8_t *c, uint16_t v)
{ c[0] = v & 0xff; c[1] = v >> 8; }

/* 게스트 RAM 흉내 */
extern uint8_t *sim_ram;
extern uint64_t sim_ram_base, sim_ram_size;

static inline bool sim_dma_ok(PCIDevice *dev, uint64_t addr, uint64_t len)
{
    if (!(pci_get_word(dev->config + PCI_COMMAND) & PCI_COMMAND_MASTER))
        return false;                     /* bus master 꺼짐 → 디코드 실패 */
    if (addr < sim_ram_base) return false;
    if (addr + len < addr) return false;
    if (addr + len > sim_ram_base + sim_ram_size) return false;
    return true;
}
static inline MemTxResult pci_dma_read(PCIDevice *dev, dma_addr_t addr,
                                       void *buf, dma_addr_t len)
{
    if (!sim_dma_ok(dev, addr, len)) return MEMTX_DECODE_ERROR;
    memcpy(buf, sim_ram + (addr - sim_ram_base), len);
    return MEMTX_OK;
}
static inline MemTxResult pci_dma_write(PCIDevice *dev, dma_addr_t addr,
                                        const void *buf, dma_addr_t len)
{
    if (!sim_dma_ok(dev, addr, len)) return MEMTX_DECODE_ERROR;
    memcpy(sim_ram + (addr - sim_ram_base), buf, len);
    return MEMTX_OK;
}

/* INTx 라인 */
extern int sim_irq_level; extern int sim_irq_transitions;
static inline void pci_set_irq(PCIDevice *dev, int level)
{ (void)dev; if (level != sim_irq_level) sim_irq_transitions++; sim_irq_level = level; }
#endif
