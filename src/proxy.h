#pragma once

#include "xstore_types.h"
#include <Windows.h>

namespace Proxy {
    void SetOurModule(HMODULE m);
    bool EnsureInitialized();
    void Shutdown(bool processTerminating);

    QueryApiImpl_t          GetReal_QueryApiImpl();
    InitializeApiImpl_t     GetReal_InitializeApiImpl();
    InitializeApiImplEx_t   GetReal_InitializeApiImplEx();
    InitializeApiImplEx2_t  GetReal_InitializeApiImplEx2();
    UninitializeApiImpl_t   GetReal_UninitializeApiImpl();
    DllCanUnloadNow_t       GetReal_DllCanUnloadNow();
    XErrorReport_t          GetReal_XErrorReport();
}

// Provider GUIDs (from IDA: GetGuid functions on provider objects)
static const GUID XSTORE_PROVIDER_GUID = {
    0x0DD112AC, 0x7C24, 0x448C,
    { 0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C }
};

static const GUID XPACKAGE_PROVIDER_GUID = {
    0xAF406016, 0xE850, 0x4AA8,
    { 0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E }
};

inline bool GuidsEqual(const GUID* a, const GUID* b) {
    return memcmp(a, b, sizeof(GUID)) == 0;
}
