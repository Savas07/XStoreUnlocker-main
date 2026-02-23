#include "store_hooks.h"
#include "proxy.h"
#include "logger.h"
#include <atomic>
#include <cstring>
#include <unordered_set>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <mutex>

// Shared state
static UnlockerConfig    g_cfg;
static VtableEntry_t     g_orig[STORE_VTABLE_SIZE] = {};
static SRWLOCK           g_hookLock     = SRWLOCK_INIT;
static VtableEntry_t*    g_hookedVtable = nullptr;
static std::atomic<bool> g_shutdown{false};
static inline bool       LogOn() { return g_cfg.logEnabled; }

static std::unordered_set<std::string> g_discoveredStoreIds;
static SRWLOCK g_discoveredLock = SRWLOCK_INIT;

static void AddDiscoveredStoreId(const std::string& id) {
    if (id.empty()) return;
    AcquireSRWLockExclusive(&g_discoveredLock);
    bool isNew = g_discoveredStoreIds.insert(id).second;
    ReleaseSRWLockExclusive(&g_discoveredLock);
    if (isNew && LogOn())
        LOG_INFO("discovered store ID: %s (total: %zu)", id.c_str(), g_discoveredStoreIds.size());
}

static std::unordered_set<std::string> GetDiscoveredStoreIds() {
    AcquireSRWLockShared(&g_discoveredLock);
    auto copy = g_discoveredStoreIds;
    ReleaseSRWLockShared(&g_discoveredLock);
    return copy;
}

static std::atomic_bool g_hookFired[STORE_VTABLE_SIZE] = {};
static void LogHookFireOnce(int idx, const char* name) {
    if (!LogOn() || idx < 0 || idx >= (int)STORE_VTABLE_SIZE) return;
    if (!g_hookFired[idx].exchange(true))
        LOG_INFO("hook fired: vt[%d] %s", idx, name ? name : "?");
}

// Layout constants

constexpr size_t XSTORELICENSE_SIZE = 0x30;  // 48 bytes
constexpr size_t OFF_LICENSE_VALID  = 4;

constexpr size_t OFF_GAME_LICENSE_ACTIVE      = 18;
constexpr size_t OFF_GAME_LICENSE_TRIAL_OWNED = 19;
constexpr size_t OFF_GAME_LICENSE_DISC        = 20;
constexpr size_t OFF_GAME_LICENSE_TRIAL       = 21;
constexpr size_t OFF_GAME_LICENSE_EXPIRY      = 96;
constexpr size_t GAME_LICENSE_SIZE            = 104;

constexpr size_t OFF_ADDON_IS_ACTIVE = 82;
constexpr size_t ADDON_LICENSE_SIZE  = 96;

constexpr size_t OFF_CAN_ACQUIRE_STATUS = 8;

// XStoreProduct (208 bytes)
constexpr size_t STRIDE        = 208;
constexpr size_t OFF_ID        = 0;
constexpr size_t OFF_TITLE     = 8;
constexpr size_t OFF_DESC      = 16;
constexpr size_t OFF_LANG      = 24;
constexpr size_t OFF_OFFER     = 32;
constexpr size_t OFF_LINK      = 40;
constexpr size_t OFF_KIND      = 48;
constexpr size_t OFF_CURRENCY  = 72;
constexpr size_t OFF_FMT_BASE  = 80;
constexpr size_t OFF_FMT_PRICE = 96;
constexpr size_t OFF_FMT_REC   = 112;
constexpr size_t OFF_HAS_DL    = 144;
constexpr size_t OFF_OWNED     = 145;
constexpr size_t OFF_SKU_COUNT = 160;
constexpr size_t OFF_SKU_PTR   = 168;
constexpr size_t OFF_ARR_START = 24;
constexpr size_t OFF_ARR_END   = 32;

// XStoreSku (0x110 = 272 bytes)
constexpr size_t SKU_STRIDE             = 0x110;
constexpr size_t SKU_OFF_ID             = 0;
constexpr size_t SKU_OFF_TITLE          = 8;
constexpr size_t SKU_OFF_IS_TRIAL       = 120;
constexpr size_t SKU_OFF_HAS_COLL       = 121;
constexpr size_t SKU_OFF_COLL_ACQUIRED  = 128;
constexpr size_t SKU_OFF_COLL_START     = 136;
constexpr size_t SKU_OFF_COLL_END       = 144;
constexpr size_t SKU_OFF_COLL_IS_TRIAL  = 152;
constexpr size_t SKU_OFF_COLL_TRIAL_SEC = 156;
constexpr size_t SKU_OFF_COLL_QUANTITY  = 160;
constexpr size_t SKU_OFF_COLL_CAMPAIGN  = 168;
constexpr size_t SKU_OFF_COLL_DEVOFFER  = 176;
constexpr size_t SKU_OFF_AVAIL_COUNT    = 256;
constexpr size_t SKU_OFF_AVAIL_PTR      = 264;

constexpr int64_t FAKE_ACQUIRED = 133484064000000000LL;  // feel free to set to current this is set to 2024 
constexpr int64_t FAKE_END      = 0x7FFFFFFFFFFFFFFELL;

// Function pointer typedefs
typedef int64_t(__fastcall* Fn2)(void*, void*);
typedef int64_t(__fastcall* Fn3)(void*, void*, void*);
typedef int64_t(__fastcall* Fn4)(void*, void*, void*, void*);
typedef int64_t(__fastcall* Fn5)(void*, void*, uint64_t, uint64_t, void*);
typedef int64_t(__fastcall* Fn6)(void*, void*, void*, uint64_t, uint64_t, void*);
typedef int64_t(__fastcall* Fn8)(void*, void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, void*);
typedef int64_t(__fastcall* FnResultQH_t)(void*, void*, void**);
typedef uint8_t(__fastcall* ProductCb_t)(void*, void*);
typedef int64_t(__fastcall* FnRegisterCallback_t)(void*, void*, void*, void*, void*, void*);
typedef int64_t(__fastcall* FnUnregisterCallback_t)(void*, void*, void*, void*);
typedef int64_t(__fastcall* CreateContextFn_t)(void*, void*, void**);

// Fake license allocation and tracking
static void* AllocFakeLicense() {
    auto* p = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, XSTORELICENSE_SIZE);
    if (p) p[OFF_LICENSE_VALID] = 1;
    return p;
}

static std::unordered_set<void*> g_fakeLicenses;
static SRWLOCK g_fakeLicenseLock = SRWLOCK_INIT;

static void TrackFakeLicense(void* h) {
    AcquireSRWLockExclusive(&g_fakeLicenseLock);
    g_fakeLicenses.insert(h);
    ReleaseSRWLockExclusive(&g_fakeLicenseLock);
}

static bool IsFakeLicense(void* h) {
    AcquireSRWLockShared(&g_fakeLicenseLock);
    bool found = g_fakeLicenses.count(h) > 0;
    ReleaseSRWLockShared(&g_fakeLicenseLock);
    return found;
}

static bool ConsumeFakeLicense(void* h) {
    AcquireSRWLockExclusive(&g_fakeLicenseLock);
    bool found = g_fakeLicenses.erase(h) > 0;
    ReleaseSRWLockExclusive(&g_fakeLicenseLock);
    return found;
}

static std::atomic<uint64_t> g_nextFakeToken{0xDEAD0001};

// Fake async tracking
static std::unordered_set<void*> g_fakeAsyncBlocks;
static SRWLOCK g_fakeAsyncLock = SRWLOCK_INIT;

static bool IsFakePackageName(const char* pkg) {
    return pkg && strncmp(pkg, "FakeDLC.", 8) == 0;
}

static void TrackFakeAsync(void* ab) {
    AcquireSRWLockExclusive(&g_fakeAsyncLock);
    g_fakeAsyncBlocks.insert(ab);
    ReleaseSRWLockExclusive(&g_fakeAsyncLock);
}

static bool ConsumeFakeAsync(void* ab) {
    AcquireSRWLockExclusive(&g_fakeAsyncLock);
    bool found = g_fakeAsyncBlocks.erase(ab) > 0;
    ReleaseSRWLockExclusive(&g_fakeAsyncLock);
    return found;
}

static DWORD WINAPI FakeAsyncCompletionThread(LPVOID param) {
    void* asyncBlock = param;
    Sleep(1);
    typedef void(__stdcall* AsyncCallback_t)(void*);
    auto cb = *(AsyncCallback_t*)((uint8_t*)asyncBlock + 16);
    if (cb) {
        if (LogOn()) LOG_INFO("FakeAsync: firing completion for asyncBlock=%p", asyncBlock);
        cb(asyncBlock);
    }
    return 0;
}

// SKU level CollectionData patching
static void PatchProductSkuData(uint8_t* product) {
    uint32_t skuCount = *(uint32_t*)(product + OFF_SKU_COUNT);
    uint8_t* skuArr   = *(uint8_t**)(product + OFF_SKU_PTR);
    if (!skuArr || skuCount == 0) return;

    for (uint32_t i = 0; i < skuCount && i < 64; i++) {
        uint8_t* sku = skuArr + (size_t)i * SKU_STRIDE;
        sku[SKU_OFF_HAS_COLL]  = 1;
        sku[SKU_OFF_IS_TRIAL]  = 0;
        *(int64_t*)(sku + SKU_OFF_COLL_ACQUIRED)  = FAKE_ACQUIRED;
        *(int64_t*)(sku + SKU_OFF_COLL_START)      = FAKE_ACQUIRED;
        *(int64_t*)(sku + SKU_OFF_COLL_END)        = FAKE_END;
        sku[SKU_OFF_COLL_IS_TRIAL]                 = 0;
        *(uint32_t*)(sku + SKU_OFF_COLL_TRIAL_SEC) = 0;
        *(uint32_t*)(sku + SKU_OFF_COLL_QUANTITY)  = 1;
    }
}


