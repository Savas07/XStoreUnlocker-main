#pragma once

#include <cstdint>
#include <Windows.h>

//TODO: dig a bit deeper into 83-84
constexpr size_t STORE_VTABLE_SIZE = 85;

constexpr size_t STORE_SKU_ID_SIZE           = 18;
constexpr size_t IN_APP_OFFER_TOKEN_MAX_SIZE = 64;
constexpr size_t TRIAL_UNIQUE_ID_MAX_SIZE    = 64;
constexpr size_t PRICE_MAX_SIZE              = 16;

constexpr uint32_t PRODUCT_KIND_DURABLE = 0x02;

struct XStoreCanAcquireLicenseResult {
    char     licensableSku[8];
    uint32_t status;            // 0 = licensable
};
static_assert(sizeof(XStoreCanAcquireLicenseResult) == 12);

struct XStoreGameLicense {
    char     skuStoreId[STORE_SKU_ID_SIZE];
    bool     isActive;
    bool     isTrialOwnedByThisUser;
    bool     isDiscLicense;
    bool     isTrial;
    uint8_t  _pad0[2];
    uint32_t trialTimeRemainingInSeconds;
    char     trialUniqueId[TRIAL_UNIQUE_ID_MAX_SIZE];
    uint8_t  _pad1[4];
    int64_t  expirationDate;
};
static_assert(sizeof(XStoreGameLicense) == 104);

struct XStoreAddonLicense {
    char     skuStoreId[STORE_SKU_ID_SIZE];
    char     inAppOfferToken[IN_APP_OFFER_TOKEN_MAX_SIZE];
    bool     isActive;
    uint8_t  _pad0[5];
    int64_t  expirationDate;
};
static_assert(sizeof(XStoreAddonLicense) == 96);

typedef int64_t(__fastcall* VtableEntry_t)(void*, ...);

namespace StoreVtable {
    // IUnknown
    constexpr int QueryInterface                              = 0;
    constexpr int AddRef                                      = 1;
    constexpr int Release                                     = 2;

    // IStoreImpl1
    constexpr int CreateContext                                = 3;
    constexpr int CloseContext                                 = 4;
    constexpr int QueryAssociatedProductsAsync                 = 5;
    constexpr int QueryAssociatedProductsResult                = 6;
    constexpr int QueryProductsAsync                           = 7;
    constexpr int QueryProductsResult                          = 8;
    constexpr int QueryEntitledProductsAsync                   = 9;
    constexpr int QueryEntitledProductsResult                  = 10;
    constexpr int QueryProductForCurrentGameAsync              = 11;
    constexpr int QueryProductForCurrentGameResult             = 12;
    constexpr int QueryProductForPackageAsync                  = 13;
    constexpr int QueryProductForPackageResult                 = 14;
    constexpr int ProductsQueryGetProducts                     = 15;
    constexpr int ProductsQueryHasMorePages                    = 16;
    constexpr int ProductsQueryNextPageAsync                   = 17;
    constexpr int ProductsQueryNextPageResult                  = 18;
    constexpr int ProductsQueryClose                           = 19;
    constexpr int AcquireLicenseForPackageAsync                = 20;
    constexpr int AcquireLicenseForPackageResult               = 21;
    constexpr int LicenseIsValid                               = 22;
    constexpr int CloseLicenseHandle                           = 23;
    constexpr int CanAcquireLicenseForStoreIdAsync             = 24;
    constexpr int CanAcquireLicenseForStoreIdResult            = 25;
    constexpr int CanAcquireLicenseForPackageAsync             = 26;
    constexpr int CanAcquireLicenseForPackageResult            = 27;
    constexpr int QueryGameLicenseAsync                        = 28;
    constexpr int QueryGameLicenseResult                       = 29;
    constexpr int QueryAddOnLicensesAsync                      = 30;
    constexpr int QueryAddOnLicensesResultCount                = 31;
    constexpr int QueryAddOnLicensesResult                     = 32;
    constexpr int QueryConsumableBalanceAsync                  = 33;
    constexpr int QueryConsumableBalanceResult                 = 34;
    constexpr int ReportConsumableFulfillmentAsync              = 35;
    constexpr int ReportConsumableFulfillmentResult             = 36;
    constexpr int GetUserCollectionsIdAsync                     = 37;
    constexpr int GetUserCollectionsIdResultSize                = 38;
    constexpr int GetUserCollectionsIdResult                    = 39;
    constexpr int GetUserPurchaseIdAsync                        = 40;
    constexpr int GetUserPurchaseIdResultSize                   = 41;
    constexpr int GetUserPurchaseIdResult                       = 42;
    constexpr int QueryLicenseTokenAsync                        = 43;
    constexpr int QueryLicenseTokenResultSize                   = 44;
    constexpr int QueryLicenseTokenResult                       = 45;
    constexpr int SendRequestAsync                              = 46;
    constexpr int SendRequestResultSize                         = 47;
    constexpr int SendRequestResult                             = 48;
    constexpr int ShowPurchaseUIAsync                           = 49;
    constexpr int ShowPurchaseUIResult                          = 50;
    constexpr int ShowRateAndReviewUIAsync                      = 51;
    constexpr int ShowRateAndReviewUIResult                     = 52;
    constexpr int ShowRedeemTokenUIAsync                        = 53;
    constexpr int ShowRedeemTokenUIResult                       = 54;

