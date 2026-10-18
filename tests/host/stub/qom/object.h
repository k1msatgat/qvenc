#ifndef STUB_OBJECT_H
#define STUB_OBJECT_H
typedef struct ObjectClass ObjectClass;
typedef struct Object Object;
#define OBJECT(x) ((Object *)(x))
#define DECLARE_INSTANCE_CHECKER(S, NAME, T) \
    static inline S *NAME(void *p) { return (S *)p; }
typedef struct InterfaceInfo { const char *type; } InterfaceInfo;
typedef struct TypeInfo {
    const char *name, *parent;
    size_t instance_size;
    void (*class_init)(ObjectClass *, const void *);
    const InterfaceInfo *interfaces;
} TypeInfo;
#define DEFINE_TYPES(t) const TypeInfo *stub_types = t; const size_t stub_ntypes = sizeof(t)/sizeof(t[0]);
#endif