// Group 1: License acquisition

static int64_t __fastcall Hook_PackageLicenseAsync(void* self, void* ctx, void* pkg, void* async) {
    if (g_shutdown) return ((Fn4)g_orig[StoreVtable::AcquireLicenseForPackageAsync])(self, ctx, pkg, async);
    LogHookFireOnce(StoreVtable::AcquireLicenseForPackageAsync, "AcquireLicenseForPackageAsync");

    if (LogOn() && pkg) LOG_INFO("AcquireLicenseForPackageAsync: pkg=%s", (const char*)pkg);

    if (IsFakePackageName((const char*)pkg)) {
        if (LogOn()) LOG_INFO("AcquireLicenseForPackageAsync: FAKE pkg, bypassing");
        TrackFakeAsync(async);
        HANDLE h = CreateThread(nullptr, 0, FakeAsyncCompletionThread, async, 0, nullptr);
        if (h) CloseHandle(h);
        return 0;
    }
    return ((Fn4)g_orig[StoreVtable::AcquireLicenseForPackageAsync])(self, ctx, pkg, async);
}

static int64_t __fastcall Hook_PackageLicenseResult(void* self, void* async, void** out) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::AcquireLicenseForPackageResult])(self, async, out);
    LogHookFireOnce(StoreVtable::AcquireLicenseForPackageResult, "AcquireLicenseForPackageResult");

    if (ConsumeFakeAsync(async)) {
        if (out) {
            void* fake = AllocFakeLicense();
            if (fake) { TrackFakeLicense(fake); *out = fake; }
            if (LogOn()) LOG_INFO("PackageLicenseResult: FAKE async -> injected fake license");
        }
        return 0;
    }

    int64_t hr = ((Fn3)g_orig[StoreVtable::AcquireLicenseForPackageResult])(self, async, out);
    if (hr == 0 && out && *out) {
        ((uint8_t*)*out)[OFF_LICENSE_VALID] = 1;
        if (LogOn()) LOG_INFO("PackageLicenseResult: forced valid");
    } else if (out) {
        void* fake = AllocFakeLicense();
        if (fake) {
            TrackFakeLicense(fake); *out = fake;
            if (LogOn()) LOG_INFO("PackageLicenseResult: FAILED (0x%llX) -> fake", (unsigned long long)hr);
            hr = 0;
        }
    }
    return hr;
}

static int64_t __fastcall Hook_DurablesLicenseAsync(void* self, void* ctx, void* id, void* async) {
    if (g_shutdown) return ((Fn4)g_orig[StoreVtable::AcquireLicenseForDurablesAsync])(self, ctx, id, async);
    LogHookFireOnce(StoreVtable::AcquireLicenseForDurablesAsync, "AcquireLicenseForDurablesAsync");
    if (LogOn() && id) LOG_INFO("AcquireLicenseForDurablesAsync: id=%s", (const char*)id);
    return ((Fn4)g_orig[StoreVtable::AcquireLicenseForDurablesAsync])(self, ctx, id, async);
}

static int64_t __fastcall Hook_DurablesLicenseResult(void* self, void* async, void** out) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::AcquireLicenseForDurablesResult])(self, async, out);
    LogHookFireOnce(StoreVtable::AcquireLicenseForDurablesResult, "AcquireLicenseForDurablesResult");

    int64_t hr = ((Fn3)g_orig[StoreVtable::AcquireLicenseForDurablesResult])(self, async, out);
    if (hr == 0 && out && *out) {
        ((uint8_t*)*out)[OFF_LICENSE_VALID] = 1;
        if (LogOn()) LOG_INFO("DurablesLicenseResult: forced valid");
    } else if (out) {
        void* fake = AllocFakeLicense();
        if (fake) {
            TrackFakeLicense(fake); *out = fake;
            if (LogOn()) LOG_INFO("DurablesLicenseResult: FAILED (0x%llX) -> fake", (unsigned long long)hr);
            hr = 0;
        }
    }
    return hr;
}

static int64_t __fastcall Hook_LicenseIsValid(void* self, void* handle) {
    if (g_shutdown) return ((Fn2)g_orig[StoreVtable::LicenseIsValid])(self, handle);
    LogHookFireOnce(StoreVtable::LicenseIsValid, "LicenseIsValid");
    if (handle) ((uint8_t*)handle)[OFF_LICENSE_VALID] = 1;
    return 1;
}

static int64_t __fastcall Hook_CloseLicenseHandle(void* self, void* handle) {
    if (g_shutdown) return ((Fn2)g_orig[StoreVtable::CloseLicenseHandle])(self, handle);
    LogHookFireOnce(StoreVtable::CloseLicenseHandle, "CloseLicenseHandle");
    if (ConsumeFakeLicense(handle)) {
        HeapFree(GetProcessHeap(), 0, handle);
        if (LogOn()) LOG_INFO("CloseLicenseHandle: freed fake license");
        return 0;
    }
    return ((Fn2)g_orig[StoreVtable::CloseLicenseHandle])(self, handle);
}

// Group 2: License revocation suppression

static int64_t __fastcall Hook_RegisterPackageLicenseLost(
    void* self, void* license, void* queue, void* context, void* callback, void* tokenOut)
{
    if (g_shutdown) return ((FnRegisterCallback_t)g_orig[StoreVtable::RegisterPackageLicenseLost])(self, license, queue, context, callback, tokenOut);
    LogHookFireOnce(StoreVtable::RegisterPackageLicenseLost, "RegisterPackageLicenseLost");
    if (tokenOut) *(uint64_t*)tokenOut = g_nextFakeToken.fetch_add(1);
    if (LogOn()) LOG_INFO("RegisterPackageLicenseLost: SUPPRESSED");
    return 0;
}

static int64_t __fastcall Hook_UnregisterPackageLicenseLost(void* self, void* license, void* token, void* wait) {
    if (g_shutdown) return ((FnUnregisterCallback_t)g_orig[StoreVtable::UnregisterPackageLicenseLost])(self, license, token, wait);
    LogHookFireOnce(StoreVtable::UnregisterPackageLicenseLost, "UnregisterPackageLicenseLost");
    return 0;
}

static int64_t __fastcall Hook_RegisterGameLicenseChanged(
    void* self, void* ctx, void* queue, void* context, void* callback, void* tokenOut)
{
    if (g_shutdown) return ((FnRegisterCallback_t)g_orig[StoreVtable::RegisterGameLicenseChanged])(self, ctx, queue, context, callback, tokenOut);
    LogHookFireOnce(StoreVtable::RegisterGameLicenseChanged, "RegisterGameLicenseChanged");
    if (tokenOut) *(uint64_t*)tokenOut = g_nextFakeToken.fetch_add(1);
    if (LogOn()) LOG_INFO("RegisterGameLicenseChanged: SUPPRESSED");
    return 0;
}

static int64_t __fastcall Hook_UnregisterGameLicenseChanged(void* self, void* ctx, void* token, void* wait) {
    if (g_shutdown) return ((FnUnregisterCallback_t)g_orig[StoreVtable::UnregisterGameLicenseChanged])(self, ctx, token, wait);
    LogHookFireOnce(StoreVtable::UnregisterGameLicenseChanged, "UnregisterGameLicenseChanged");
    return 0;
}

// Group 3: Can acquire license preview

static int64_t __fastcall Hook_CanAcquireStoreIdAsync(void* self, void* ctx, void* storeId, void* async) {
    if (g_shutdown) return ((Fn4)g_orig[StoreVtable::CanAcquireLicenseForStoreIdAsync])(self, ctx, storeId, async);
    LogHookFireOnce(StoreVtable::CanAcquireLicenseForStoreIdAsync, "CanAcquireLicenseForStoreIdAsync");
    return ((Fn4)g_orig[StoreVtable::CanAcquireLicenseForStoreIdAsync])(self, ctx, storeId, async);
}

static int64_t __fastcall Hook_CanAcquireStoreIdResult(void* self, void* async, void* out) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::CanAcquireLicenseForStoreIdResult])(self, async, out);
    LogHookFireOnce(StoreVtable::CanAcquireLicenseForStoreIdResult, "CanAcquireLicenseForStoreIdResult");
    int64_t hr = ((Fn3)g_orig[StoreVtable::CanAcquireLicenseForStoreIdResult])(self, async, out);
    if (out) {
        if (hr != 0) { memset(out, 0, 12); hr = 0; }
        *(uint32_t*)((uint8_t*)out + OFF_CAN_ACQUIRE_STATUS) = 0;
    }
    return hr;
}

static int64_t __fastcall Hook_CanAcquirePackageAsync(void* self, void* ctx, void* pkg, void* async) {
    if (g_shutdown) return ((Fn4)g_orig[StoreVtable::CanAcquireLicenseForPackageAsync])(self, ctx, pkg, async);
    LogHookFireOnce(StoreVtable::CanAcquireLicenseForPackageAsync, "CanAcquireLicenseForPackageAsync");
    return ((Fn4)g_orig[StoreVtable::CanAcquireLicenseForPackageAsync])(self, ctx, pkg, async);
}

