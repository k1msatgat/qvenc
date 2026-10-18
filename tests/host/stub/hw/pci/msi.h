#ifndef STUB_MSI_H
#define STUB_MSI_H
extern bool sim_msi_on;
extern int sim_msi_notify_count;
static inline int msi_init(PCIDevice *d, uint8_t off, unsigned nv, bool b64,
                           bool pvm, Error **errp)
{ (void)d;(void)off;(void)nv;(void)b64;(void)pvm;(void)errp; return 0; }
static inline void msi_uninit(PCIDevice *d) { (void)d; }
static inline bool msi_enabled(const PCIDevice *d) { (void)d; return sim_msi_on; }
static inline void msi_notify(PCIDevice *d, unsigned v)
{ (void)d; (void)v; sim_msi_notify_count++; }
#endif
