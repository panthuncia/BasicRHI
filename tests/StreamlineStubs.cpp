#include "sl.h"

extern "C" sl::Result slInit(const sl::Preferences&, uint64_t) { return sl::Result::eErrorNotInitialized; }
extern "C" sl::Result slShutdown() { return sl::Result::eOk; }
extern "C" sl::Result slUpgradeInterface(void**) { return sl::Result::eErrorNotInitialized; }
extern "C" sl::Result slGetNativeInterface(void*, void**) { return sl::Result::eErrorNotInitialized; }
extern "C" sl::Result slSetD3DDevice(void*) { return sl::Result::eErrorNotInitialized; }