static int64_t __fastcall Hook_CanAcquirePackageResult(void* self, void* async, void* out) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::CanAcquireLicenseForPackageResult])(self, async, out);
    LogHookFireOnce(StoreVtable::CanAcquireLicenseForPackageResult, "CanAcquireLicenseForPackageResult");
    int64_t hr = ((Fn3)g_orig[StoreVtable::CanAcquireLicenseForPackageResult])(self, async, out);
    if (out) {
        if (hr != 0) { memset(out, 0, 12); hr = 0; }
        *(uint32_t*)((uint8_t*)out + OFF_CAN_ACQUIRE_STATUS) = 0;
    }
    return hr;
}

// Group 4: Game license

static int64_t __fastcall Hook_GameLicenseAsync(void* self, void* ctx, void* async) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::QueryGameLicenseAsync])(self, ctx, async);
    LogHookFireOnce(StoreVtable::QueryGameLicenseAsync, "QueryGameLicenseAsync");
    return ((Fn3)g_orig[StoreVtable::QueryGameLicenseAsync])(self, ctx, async);
}

static int64_t __fastcall Hook_GameLicenseResult(void* self, void* async, void* out) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::QueryGameLicenseResult])(self, async, out);
    LogHookFireOnce(StoreVtable::QueryGameLicenseResult, "QueryGameLicenseResult");

    int64_t hr = ((Fn3)g_orig[StoreVtable::QueryGameLicenseResult])(self, async, out);
    if (out) {
        auto* p = (uint8_t*)out;
        if (hr != 0) { memset(p, 0, GAME_LICENSE_SIZE); hr = 0; }
        p[OFF_GAME_LICENSE_ACTIVE]      = 1;
        p[OFF_GAME_LICENSE_TRIAL_OWNED] = 0;
        p[OFF_GAME_LICENSE_DISC]        = 0;
        p[OFF_GAME_LICENSE_TRIAL]       = 0;
        *(int64_t*)(p + OFF_GAME_LICENSE_EXPIRY) = FAKE_END;
    }
    return hr;
}

// Group 5: Addon license enumeration

static int64_t __fastcall Hook_AddOnLicensesAsync(void* self, void* ctx, void* async) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::QueryAddOnLicensesAsync])(self, ctx, async);
    LogHookFireOnce(StoreVtable::QueryAddOnLicensesAsync, "QueryAddOnLicensesAsync");
    return ((Fn3)g_orig[StoreVtable::QueryAddOnLicensesAsync])(self, ctx, async);
}

static int64_t __fastcall Hook_AddOnLicensesResultCount(void* self, void* async, uint32_t* count) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::QueryAddOnLicensesResultCount])(self, async, count);
    LogHookFireOnce(StoreVtable::QueryAddOnLicensesResultCount, "QueryAddOnLicensesResultCount");
    int64_t hr = ((Fn3)g_orig[StoreVtable::QueryAddOnLicensesResultCount])(self, async, count);
    if (LogOn() && hr == 0 && count) LOG_INFO("AddOnLicensesResultCount: %u", *count);
    return hr;
}

static int64_t __fastcall Hook_AddOnLicensesResult(void* self, void* async, size_t bufSize, void* outBuf) {
    if (g_shutdown) return ((int64_t(__fastcall*)(void*,void*,size_t,void*))
        g_orig[StoreVtable::QueryAddOnLicensesResult])(self, async, bufSize, outBuf);
    LogHookFireOnce(StoreVtable::QueryAddOnLicensesResult, "QueryAddOnLicensesResult");

    int64_t hr = ((int64_t(__fastcall*)(void*,void*,size_t,void*))
        g_orig[StoreVtable::QueryAddOnLicensesResult])(self, async, bufSize, outBuf);

    if (hr == 0 && outBuf) {
        size_t count = bufSize / ADDON_LICENSE_SIZE;
        auto* base = (uint8_t*)outBuf;
        int patched = 0;
        for (size_t i = 0; i < count; i++) {
            auto* rec = base + i * ADDON_LICENSE_SIZE;
            std::string compound((const char*)rec, strnlen_s((const char*)rec, 18));
            auto slash = compound.find('/');
            std::string pid = (slash != std::string::npos) ? compound.substr(0, slash) : compound;

            if (!pid.empty() && g_cfg.unlockAll && g_cfg.dlcs.empty()) AddDiscoveredStoreId(pid);
            if (!pid.empty() && g_cfg.blacklist.count(pid)) continue;

            if (g_cfg.unlockAll || (!pid.empty() && g_cfg.dlcs.count(pid))) {
                rec[OFF_ADDON_IS_ACTIVE] = 1;
                *(int64_t*)(rec + 88) = FAKE_END;
                patched++;
            }
        }
        if (LogOn() && patched > 0)
            LOG_INFO("AddOnLicensesResult: forced active on %d/%zu addons", patched, count);
    }
    return hr;
}

// Group 6: Product and entitlement queries

struct FakeProduct {
    std::string storeId;
    std::unique_ptr<uint8_t[]> data;
    std::unique_ptr<uint8_t[]> skuData;

    FakeProduct(const std::string& id)
        : storeId(id)
        , data(std::make_unique<uint8_t[]>(STRIDE))
        , skuData(std::make_unique<uint8_t[]>(SKU_STRIDE))
    {
        memset(data.get(), 0, STRIDE);
        memset(skuData.get(), 0, SKU_STRIDE);

        auto* p = data.get();
        *(const char**)(p + OFF_ID)       = storeId.c_str();
        *(const char**)(p + OFF_TITLE)    = storeId.c_str();
        *(const char**)(p + OFF_DESC)     = "";
        *(const char**)(p + OFF_LANG)     = "en-US";
        *(const char**)(p + OFF_OFFER)    = "";
        *(const char**)(p + OFF_LINK)     = "";
        *(uint32_t*)(p + OFF_KIND)        = PRODUCT_KIND_DURABLE;
        *(const char**)(p + OFF_CURRENCY) = "USD";
        strncpy((char*)(p + OFF_FMT_BASE),  "$0.00", PRICE_MAX_SIZE);
        strncpy((char*)(p + OFF_FMT_PRICE), "$0.00", PRICE_MAX_SIZE);
        strncpy((char*)(p + OFF_FMT_REC),   "$0.00", PRICE_MAX_SIZE);
        p[OFF_HAS_DL] = 1;
        p[OFF_OWNED]  = 1;

        auto* sku = skuData.get();
        *(const char**)(sku + SKU_OFF_ID)          = storeId.c_str();
        *(const char**)(sku + SKU_OFF_TITLE)       = storeId.c_str();
        sku[SKU_OFF_HAS_COLL]                      = 1;
        sku[SKU_OFF_IS_TRIAL]                      = 0;
        *(int64_t*)(sku + SKU_OFF_COLL_ACQUIRED)   = FAKE_ACQUIRED;
        *(int64_t*)(sku + SKU_OFF_COLL_START)      = FAKE_ACQUIRED;
        *(int64_t*)(sku + SKU_OFF_COLL_END)        = FAKE_END;
        sku[SKU_OFF_COLL_IS_TRIAL]                 = 0;
        *(uint32_t*)(sku + SKU_OFF_COLL_TRIAL_SEC) = 0;
        *(uint32_t*)(sku + SKU_OFF_COLL_QUANTITY)  = 1;

        *(uint32_t*)(p + OFF_SKU_COUNT) = 1;
        *(uint8_t**)(p + OFF_SKU_PTR)   = sku;
    }

    FakeProduct(const FakeProduct&) = delete;
    FakeProduct& operator=(const FakeProduct&) = delete;
};

static std::vector<std::unique_ptr<FakeProduct>> g_fakeProducts;
static std::atomic<bool> g_fakesBuilt{false};
static SRWLOCK g_fakesLock = SRWLOCK_INIT;

static void BuildFakeProducts() {
    if (g_fakesBuilt) return;
    AcquireSRWLockExclusive(&g_fakesLock);
    if (!g_fakesBuilt) {
        for (const auto& id : g_cfg.dlcs) {
            if (g_cfg.blacklist.count(id)) continue;
            g_fakeProducts.push_back(std::make_unique<FakeProduct>(id));
        }
        g_fakesBuilt = true;
        if (LogOn()) LOG_INFO("built %zu fake products", g_fakeProducts.size());
    }
    ReleaseSRWLockExclusive(&g_fakesLock);
}

struct ProductsCbCtx {
    ProductCb_t userCb = nullptr;
    void* userCtx = nullptr;
    std::unordered_set<std::string>* localSeen = nullptr;
    int total = 0, patched = 0;
};

static uint8_t __fastcall Hook_GetProducts_Callback(void* product, void* ctx) {
    auto* c = reinterpret_cast<ProductsCbCtx*>(ctx);
    if (!c || !product) return 1;

    auto* p = static_cast<uint8_t*>(product);
    c->total++;

    const char* id = *(const char**)(p + OFF_ID);
    if (id && c->localSeen) c->localSeen->insert(id);
    if (id && id[0] && g_cfg.unlockAll && g_cfg.dlcs.empty()) AddDiscoveredStoreId(id);

    if (!(id && g_cfg.blacklist.count(id))) {
        if (g_cfg.unlockAll || (id && g_cfg.dlcs.count(id))) {
            if (!p[OFF_OWNED])  { p[OFF_OWNED] = 1;  c->patched++; }
            if (!p[OFF_HAS_DL]) { p[OFF_HAS_DL] = 1; c->patched++; }
            PatchProductSkuData(p);
        }
    }

    if (LogOn() && c->total <= 20) {
        const char* title = *(const char**)(p + OFF_TITLE);
        LOG_INFO("  product[%d]: id=%s title=%s kind=%u owned=%u",
                 c->total, id ? id : "?", title ? title : "?",
                 *(uint32_t*)(p + OFF_KIND), p[OFF_OWNED]);
    }
    return c->userCb ? c->userCb(product, c->userCtx) : 1;
}

