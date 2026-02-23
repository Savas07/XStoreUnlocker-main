#pragma once

#include "xstore_types.h"
#include "config.h"

namespace StoreHooks {
    void Initialize(const UnlockerConfig& cfg);
    void OnStoreInterfaceCreated(void** ppInterface);
    void Shutdown();
}

namespace PackageHooks {
    void Initialize(const UnlockerConfig& cfg);
    void OnPackageInterfaceCreated(void** ppInterface);
    void Shutdown();
}

namespace ComServerHooks {
    void Initialize(const UnlockerConfig& cfg);
    void TryHookContext(void* xstoreContext);
    void Shutdown();
}