    // IStoreImpl2
    constexpr int QueryGameAndDlcPackageUpdatesAsync            = 55;
    constexpr int QueryGameAndDlcPackageUpdatesResultCount      = 56;
    constexpr int QueryGameAndDlcPackageUpdatesResult           = 57;
    constexpr int DownloadPackageUpdatesAsync                   = 58;
    constexpr int DownloadPackageUpdatesResult                  = 59;
    constexpr int DownloadAndInstallPackageUpdatesAsync         = 60;
    constexpr int DownloadAndInstallPackageUpdatesResult        = 61;
    constexpr int DownloadAndInstallPackagesAsync               = 62;
    constexpr int DownloadAndInstallPackagesResultCount         = 63;
    constexpr int DownloadAndInstallPackagesResult              = 64;

    // IStoreImpl3
    constexpr int QueryPackageIdentifier                        = 65;
    constexpr int RegisterGameLicenseChanged                    = 66;
    constexpr int UnregisterGameLicenseChanged                  = 67;
    constexpr int RegisterPackageLicenseLost                    = 68;
    constexpr int UnregisterPackageLicenseLost                  = 69;

    // IStoreImpl4
    constexpr int IsAvailabilityPurchasable                     = 70;
    constexpr int AcquireLicenseForDurablesAsync                = 71;
    constexpr int AcquireLicenseForDurablesResult               = 72;
    constexpr int ShowAssociatedProductsUIAsync                 = 73;
    constexpr int ShowAssociatedProductsUIResult                = 74;
    constexpr int ShowProductPageUIAsync                        = 75;
    constexpr int ShowProductPageUIResult                       = 76;

    // IStoreImpl5
    constexpr int QueryAssociatedProductsForStoreIdAsync        = 77;
    constexpr int QueryAssociatedProductsForStoreIdResult       = 78;
    constexpr int QueryPackageUpdatesAsync                      = 79;
    constexpr int QueryPackageUpdatesResultCount                = 80;
    constexpr int QueryPackageUpdatesResult                     = 81;

    // IStoreImpl6
    constexpr int Unknown82                                     = 82;
    constexpr int Unknown83                                     = 83;
    constexpr int Unknown84                                     = 84;
}

// === Package vtable (42 entries) ===
constexpr size_t PACKAGE_VTABLE_SIZE = 42;

enum class XPackageKind : uint32_t {
    Game    = 0,
    Content = 1,
};

struct XVersion {
    union {
        struct {
            uint16_t major;
            uint16_t minor;
            uint16_t build;
            uint16_t revision;
        };
        uint64_t Value;
    };
};
static_assert(sizeof(XVersion) == 8);

struct XPackageDetails {
    const char* packageIdentifier;
    XVersion    version;
    uint32_t    kind;
    uint8_t     _pad_kind[4];
    const char* displayName;
    const char* description;
    const char* publisher;
    const char* storeId;
    bool        installing;
    uint8_t     _pad0[3];
    uint32_t    index;
    uint32_t    count;
    bool        ageRestricted;
    uint8_t     _pad1[3];
    const char* titleID;
};
static_assert(sizeof(XPackageDetails) == 80);

namespace PackageVtable {
    // IUnknown
    constexpr int QueryInterface                        = 0;
    constexpr int AddRef                                = 1;
    constexpr int Release                               = 2;

    // IPackageImpl1
    constexpr int GetCurrentProcessPackageIdentifier    = 3;
    constexpr int IsPackagedProcess                     = 4;
    constexpr int CreateInstallationMonitor             = 5;
    constexpr int CloseInstallationMonitorHandle        = 6;
    constexpr int GetInstallationProgress               = 7;
    constexpr int UpdateInstallationMonitor             = 8;
    constexpr int RegisterInstallationProgressChanged   = 9;
    constexpr int UnregisterInstallationProgressChanged = 10;
    constexpr int GetUserLocale                         = 11;
    constexpr int FindChunkAvailability                 = 12;
    constexpr int EnumerateChunkAvailability            = 13;
    constexpr int ChangeChunkInstallOrder               = 14;
    constexpr int InstallChunks                         = 15;
    constexpr int InstallChunksAsync                    = 16;
    constexpr int InstallChunksResult                   = 17;
    constexpr int EstimateDownloadSize                  = 18;
    constexpr int UninstallChunks                       = 19;