static int64_t __fastcall Hook_GetProducts(void* self, void* qh, void* ctx, void* cb) {
    if (g_shutdown) return ((Fn4)g_orig[StoreVtable::ProductsQueryGetProducts])(self, qh, ctx, cb);
    LogHookFireOnce(StoreVtable::ProductsQueryGetProducts, "ProductsQueryGetProducts");

    std::unordered_set<std::string> localSeen;
    ProductsCbCtx cbCtx;
    cbCtx.userCb = (ProductCb_t)cb; cbCtx.userCtx = ctx; cbCtx.localSeen = &localSeen;

    void* callCtx = cb ? (void*)&cbCtx : ctx;
    void* callCb  = cb ? (void*)&Hook_GetProducts_Callback : cb;
    int64_t hr = ((Fn4)g_orig[StoreVtable::ProductsQueryGetProducts])(self, qh, callCtx, callCb);

    if (!cb && hr == 0 && qh) {
        auto* base = *(uint8_t**)((uint8_t*)qh + OFF_ARR_START);
        auto* end  = *(uint8_t**)((uint8_t*)qh + OFF_ARR_END);
        int patched = 0;
        for (uint8_t* p = base; base && p < end; p += STRIDE) {
            const char* id = *(const char**)(p + OFF_ID);
            if (id) { localSeen.insert(id); if (id[0] && g_cfg.unlockAll && g_cfg.dlcs.empty()) AddDiscoveredStoreId(id); }
            if (id && g_cfg.blacklist.count(id)) continue;
            if (g_cfg.unlockAll || (id && g_cfg.dlcs.count(id))) {
                if (!p[OFF_OWNED])  { p[OFF_OWNED] = 1;  patched++; }
                if (!p[OFF_HAS_DL]) { p[OFF_HAS_DL] = 1; patched++; }
                PatchProductSkuData(p);
            }
        }
        if (LogOn() && patched > 0) LOG_INFO("GetProducts(no-cb): patched=%d", patched);
    }

    if (hr == 0 && cb && LogOn() && cbCtx.patched > 0)
        LOG_INFO("GetProducts: real=%d patched=%d", cbCtx.total, cbCtx.patched);

    if (hr == 0 && cb && !g_cfg.dlcs.empty()) {
        BuildFakeProducts();
        std::vector<uint8_t*> pending;
        AcquireSRWLockShared(&g_fakesLock);
        for (const auto& f : g_fakeProducts)
            if (!localSeen.count(f->storeId)) pending.push_back(f->data.get());
        ReleaseSRWLockShared(&g_fakesLock);

        for (auto* p : pending)
            if (!((ProductCb_t)cb)(p, ctx)) break;
        if (LogOn() && !pending.empty())
            LOG_INFO("GetProducts: injected %zu fake products", pending.size());
    }
    return hr;
}

static int64_t __fastcall Hook_QueryProductsAsync(
    void* self, void* ctx, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, void* async) {
    if (g_shutdown) return ((Fn8)g_orig[StoreVtable::QueryProductsAsync])(self, ctx, a3, a4, a5, a6, a7, async);
    LogHookFireOnce(StoreVtable::QueryProductsAsync, "QueryProductsAsync");
    return ((Fn8)g_orig[StoreVtable::QueryProductsAsync])(self, ctx, a3, a4, a5, a6, a7, async);
}

static int64_t __fastcall Hook_QueryProductsResult(void* self, void* async, void** out) {
    if (g_shutdown) return ((FnResultQH_t)g_orig[StoreVtable::QueryProductsResult])(self, async, out);
    LogHookFireOnce(StoreVtable::QueryProductsResult, "QueryProductsResult");
    return ((FnResultQH_t)g_orig[StoreVtable::QueryProductsResult])(self, async, out);
}

static int64_t __fastcall Hook_QueryEntitledAsync(void* self, void* ctx, uint64_t a3, uint64_t a4, void* async) {
    if (g_shutdown) return ((Fn5)g_orig[StoreVtable::QueryEntitledProductsAsync])(self, ctx, a3, a4, async);
    LogHookFireOnce(StoreVtable::QueryEntitledProductsAsync, "QueryEntitledProductsAsync");
    return ((Fn5)g_orig[StoreVtable::QueryEntitledProductsAsync])(self, ctx, a3, a4, async);
}

static int64_t __fastcall Hook_QueryEntitledResult(void* self, void* async, void** out) {
    if (g_shutdown) return ((FnResultQH_t)g_orig[StoreVtable::QueryEntitledProductsResult])(self, async, out);
    LogHookFireOnce(StoreVtable::QueryEntitledProductsResult, "QueryEntitledProductsResult");
    return ((FnResultQH_t)g_orig[StoreVtable::QueryEntitledProductsResult])(self, async, out);
}

// Group 7: Availability and pass through hooks

static int64_t __fastcall Hook_IsAvailabilityPurchasable(void* self, void* a2) {
    if (g_shutdown) return ((Fn2)g_orig[StoreVtable::IsAvailabilityPurchasable])(self, a2);
    LogHookFireOnce(StoreVtable::IsAvailabilityPurchasable, "IsAvailabilityPurchasable");
    return 1;
}

