#ifndef STUB_PCI_H
#define STUB_PCI_H
#include "qom/object.h"
#define TYPE_PCI_DEVICE "pci-device"
#define INTERFACE_CONVENTIONAL_PCI_DEVICE "conventional-pci-device"
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0
#define PCI_CLASS_MULTIMEDIA_VIDEO 0x0400
#define DEVICE_CATEGORY_MISC 2
#define DEVICE_LITTLE_ENDIAN 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef struct PCIDevice { uint8_t config[256]; } PCIDevice;
typedef struct MemoryRegion { const char *name; uint64_t size; void *opaque; } MemoryRegion;
typedef struct MemoryRegionOps {
    uint64_t (*read)(void *, hwaddr, unsigned);
    void (*write)(void *, hwaddr, uint64_t, unsigned);
    int endianness;
    struct { unsigned min_access_size, max_access_size; bool unaligned; } valid, impl;
} MemoryRegionOps;
typedef struct DeviceClass { const char *desc; unsigned long categories[1]; } DeviceClass;
typedef struct PCIDeviceClass {
    DeviceClass parent_class;   /* real QEMU embeds DeviceClass first */
    void (*realize)(PCIDevice *, Error **);
    void (*exit)(PCIDevice *);
    uint16_t vendor_id, device_id; uint8_t revision; uint16_t class_id;
} PCIDeviceClass;
#define DEVICE_CLASS(c) ((DeviceClass *)(c))
#define PCI_DEVICE_CLASS(c) ((PCIDeviceClass *)(c))
static inline void pci_config_set_interrupt_pin(uint8_t *c, uint8_t v) { c[0x3d] = v; }
static inline void memory_region_init_io(MemoryRegion *mr, Object *o,
        const MemoryRegionOps *ops, void *opaque, const char *n, uint64_t sz)
{ mr->name = n; mr->size = sz; mr->opaque = opaque; (void)o; (void)ops; }
static inline void pci_register_bar(PCIDevice *d, int n, uint8_t t, MemoryRegion *mr)
{ (void)d; (void)n; (void)t; (void)mr; }
#endif
