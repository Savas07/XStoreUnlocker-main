#include "proxy.h"
#include "logger.h"
#include <string>
#include <mutex>

static HMODULE g_module  = nullptr;
static HMODULE g_realDll = nullptr;
static bool g_ok = false;
static std::once_flag g_flag;

static QueryApiImpl_t          g_QueryApiImpl        = nullptr;
static InitializeApiImpl_t     g_InitializeApiImpl    = nullptr;
static InitializeApiImplEx_t   g_InitializeApiImplEx  = nullptr;
static InitializeApiImplEx2_t  g_InitializeApiImplEx2 = nullptr;
static UninitializeApiImpl_t   g_UninitializeApiImpl   = nullptr;
static DllCanUnloadNow_t       g_DllCanUnloadNow       = nullptr;
static XErrorReport_t          g_XErrorReport           = nullptr;

static std::string GetDllDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(g_module, buf, MAX_PATH);
    std::string path(buf);
    auto pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(0, pos) : path;
}

static void DoInit() {
    std::string dllPath = GetDllDir() + "\\XGameRuntime_o.dll";
    LOG_INFO("Loading real DLL: %s", dllPath.c_str());

    g_realDll = LoadLibraryA(dllPath.c_str());
    if (!g_realDll) {
        LOG_ERROR("Failed to load XGameRuntime_o.dll (err=%lu)", GetLastError());
        return;
    }

    g_QueryApiImpl        = (QueryApiImpl_t)       GetProcAddress(g_realDll, "QueryApiImpl");
    g_InitializeApiImpl   = (InitializeApiImpl_t)  GetProcAddress(g_realDll, "InitializeApiImpl");
    g_InitializeApiImplEx = (InitializeApiImplEx_t)GetProcAddress(g_realDll, "InitializeApiImplEx");
    g_InitializeApiImplEx2= (InitializeApiImplEx2_t)GetProcAddress(g_realDll, "InitializeApiImplEx2");
    g_UninitializeApiImpl = (UninitializeApiImpl_t)GetProcAddress(g_realDll, "UninitializeApiImpl");
    g_DllCanUnloadNow     = (DllCanUnloadNow_t)    GetProcAddress(g_realDll, "DllCanUnloadNow");
    g_XErrorReport        = (XErrorReport_t)       GetProcAddress(g_realDll, "XErrorReport");

    if (!g_QueryApiImpl) {
        LOG_ERROR("QueryApiImpl not found in real DLL.");
        return;
    }

    LOG_INFO("Real DLL loaded at 0x%p. All exports resolved.", g_realDll);
    g_ok = true;
}

void Proxy::SetOurModule(HMODULE m) { g_module = m; }

bool Proxy::EnsureInitialized() {
    std::call_once(g_flag, DoInit);
    return g_ok;
}

void Proxy::Shutdown(bool processTerminating) {
    if (g_realDll && !processTerminating) {
        FreeLibrary(g_realDll);
    }
    g_realDll = nullptr;
}

QueryApiImpl_t          Proxy::GetReal_QueryApiImpl()          { return g_QueryApiImpl; }
InitializeApiImpl_t     Proxy::GetReal_InitializeApiImpl()     { return g_InitializeApiImpl; }
InitializeApiImplEx_t   Proxy::GetReal_InitializeApiImplEx()   { return g_InitializeApiImplEx; }
InitializeApiImplEx2_t  Proxy::GetReal_InitializeApiImplEx2()  { return g_InitializeApiImplEx2; }
UninitializeApiImpl_t   Proxy::GetReal_UninitializeApiImpl()   { return g_UninitializeApiImpl; }
DllCanUnloadNow_t       Proxy::GetReal_DllCanUnloadNow()       { return g_DllCanUnloadNow; }
XErrorReport_t          Proxy::GetReal_XErrorReport()          { return g_XErrorReport; }