#define PASSTHROUGH2(Name, VtIdx) \
static int64_t __fastcall Hook_##Name(void* self, void* a2) { \
    if (g_shutdown) return ((Fn2)g_orig[VtIdx])(self, a2); \
    LogHookFireOnce(VtIdx, #Name); return ((Fn2)g_orig[VtIdx])(self, a2); }

#define PASSTHROUGH3(Name, VtIdx) \
static int64_t __fastcall Hook_##Name(void* self, void* a2, void* a3) { \
    if (g_shutdown) return ((Fn3)g_orig[VtIdx])(self, a2, a3); \
    LogHookFireOnce(VtIdx, #Name); return ((Fn3)g_orig[VtIdx])(self, a2, a3); }

#define PASSTHROUGH_QH(Name, VtIdx) \
static int64_t __fastcall Hook_##Name(void* self, void* async, void** out) { \
    if (g_shutdown) return ((FnResultQH_t)g_orig[VtIdx])(self, async, out); \
    LogHookFireOnce(VtIdx, #Name); return ((FnResultQH_t)g_orig[VtIdx])(self, async, out); }

PASSTHROUGH3(QueryAssocAsync,          StoreVtable::QueryAssociatedProductsAsync)  // actually Fn5 but only logging
PASSTHROUGH_QH(QueryAssocResult,       StoreVtable::QueryAssociatedProductsResult)
PASSTHROUGH3(QueryCurrentGameAsync,    StoreVtable::QueryProductForCurrentGameAsync)
PASSTHROUGH_QH(QueryCurrentGameResult, StoreVtable::QueryProductForCurrentGameResult)
PASSTHROUGH_QH(QueryPkgResult,         StoreVtable::QueryProductForPackageResult)
PASSTHROUGH3(HasMorePages,             StoreVtable::ProductsQueryHasMorePages)
PASSTHROUGH2(QueryClose,               StoreVtable::ProductsQueryClose)
PASSTHROUGH_QH(QueryAssocStoreIdResult, StoreVtable::QueryAssociatedProductsForStoreIdResult)

#undef PASSTHROUGH2
#undef PASSTHROUGH3
#undef PASSTHROUGH_QH

static int64_t __fastcall Hook_QueryProductForPackageAsync(void* self, void* ctx, uint64_t kind, const char* pkg, void* async) {
    if (g_shutdown) return ((Fn5)g_orig[StoreVtable::QueryProductForPackageAsync])(self, ctx, kind, (uint64_t)pkg, async);
    LogHookFireOnce(StoreVtable::QueryProductForPackageAsync, "QueryProductForPackageAsync");
    if (LogOn() && pkg) LOG_INFO("QueryProductForPackageAsync: pkg=%s", pkg);
    return ((Fn5)g_orig[StoreVtable::QueryProductForPackageAsync])(self, ctx, kind, (uint64_t)pkg, async);
}

static int64_t __fastcall Hook_QueryAssocForStoreIdAsync(
    void* self, void* ctx, void* storeId, uint64_t kind, uint64_t maxItems, void* async) {
    if (g_shutdown) return ((Fn6)g_orig[StoreVtable::QueryAssociatedProductsForStoreIdAsync])(self, ctx, storeId, kind, maxItems, async);
    LogHookFireOnce(StoreVtable::QueryAssociatedProductsForStoreIdAsync, "QueryAssocForStoreIdAsync");
    return ((Fn6)g_orig[StoreVtable::QueryAssociatedProductsForStoreIdAsync])(self, ctx, storeId, kind, maxItems, async);
}

// Group 8: Package identifier resolution

static int64_t __fastcall Hook_QueryPackageIdentifier(
    void* self, const char* storeId, uint64_t bufSize, char* outBuf)
{
    if (g_shutdown) return ((int64_t(__fastcall*)(void*,const char*,uint64_t,char*))
        g_orig[StoreVtable::QueryPackageIdentifier])(self, storeId, bufSize, outBuf);
    LogHookFireOnce(StoreVtable::QueryPackageIdentifier, "QueryPackageIdentifier");

    if (!storeId || !outBuf)
        return ((int64_t(__fastcall*)(void*,const char*,uint64_t,char*))
            g_orig[StoreVtable::QueryPackageIdentifier])(self, storeId, bufSize, outBuf);

    if (bufSize > 0) outBuf[0] = '\0';
    int64_t hr = ((int64_t(__fastcall*)(void*,const char*,uint64_t,char*))
        g_orig[StoreVtable::QueryPackageIdentifier])(self, storeId, bufSize, outBuf);

    if (hr >= 0 && outBuf[0] != '\0') {
        if (LogOn()) LOG_INFO("QueryPackageIdentifier: %s -> %s", storeId, outBuf);
        return hr;
    }
    std::string sid(storeId);
    if (g_cfg.blacklist.count(sid)) return hr;
    if (g_cfg.unlockAll || g_cfg.dlcs.count(sid)) {
        std::string fake = "FakeDLC." + sid + "_unlocker";
        if (bufSize > fake.size()) {
            memcpy(outBuf, fake.c_str(), fake.size() + 1);
            if (LogOn()) LOG_INFO("QueryPackageIdentifier: %s -> FAKE=%s", storeId, outBuf);
            return 0;
        }
        return 0x800700EA;
    }
    return hr;
}

// Group 9: License token queries

static int64_t __fastcall Hook_QueryLicenseTokenAsync(
    void* self, void* ctx, void* ids, uint64_t count, void* cat, void* async) {
    if (g_shutdown) return ((int64_t(__fastcall*)(void*,void*,void*,uint64_t,void*,void*))
        g_orig[StoreVtable::QueryLicenseTokenAsync])(self, ctx, ids, count, cat, async);
    LogHookFireOnce(StoreVtable::QueryLicenseTokenAsync, "QueryLicenseTokenAsync");
    if (LogOn()) {
        auto** arr = (const char**)ids;
        for (uint64_t i = 0; i < count && i < 10; i++)
            LOG_INFO("LicenseToken: id[%llu]=%s", (unsigned long long)i, arr[i] ? arr[i] : "?");
    }
    return ((int64_t(__fastcall*)(void*,void*,void*,uint64_t,void*,void*))
        g_orig[StoreVtable::QueryLicenseTokenAsync])(self, ctx, ids, count, cat, async);
}

static int64_t __fastcall Hook_LicenseTokenResultSize(void* self, void* async, void* out) {
    if (g_shutdown) return ((Fn3)g_orig[StoreVtable::QueryLicenseTokenResultSize])(self, async, out);
    LogHookFireOnce(StoreVtable::QueryLicenseTokenResultSize, "QueryLicenseTokenResultSize");
    return ((Fn3)g_orig[StoreVtable::QueryLicenseTokenResultSize])(self, async, out);
}

static int64_t __fastcall Hook_LicenseTokenResult(void* self, void* async, uint64_t sz, void* out) {
    if (g_shutdown) return ((int64_t(__fastcall*)(void*,void*,uint64_t,void*))
        g_orig[StoreVtable::QueryLicenseTokenResult])(self, async, sz, out);
    LogHookFireOnce(StoreVtable::QueryLicenseTokenResult, "QueryLicenseTokenResult");
    return ((int64_t(__fastcall*)(void*,void*,uint64_t,void*))
        g_orig[StoreVtable::QueryLicenseTokenResult])(self, async, sz, out);
}

// Group 10: CreateContext, captures XStoreContext for COM hooks

static int64_t __fastcall Hook_CreateContext(void* self, void* xuser, void** outCtx) {
    if (g_shutdown) return ((CreateContextFn_t)g_orig[StoreVtable::CreateContext])(self, xuser, outCtx);
    LogHookFireOnce(StoreVtable::CreateContext, "XStoreCreateContext");
    int64_t hr = ((CreateContextFn_t)g_orig[StoreVtable::CreateContext])(self, xuser, outCtx);
    if (hr == 0 && outCtx && *outCtx) {
        if (LogOn()) LOG_INFO("XStoreCreateContext: ctx=%p -> installing COM hooks", *outCtx);
        ComServerHooks::TryHookContext(*outCtx);
    }
    return hr;
}

// Store vtable installation

static void InstallHooks(VtableEntry_t* vt) {
    memcpy(g_orig, vt, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE);

    DWORD old;
    if (!VirtualProtect(vt, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE, PAGE_READWRITE, &old)) {
        LOG_ERROR("VirtualProtect failed: %lu", GetLastError());
        return;
    }

    vt[StoreVtable::CreateContext]                             = (VtableEntry_t)&Hook_CreateContext;
    vt[StoreVtable::AcquireLicenseForPackageAsync]             = (VtableEntry_t)&Hook_PackageLicenseAsync;
    vt[StoreVtable::AcquireLicenseForPackageResult]            = (VtableEntry_t)&Hook_PackageLicenseResult;
    vt[StoreVtable::AcquireLicenseForDurablesAsync]            = (VtableEntry_t)&Hook_DurablesLicenseAsync;
    vt[StoreVtable::AcquireLicenseForDurablesResult]           = (VtableEntry_t)&Hook_DurablesLicenseResult;
    vt[StoreVtable::LicenseIsValid]                            = (VtableEntry_t)&Hook_LicenseIsValid;
    vt[StoreVtable::CloseLicenseHandle]                        = (VtableEntry_t)&Hook_CloseLicenseHandle;
    vt[StoreVtable::RegisterPackageLicenseLost]                = (VtableEntry_t)&Hook_RegisterPackageLicenseLost;
    vt[StoreVtable::UnregisterPackageLicenseLost]              = (VtableEntry_t)&Hook_UnregisterPackageLicenseLost;
    vt[StoreVtable::RegisterGameLicenseChanged]                = (VtableEntry_t)&Hook_RegisterGameLicenseChanged;
    vt[StoreVtable::UnregisterGameLicenseChanged]              = (VtableEntry_t)&Hook_UnregisterGameLicenseChanged;
    vt[StoreVtable::CanAcquireLicenseForStoreIdAsync]          = (VtableEntry_t)&Hook_CanAcquireStoreIdAsync;
    vt[StoreVtable::CanAcquireLicenseForStoreIdResult]         = (VtableEntry_t)&Hook_CanAcquireStoreIdResult;
    vt[StoreVtable::CanAcquireLicenseForPackageAsync]          = (VtableEntry_t)&Hook_CanAcquirePackageAsync;
    vt[StoreVtable::CanAcquireLicenseForPackageResult]         = (VtableEntry_t)&Hook_CanAcquirePackageResult;
    vt[StoreVtable::QueryGameLicenseAsync]                     = (VtableEntry_t)&Hook_GameLicenseAsync;
    vt[StoreVtable::QueryGameLicenseResult]                    = (VtableEntry_t)&Hook_GameLicenseResult;
    vt[StoreVtable::QueryAddOnLicensesAsync]                   = (VtableEntry_t)&Hook_AddOnLicensesAsync;
    vt[StoreVtable::QueryAddOnLicensesResultCount]             = (VtableEntry_t)&Hook_AddOnLicensesResultCount;
    vt[StoreVtable::QueryAddOnLicensesResult]                  = (VtableEntry_t)&Hook_AddOnLicensesResult;
    vt[StoreVtable::QueryProductsAsync]                        = (VtableEntry_t)&Hook_QueryProductsAsync;
    vt[StoreVtable::QueryProductsResult]                       = (VtableEntry_t)&Hook_QueryProductsResult;
    vt[StoreVtable::QueryEntitledProductsAsync]                = (VtableEntry_t)&Hook_QueryEntitledAsync;
    vt[StoreVtable::QueryEntitledProductsResult]               = (VtableEntry_t)&Hook_QueryEntitledResult;
    vt[StoreVtable::ProductsQueryGetProducts]                  = (VtableEntry_t)&Hook_GetProducts;
    vt[StoreVtable::IsAvailabilityPurchasable]                 = (VtableEntry_t)&Hook_IsAvailabilityPurchasable;
    vt[StoreVtable::QueryAssociatedProductsAsync]              = (VtableEntry_t)&Hook_QueryAssocAsync;
    vt[StoreVtable::QueryAssociatedProductsResult]             = (VtableEntry_t)&Hook_QueryAssocResult;
    vt[StoreVtable::QueryProductForCurrentGameAsync]           = (VtableEntry_t)&Hook_QueryCurrentGameAsync;
    vt[StoreVtable::QueryProductForCurrentGameResult]          = (VtableEntry_t)&Hook_QueryCurrentGameResult;
    vt[StoreVtable::QueryProductForPackageAsync]               = (VtableEntry_t)&Hook_QueryProductForPackageAsync;
    vt[StoreVtable::QueryProductForPackageResult]              = (VtableEntry_t)&Hook_QueryPkgResult;
    vt[StoreVtable::ProductsQueryHasMorePages]                 = (VtableEntry_t)&Hook_HasMorePages;
    vt[StoreVtable::ProductsQueryClose]                        = (VtableEntry_t)&Hook_QueryClose;
    vt[StoreVtable::QueryAssociatedProductsForStoreIdAsync]    = (VtableEntry_t)&Hook_QueryAssocForStoreIdAsync;
    vt[StoreVtable::QueryAssociatedProductsForStoreIdResult]   = (VtableEntry_t)&Hook_QueryAssocStoreIdResult;
    vt[StoreVtable::QueryPackageIdentifier]                    = (VtableEntry_t)&Hook_QueryPackageIdentifier;
    vt[StoreVtable::QueryLicenseTokenAsync]                    = (VtableEntry_t)&Hook_QueryLicenseTokenAsync;
    vt[StoreVtable::QueryLicenseTokenResultSize]               = (VtableEntry_t)&Hook_LicenseTokenResultSize;
    vt[StoreVtable::QueryLicenseTokenResult]                   = (VtableEntry_t)&Hook_LicenseTokenResult;

    VirtualProtect(vt, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE, old, &old);
    g_hookedVtable = vt;
    if (LogOn()) LOG_INFO("store hooks installed (%zu entries)", STORE_VTABLE_SIZE);
}

void StoreHooks::Initialize(const UnlockerConfig& cfg) {
    g_cfg = cfg;
    if (LogOn())
        LOG_INFO("StoreHooks: unlock_all=%d dlcs=%zu blacklist=%zu",
                 cfg.unlockAll, cfg.dlcs.size(), cfg.blacklist.size());
}

void StoreHooks::Shutdown() {
    g_shutdown = true;
    AcquireSRWLockExclusive(&g_hookLock);
    if (g_hookedVtable) {
        DWORD old;
        if (VirtualProtect(g_hookedVtable, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE, PAGE_READWRITE, &old)) {
            memcpy(g_hookedVtable, g_orig, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE);
            VirtualProtect(g_hookedVtable, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE, old, &old);
        }
        g_hookedVtable = nullptr;
    }
    ReleaseSRWLockExclusive(&g_hookLock);

    AcquireSRWLockExclusive(&g_fakeLicenseLock);
    for (void* h : g_fakeLicenses) HeapFree(GetProcessHeap(), 0, h);
    g_fakeLicenses.clear();
    ReleaseSRWLockExclusive(&g_fakeLicenseLock);
}

void StoreHooks::OnStoreInterfaceCreated(void** ppInterface) {
    if (!ppInterface || !*ppInterface) return;
    AcquireSRWLockExclusive(&g_hookLock);
    auto* vt = *(VtableEntry_t**)*ppInterface;
    if (vt != g_hookedVtable) {
        if (g_hookedVtable) {
            DWORD old;
            if (VirtualProtect(g_hookedVtable, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE, PAGE_READWRITE, &old)) {
                memcpy(g_hookedVtable, g_orig, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE);
                VirtualProtect(g_hookedVtable, sizeof(VtableEntry_t) * STORE_VTABLE_SIZE, old, &old);
            }
        }
        InstallHooks(vt);
    }
    ReleaseSRWLockExclusive(&g_hookLock);
}


// Package hooks

static UnlockerConfig    g_pkgCfg;
static VtableEntry_t     g_pkgOrig[PACKAGE_VTABLE_SIZE] = {};
static SRWLOCK           g_pkgHookLock     = SRWLOCK_INIT;
static VtableEntry_t*    g_pkgHookedVtable = nullptr;
static std::atomic<bool> g_pkgShutdown{false};
static inline bool       PkgLogOn() { return g_pkgCfg.logEnabled; }

static std::atomic_bool g_pkgHookFired[PACKAGE_VTABLE_SIZE] = {};
static void PkgLogHookFireOnce(int idx, const char* name) {
    if (!PkgLogOn() || idx < 0 || idx >= (int)PACKAGE_VTABLE_SIZE) return;
    if (!g_pkgHookFired[idx].exchange(true))
        LOG_INFO("pkg hook: vt[%d] %s", idx, name ? name : "?");
}

// Fake package entries
struct FakePackageEntry {
    std::string packageId, displayName, storeId, titleIdStr;

    XPackageDetails ToDetails(uint32_t idx, uint32_t count) const {
        XPackageDetails d = {};
        d.packageIdentifier = packageId.c_str();
        d.version.major     = 1;
        d.kind              = (uint32_t)XPackageKind::Content;
        d.displayName       = displayName.c_str();
        d.description       = "";
        d.publisher         = "";
        d.storeId           = storeId.c_str();
        d.index             = idx;
        d.count             = count;
        d.titleID           = titleIdStr.c_str();
        return d;
    }
};

static std::vector<std::unique_ptr<FakePackageEntry>> g_fakePackages;
static SRWLOCK g_fakePackagesLock = SRWLOCK_INIT;
static std::unordered_set<std::string> g_fakePackageIds;

static int BuildFakePackages() {
    std::unordered_set<std::string> src;
    if (!g_pkgCfg.dlcs.empty()) src = g_pkgCfg.dlcs;
    else if (g_pkgCfg.unlockAll) src = GetDiscoveredStoreIds();
    if (src.empty()) return 0;

    int added = 0;
    AcquireSRWLockExclusive(&g_fakePackagesLock);
    for (const auto& id : src) {
        if (g_pkgCfg.blacklist.count(id) || g_fakePackageIds.count(id)) continue;
        auto e = std::make_unique<FakePackageEntry>();
        e->storeId = id; e->packageId = "FakeDLC." + id + "_unlocker"; e->displayName = "DLC " + id;
        g_fakePackageIds.insert(id);
        g_fakePackages.push_back(std::move(e));
        added++;
    }
    ReleaseSRWLockExclusive(&g_fakePackagesLock);
    if (added > 0 && PkgLogOn()) LOG_INFO("built %d fake packages (total: %zu)", added, g_fakePackages.size());
    return added;
}

// Package enumeration
typedef bool(__fastcall* PkgEnumCb_t)(void*, const XPackageDetails*);
typedef int64_t(__fastcall* PkgEnumFn_t)(void*, uint32_t, uint32_t, void*, void*);

struct PkgEnumCbCtx { PkgEnumCb_t userCb; void* userCtx; std::unordered_set<std::string>* seen; int total; };

static bool __fastcall Hook_PkgEnum_Callback(void* ctx, const XPackageDetails* d) {
    auto* c = reinterpret_cast<PkgEnumCbCtx*>(ctx);
    if (!c || !d) return true;
    c->total++;
    if (d->storeId && d->storeId[0]) c->seen->insert(d->storeId);
    if (d->packageIdentifier && d->packageIdentifier[0]) c->seen->insert(d->packageIdentifier);
    return c->userCb ? c->userCb(c->userCtx, d) : true;
}

static int64_t EnumeratePackagesCommon(int vtIdx, void* self, uint32_t kind, uint32_t scope, void* ctx, void* cb) {
    if (PkgLogOn())
        LOG_INFO("EnumPkgs(vt[%d]): kind=%u(%s) scope=%u",
                 vtIdx, kind, kind == 0 ? "Game" : kind == 1 ? "Content" : "?", scope);

    if (kind != (uint32_t)XPackageKind::Content || !cb)
        return ((PkgEnumFn_t)g_pkgOrig[vtIdx])(self, kind, scope, ctx, cb);

    std::unordered_set<std::string> seen;
    PkgEnumCbCtx c = { (PkgEnumCb_t)cb, ctx, &seen, 0 };

    int64_t hr = ((PkgEnumFn_t)g_pkgOrig[vtIdx])(self, kind, scope, (void*)&c, (void*)&Hook_PkgEnum_Callback);
    if (PkgLogOn()) LOG_INFO("EnumPkgs(vt[%d]): %d real (hr=0x%llX)", vtIdx, c.total, (unsigned long long)hr);

    if (hr >= 0 && (g_pkgCfg.unlockAll || !g_pkgCfg.dlcs.empty())) {
        BuildFakePackages();
        std::vector<const FakePackageEntry*> pending;
        AcquireSRWLockShared(&g_fakePackagesLock);
        for (const auto& f : g_fakePackages)
            if (!seen.count(f->storeId) && !seen.count(f->packageId)) pending.push_back(f.get());
        ReleaseSRWLockShared(&g_fakePackagesLock);

        if (!pending.empty()) {
            uint32_t total = c.total + (uint32_t)pending.size();
            int injected = 0;
            for (const auto* e : pending) {
                XPackageDetails d = e->ToDetails(c.total + injected, total);
                if (PkgLogOn()) LOG_INFO("  inject[%d]: %s (%s)", injected, d.packageIdentifier, d.storeId);
                if (!((PkgEnumCb_t)cb)(ctx, &d)) break;
                injected++;
            }
            if (PkgLogOn()) LOG_INFO("EnumPkgs: injected %d fake DLCs", injected);
        }
    }
    return hr;
}

#define ENUM_HOOK(Name, VtIdx) \
static int64_t __fastcall Hook_##Name(void* self, uint32_t kind, uint32_t scope, void* ctx, void* cb) { \
    if (g_pkgShutdown) return ((PkgEnumFn_t)g_pkgOrig[VtIdx])(self, kind, scope, ctx, cb); \
    PkgLogHookFireOnce(VtIdx, #Name); \
    return EnumeratePackagesCommon(VtIdx, self, kind, scope, ctx, cb); }

ENUM_HOOK(EnumeratePackages,    PackageVtable::EnumeratePackages)
ENUM_HOOK(EnumeratePackages_V5, PackageVtable::EnumeratePackages_V5)
ENUM_HOOK(EnumeratePackages_V7, PackageVtable::EnumeratePackages_V7)
ENUM_HOOK(EnumeratePackages_V8, PackageVtable::EnumeratePackages_V8)
#undef ENUM_HOOK

// Mount hooks
typedef int64_t(__fastcall* MountFn_t)(void*, const char*, void**);
typedef int64_t(__fastcall* MountUiAsyncFn_t)(void*, const char*, void*);
typedef int64_t(__fastcall* MountUiResultFn_t)(void*, void*, void**);
typedef int64_t(__fastcall* GetMountPathSizeFn_t)(void*, void*, uint64_t*);
typedef int64_t(__fastcall* GetMountPathFn_t)(void*, void*, uint64_t, char*);
typedef void(__fastcall* CloseMountFn_t)(void*, void*);

static std::unordered_set<void*> g_fakeMountHandles;
static SRWLOCK g_fakeMountLock = SRWLOCK_INIT;

static void* AllocFakeMountHandle() { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x10); }

static bool IsFakeMountHandle(void* h) {
    AcquireSRWLockShared(&g_fakeMountLock);
    bool f = g_fakeMountHandles.count(h) > 0;
    ReleaseSRWLockShared(&g_fakeMountLock);
    return f;
}

static std::string GetGameDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    return (pos != std::string::npos) ? dir.substr(0, pos) : dir;
}

static int64_t __fastcall Hook_Mount(void* self, const char* pkgId, void** out) {
    if (g_pkgShutdown) return ((MountFn_t)g_pkgOrig[PackageVtable::Mount])(self, pkgId, out);
    PkgLogHookFireOnce(PackageVtable::Mount, "Mount");
    int64_t hr = ((MountFn_t)g_pkgOrig[PackageVtable::Mount])(self, pkgId, out);
    if (hr == 0) return hr;
    if (out) {
        void* fake = AllocFakeMountHandle();
        if (fake) {
            *out = fake;
            AcquireSRWLockExclusive(&g_fakeMountLock); g_fakeMountHandles.insert(fake); ReleaseSRWLockExclusive(&g_fakeMountLock);
            if (PkgLogOn()) LOG_INFO("Mount: FAILED (0x%llX) %s -> fake", (unsigned long long)hr, pkgId ? pkgId : "?");
            return 0;
        }
    }
    return hr;
}

static int64_t __fastcall Hook_MountWithUiAsync(void* self, const char* pkgId, void* async) {
    if (g_pkgShutdown) return ((MountUiAsyncFn_t)g_pkgOrig[PackageVtable::MountWithUiAsync])(self, pkgId, async);
    PkgLogHookFireOnce(PackageVtable::MountWithUiAsync, "MountWithUiAsync");
    if (PkgLogOn()) LOG_INFO("MountWithUiAsync: %s", pkgId ? pkgId : "?");
    return ((MountUiAsyncFn_t)g_pkgOrig[PackageVtable::MountWithUiAsync])(self, pkgId, async);
}

static int64_t __fastcall Hook_MountWithUiResult(void* self, void* async, void** out) {
    if (g_pkgShutdown) return ((MountUiResultFn_t)g_pkgOrig[PackageVtable::MountWithUiResult])(self, async, out);
    PkgLogHookFireOnce(PackageVtable::MountWithUiResult, "MountWithUiResult");
    int64_t hr = ((MountUiResultFn_t)g_pkgOrig[PackageVtable::MountWithUiResult])(self, async, out);
    if (hr == 0) return hr;
    if (out) {
        void* fake = AllocFakeMountHandle();
        if (fake) {
            *out = fake;
            AcquireSRWLockExclusive(&g_fakeMountLock); g_fakeMountHandles.insert(fake); ReleaseSRWLockExclusive(&g_fakeMountLock);
            if (PkgLogOn()) LOG_INFO("MountWithUiResult: FAILED (0x%llX) -> fake", (unsigned long long)hr);
            return 0;
        }
    }
    return hr;
}

static int64_t __fastcall Hook_GetMountPathSize(void* self, void* handle, uint64_t* outSize) {
    if (g_pkgShutdown) return ((GetMountPathSizeFn_t)g_pkgOrig[PackageVtable::GetMountPathSize])(self, handle, outSize);
    PkgLogHookFireOnce(PackageVtable::GetMountPathSize, "GetMountPathSize");
    if (IsFakeMountHandle(handle)) { if (outSize) *outSize = GetGameDirectory().size() + 1; return 0; }
    return ((GetMountPathSizeFn_t)g_pkgOrig[PackageVtable::GetMountPathSize])(self, handle, outSize);
}

static int64_t __fastcall Hook_GetMountPath(void* self, void* handle, uint64_t bufSize, char* buf) {
    if (g_pkgShutdown) return ((GetMountPathFn_t)g_pkgOrig[PackageVtable::GetMountPath])(self, handle, bufSize, buf);
    PkgLogHookFireOnce(PackageVtable::GetMountPath, "GetMountPath");
    if (IsFakeMountHandle(handle)) {
        std::string d = GetGameDirectory();
        if (buf && bufSize > 0) strncpy_s(buf, (size_t)bufSize, d.c_str(), _TRUNCATE);
        if (PkgLogOn()) LOG_INFO("GetMountPath: fake -> %s", d.c_str());
        return 0;
    }
    return ((GetMountPathFn_t)g_pkgOrig[PackageVtable::GetMountPath])(self, handle, bufSize, buf);
}

static void __fastcall Hook_CloseMountHandle(void* self, void* handle) {
    if (g_pkgShutdown) { ((CloseMountFn_t)g_pkgOrig[PackageVtable::CloseMountHandle])(self, handle); return; }
    PkgLogHookFireOnce(PackageVtable::CloseMountHandle, "CloseMountHandle");
    AcquireSRWLockExclusive(&g_fakeMountLock);
    bool wasFake = g_fakeMountHandles.erase(handle) > 0;
    ReleaseSRWLockExclusive(&g_fakeMountLock);
    if (wasFake) { HeapFree(GetProcessHeap(), 0, handle); return; }
    ((CloseMountFn_t)g_pkgOrig[PackageVtable::CloseMountHandle])(self, handle);
}

// Installation progress, force 100%
typedef void(__fastcall* GetProgressFn_t)(void*, void*, void*);

struct XPackageInstallationProgress {
    uint64_t totalBytes, installedBytes, launchBytes;
    bool launchable, completed;
    uint8_t _pad[6];
};
static_assert(sizeof(XPackageInstallationProgress) == 32);

static void __fastcall Hook_GetInstallationProgress(void* self, void* monitor, void* progress) {
    if (g_pkgShutdown) { ((GetProgressFn_t)g_pkgOrig[PackageVtable::GetInstallationProgress])(self, monitor, progress); return; }
    PkgLogHookFireOnce(PackageVtable::GetInstallationProgress, "GetInstallationProgress");
    ((GetProgressFn_t)g_pkgOrig[PackageVtable::GetInstallationProgress])(self, monitor, progress);
    if (progress) {
        auto* p = (XPackageInstallationProgress*)progress;
        if (p->totalBytes == 0) p->totalBytes = 1;
        p->installedBytes = p->totalBytes;
        p->launchBytes    = p->totalBytes;
        p->launchable = p->completed = true;
    }
}

// Package vtable installation
static void InstallPackageHooks(VtableEntry_t* vt) {
    memcpy(g_pkgOrig, vt, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE);

    DWORD old;
    if (!VirtualProtect(vt, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE, PAGE_READWRITE, &old)) {
        LOG_ERROR("VirtualProtect failed for pkg vtable: %lu", GetLastError()); return;
    }

    vt[PackageVtable::EnumeratePackages]       = (VtableEntry_t)&Hook_EnumeratePackages;
    vt[PackageVtable::EnumeratePackages_V5]    = (VtableEntry_t)&Hook_EnumeratePackages_V5;
    vt[PackageVtable::EnumeratePackages_V7]    = (VtableEntry_t)&Hook_EnumeratePackages_V7;
    vt[PackageVtable::EnumeratePackages_V8]    = (VtableEntry_t)&Hook_EnumeratePackages_V8;
    vt[PackageVtable::Mount]                   = (VtableEntry_t)&Hook_Mount;
    vt[PackageVtable::MountWithUiAsync]        = (VtableEntry_t)&Hook_MountWithUiAsync;
    vt[PackageVtable::MountWithUiResult]       = (VtableEntry_t)&Hook_MountWithUiResult;
    vt[PackageVtable::GetMountPathSize]        = (VtableEntry_t)&Hook_GetMountPathSize;
    vt[PackageVtable::GetMountPath]            = (VtableEntry_t)&Hook_GetMountPath;
    vt[PackageVtable::CloseMountHandle]        = (VtableEntry_t)&Hook_CloseMountHandle;
    vt[PackageVtable::GetInstallationProgress] = (VtableEntry_t)&Hook_GetInstallationProgress;

    VirtualProtect(vt, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE, old, &old);
    g_pkgHookedVtable = vt;
    if (PkgLogOn()) LOG_INFO("package hooks installed (%zu entries)", PACKAGE_VTABLE_SIZE);
}

void PackageHooks::Initialize(const UnlockerConfig& cfg) {
    g_pkgCfg = cfg;
    if (PkgLogOn()) LOG_INFO("PackageHooks: unlock_all=%d dlcs=%zu", cfg.unlockAll, cfg.dlcs.size());
}

void PackageHooks::Shutdown() {
    g_pkgShutdown = true;
    AcquireSRWLockExclusive(&g_pkgHookLock);
    if (g_pkgHookedVtable) {
        DWORD old;
        if (VirtualProtect(g_pkgHookedVtable, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE, PAGE_READWRITE, &old)) {
            memcpy(g_pkgHookedVtable, g_pkgOrig, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE);
            VirtualProtect(g_pkgHookedVtable, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE, old, &old);
        }
        g_pkgHookedVtable = nullptr;
    }
    ReleaseSRWLockExclusive(&g_pkgHookLock);

    AcquireSRWLockExclusive(&g_fakeMountLock);
    for (void* h : g_fakeMountHandles) HeapFree(GetProcessHeap(), 0, h);
    g_fakeMountHandles.clear();
    ReleaseSRWLockExclusive(&g_fakeMountLock);
}

void PackageHooks::OnPackageInterfaceCreated(void** ppInterface) {
    if (!ppInterface || !*ppInterface) return;
    AcquireSRWLockExclusive(&g_pkgHookLock);
    auto* vt = *(VtableEntry_t**)*ppInterface;
    if (vt != g_pkgHookedVtable) {
        if (g_pkgHookedVtable) {
            DWORD old;
            if (VirtualProtect(g_pkgHookedVtable, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE, PAGE_READWRITE, &old)) {
                memcpy(g_pkgHookedVtable, g_pkgOrig, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE);
                VirtualProtect(g_pkgHookedVtable, sizeof(VtableEntry_t) * PACKAGE_VTABLE_SIZE, old, &old);
            }
        }
        InstallPackageHooks(vt);
    }
    ReleaseSRWLockExclusive(&g_pkgHookLock);
}


// COM server hooks for IStoreCommonServer

static UnlockerConfig    g_comCfg;
static std::atomic<bool> g_comShutdown{false};
static inline bool       ComLogOn() { return g_comCfg.logEnabled; }

static constexpr size_t COM_VTABLE_MAX = 31;
static VtableEntry_t    g_comOrig[COM_VTABLE_MAX] = {};
static SRWLOCK          g_comHookLock     = SRWLOCK_INIT;
static VtableEntry_t*   g_comHookedVtable = nullptr;
static std::atomic_bool g_comHookFired[COM_VTABLE_MAX] = {};

static void ComLogOnce(int idx, const char* name) {
    if (!ComLogOn() || idx < 0 || idx >= (int)COM_VTABLE_MAX) return;
    if (!g_comHookFired[idx].exchange(true))
        LOG_INFO("[COM] hook: vt[%d] (+%d) %s", idx, idx * 8, name ? name : "?");
}

typedef int64_t(__fastcall* ComValidateLicense_t)(void*, void*, void*, char*);

static int64_t __fastcall Hook_COM_ValidateLicense(void* self, void* hstr, void* cb, char* outBool) {
    ComLogOnce(ComServerVtable::ValidateLicense, "ValidateLicense");
    if (g_comShutdown) return ((ComValidateLicense_t)g_comOrig[ComServerVtable::ValidateLicense])(self, hstr, cb, outBool);
    int64_t hr = ((ComValidateLicense_t)g_comOrig[ComServerVtable::ValidateLicense])(self, hstr, cb, outBool);
    if (outBool) *outBool = 1;
    if (hr < 0) hr = 0;
    if (ComLogOn()) LOG_INFO("[COM] ValidateLicense: forced TRUE");
    return hr;
}

typedef int64_t(__fastcall* ComGeneric4_t)(void*, void*, void*, void*);

static int64_t __fastcall Hook_COM_GetPreviewLicense(void* self, void* a2, void* a3, void* a4) {
    ComLogOnce(ComServerVtable::GetPreviewLicense, "GetPreviewLicense");
    int64_t hr = ((ComGeneric4_t)g_comOrig[ComServerVtable::GetPreviewLicense])(self, a2, a3, a4);
    if (ComLogOn()) LOG_INFO("[COM] GetPreviewLicense: hr=0x%llX", (unsigned long long)hr);
    return hr;
}

typedef int64_t(__fastcall* ComTitleLicense_t)(void*, void*, void*);

static int64_t __fastcall Hook_COM_GetTitleLicense(void* self, void* cv, void* out) {
    ComLogOnce(ComServerVtable::GetTitleLicense, "GetTitleLicense");
    if (g_comShutdown) return ((ComTitleLicense_t)g_comOrig[ComServerVtable::GetTitleLicense])(self, cv, out);
    int64_t hr = ((ComTitleLicense_t)g_comOrig[ComServerVtable::GetTitleLicense])(self, cv, out);
    if (ComLogOn()) LOG_INFO("[COM] GetTitleLicense: hr=0x%llX", (unsigned long long)hr);
    return hr;
}

typedef int64_t(__fastcall* ComUserColl_t)(void*, void*, void*, void*, void*, void*);

static int64_t __fastcall Hook_COM_GetUserCollection(void* self, void* a2, void* a3, void* a4, void* a5, void* a6) {
    ComLogOnce(ComServerVtable::GetUserCollection, "GetUserCollection");
    int64_t hr = ((ComUserColl_t)g_comOrig[ComServerVtable::GetUserCollection])(self, a2, a3, a4, a5, a6);
    if (ComLogOn()) LOG_INFO("[COM] GetUserCollection: hr=0x%llX", (unsigned long long)hr);
    return hr;
}

typedef int64_t(__fastcall* ComQueryProd_t)(void*, void*, uint32_t, void*, void*, void*, void*);

static int64_t __fastcall Hook_COM_QueryProducts(void* self, void* a2, uint32_t a3, void* a4, void* a5, void* a6, void* a7) {
    ComLogOnce(ComServerVtable::QueryProducts, "QueryProducts");
    int64_t hr = ((ComQueryProd_t)g_comOrig[ComServerVtable::QueryProducts])(self, a2, a3, a4, a5, a6, a7);
    if (ComLogOn()) LOG_INFO("[COM] QueryProducts: hr=0x%llX", (unsigned long long)hr);
    return hr;
}

// COM vtable installation
static void InstallComServerHooks(VtableEntry_t* vt) {
    memcpy(g_comOrig, vt, sizeof(VtableEntry_t) * COM_VTABLE_MAX);

    DWORD old;
    if (!VirtualProtect(vt, sizeof(VtableEntry_t) * COM_VTABLE_MAX, PAGE_READWRITE, &old)) {
        LOG_ERROR("[COM] VirtualProtect failed: %lu", GetLastError()); return;
    }

    vt[ComServerVtable::ValidateLicense]   = (VtableEntry_t)&Hook_COM_ValidateLicense;
    vt[ComServerVtable::GetPreviewLicense] = (VtableEntry_t)&Hook_COM_GetPreviewLicense;
    vt[ComServerVtable::GetTitleLicense]   = (VtableEntry_t)&Hook_COM_GetTitleLicense;
    vt[ComServerVtable::GetUserCollection] = (VtableEntry_t)&Hook_COM_GetUserCollection;
    vt[ComServerVtable::QueryProducts]     = (VtableEntry_t)&Hook_COM_QueryProducts;

    VirtualProtect(vt, sizeof(VtableEntry_t) * COM_VTABLE_MAX, old, &old);
    g_comHookedVtable = vt;
    if (ComLogOn()) LOG_INFO("[COM] hooks installed (%zu entries)", COM_VTABLE_MAX);
}

void ComServerHooks::TryHookContext(void* xstoreContext) {
    if (!xstoreContext || g_comShutdown) return;
    AcquireSRWLockExclusive(&g_comHookLock);

    void* factory = ((void**)xstoreContext)[2];  // +16
    if (!factory) {
        ReleaseSRWLockExclusive(&g_comHookLock);
        return;
    }

    typedef HRESULT(__fastcall* QI_t)(void*, const GUID*, void**);
    auto qi = (QI_t)(*(void***)factory)[0];
    void* pServer = nullptr;
    HRESULT hr = qi(factory, &IID_IStoreCommonServer, &pServer);

    if (FAILED(hr) || !pServer) {
        if (ComLogOn()) LOG_INFO("[COM] QI failed: 0x%lX", hr);
        ReleaseSRWLockExclusive(&g_comHookLock);
        return;
    }

    if (ComLogOn()) LOG_INFO("[COM] IStoreCommonServer at %p", pServer);

    auto* vt = *(VtableEntry_t**)pServer;
    if (vt != g_comHookedVtable) {
        if (g_comHookedVtable) {
            DWORD oldProt;
            if (VirtualProtect(g_comHookedVtable, sizeof(VtableEntry_t) * COM_VTABLE_MAX, PAGE_READWRITE, &oldProt)) {
                memcpy(g_comHookedVtable, g_comOrig, sizeof(VtableEntry_t) * COM_VTABLE_MAX);
                VirtualProtect(g_comHookedVtable, sizeof(VtableEntry_t) * COM_VTABLE_MAX, oldProt, &oldProt);
            }
        }
        InstallComServerHooks(vt);
    }

    ((int64_t(__fastcall*)(void*))g_comOrig[ComServerVtable::Release])(pServer);
    ReleaseSRWLockExclusive(&g_comHookLock);
}

void ComServerHooks::Initialize(const UnlockerConfig& cfg) {
    g_comCfg = cfg;
    if (ComLogOn()) LOG_INFO("[COM] init: unlock_all=%d", cfg.unlockAll);
}

void ComServerHooks::Shutdown() {
    g_comShutdown = true;
    AcquireSRWLockExclusive(&g_comHookLock);
    if (g_comHookedVtable) {
        DWORD old;
        if (VirtualProtect(g_comHookedVtable, sizeof(VtableEntry_t) * COM_VTABLE_MAX, PAGE_READWRITE, &old)) {
            memcpy(g_comHookedVtable, g_comOrig, sizeof(VtableEntry_t) * COM_VTABLE_MAX);
            VirtualProtect(g_comHookedVtable, sizeof(VtableEntry_t) * COM_VTABLE_MAX, old, &old);
        }
        g_comHookedVtable = nullptr;
    }
    ReleaseSRWLockExclusive(&g_comHookLock);
}