    // IPackageImpl2
    constexpr int EnumeratePackages                     = 20;
    constexpr int RegisterPackageInstalled              = 21;
    constexpr int UnregisterPackageInstalled            = 22;

    // IPackageImpl3
    constexpr int Mount                                 = 23;
    constexpr int GetMountPathSize                      = 24;
    constexpr int GetMountPath                          = 25;
    constexpr int CloseMountHandle                      = 26;

    // IPackageImpl4
    constexpr int GetIdentifier                         = 27;

    // IPackageImpl5 (adds new overloads)
    constexpr int EnumeratePackages_V5                  = 28;
    constexpr int RegisterPackageInstalled_V5           = 29;

    // IPackageImpl6
    constexpr int GetWriteStats                         = 30;
    constexpr int EnumerateFeatures                     = 31;
    constexpr int UninstallUWPInstance                   = 32;
    constexpr int EnumerateFeatures_V6                  = 33;
    constexpr int UninstallPackage                      = 34;

    // IPackageImpl7
    constexpr int EnumeratePackages_V7                  = 35;
    constexpr int RegisterPackageInstalled_V7           = 36;

    // IPackageImpl8
    constexpr int MountWithUiAsync                      = 37;
    constexpr int MountWithUiResult                     = 38;
    constexpr int EnumeratePackages_V8                  = 39;
    constexpr int RegisterPackageInstalled_V8           = 40;

    // IPackageImpl9
    constexpr int Unknown41                             = 41;
}

// === IStoreCommonServer COM vtable (31 entries) ===
static const GUID IID_IStoreCommonServer = {
    0x2ADD5417, 0xA2B7, 0x4596,
    { 0x8E, 0x90, 0xBF, 0x53, 0x79, 0xF1, 0xCF, 0xA1 }
};

static const GUID IID_IStoreServerQuery = {
    0xE7AFD7CD, 0x5E9E, 0x4CED,
    { 0x83, 0x4B, 0xD9, 0x59, 0xC8, 0x42, 0xB2, 0x68 }
};

static const GUID IID_IStoreProductServer = {
    0xE2FCC7C1, 0x3BFC, 0x5A0B,
    { 0xB2, 0xB0, 0x72, 0xE7, 0x69, 0xD1, 0xCB, 0x7E }
};

namespace ComServerVtable {
    constexpr int QueryInterface         = 0;
    constexpr int AddRef                 = 1;
    constexpr int Release                = 2;
    constexpr int GetSandboxId           = 6;
    constexpr int GetXToken              = 7;
    constexpr int GetLicenseToken        = 8;
    constexpr int ValidateLicense        = 9;
    constexpr int ReportConsumable       = 11;
    constexpr int GetConsumableBalance   = 13;
    constexpr int GetCollectionsId       = 14;
    constexpr int GetPurchaseId          = 15;
    constexpr int GetPreviewLicense      = 19;
    constexpr int GetStoreProducts       = 20;
    constexpr int GetTitleLicense        = 23;
    constexpr int GetUserCollection      = 24;
    constexpr int RequestRateAndReview   = 26;
    constexpr int QueryProducts          = 27;
    constexpr int GetAssociatedProducts  = 28;
    constexpr int DownloadPkgUpdates     = 29;
    constexpr int DownloadPackages       = 30;
    constexpr int VTABLE_SIZE            = 31;
}

constexpr size_t XSTORECONTEXT_OFF_SERVER = 16;
typedef HRESULT(__cdecl* QueryApiImpl_t)(const GUID*, const GUID*, void**);
typedef HRESULT(__cdecl* InitializeApiImpl_t)(uint64_t, uint64_t);
typedef HRESULT(__cdecl* InitializeApiImplEx_t)(uint64_t, uint64_t, int64_t);
typedef HRESULT(__cdecl* InitializeApiImplEx2_t)(uint64_t, uint64_t, int64_t, int64_t);
typedef HRESULT(__cdecl* UninitializeApiImpl_t)(void);
typedef HRESULT(__cdecl* DllCanUnloadNow_t)(void);
typedef void(__cdecl* XErrorReport_t)(uint64_t, const char*);

