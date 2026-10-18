#ifndef STUB_QDEV_PROPERTIES_H
#define STUB_QDEV_PROPERTIES_H
#include "hw/pci/pci.h"
typedef struct DeviceState DeviceState;
typedef struct Property { const char *name; size_t offset; uint64_t defval; } Property;
#define DEFINE_PROP_UINT32(_n, _s, _f, _d) \
    { .name = (_n), .offset = offsetof(_s, _f), .defval = (uint64_t)(_d) }
typedef void (*DeviceReset)(DeviceState *dev);
extern const Property *sim_props; extern size_t sim_nprops;
extern DeviceReset sim_reset_fn;
#define device_class_set_props(dc, props) \
    do { (void)(dc); sim_props = (props); sim_nprops = ARRAY_SIZE(props); } while (0)
static inline void device_class_set_legacy_reset(DeviceClass *dc, DeviceReset fn)
{ (void)dc; sim_reset_fn = fn; }
#endif
