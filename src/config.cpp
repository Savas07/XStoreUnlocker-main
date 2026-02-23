#include "config.h"
#include <Windows.h>

static void WriteDefaultConfig(const std::string& path) {
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    const char content[] =
        "; XStoreAPI Unlocker v2.1.0\r\n"
        "; By ZephKek\r\n"
        "; Drop this file next to XGameRuntime.dll in the game folder.\r\n"
        "\r\n"
        "[Settings]\r\n"
        "; unlock_all patches every product the game queries from the store.\r\n"
        "; Works for most DLCs but NOT for unlisted/promo DLCs the game never queries.\r\n"
        "; For unlisted DLCs add their store IDs to [DLCs] below.\r\n"
        "unlock_all=1\r\n"
        "\r\n"
        "; Enable logging to xstore_unlocker.log and OutputDebugString.\r\n"
        "log_enabled=1\r\n"
        "\r\n"
        "[Blacklist]\r\n"
        "; Store IDs to skip. Format: STOREID=1\r\n"
        "\r\n"
        "[DLCs]\r\n"
        "; Force-own these store IDs even if the game never queries the store for them.\r\n"
        "; Fake products get injected into query results so the game sees them as owned.\r\n"
        "; Get IDs from dbox.tools or the MS Store page. Format: STOREID=1\r\n"
        ";\r\n"
        "; Example (Forza Horizon 5 - all durables from DBox):\r\n"
        "; 9NK05Z68D85T=1\r\n"
        "; 9P2BX03TW83N=1\r\n"
        "; 9N6F78CNKF3L=1\r\n"
        "; 9PNB6L2L9RW8=1\r\n"
        "; 9NKTBXVQ1FLF=1\r\n";

    DWORD written;
    WriteFile(hFile, content, sizeof(content) - 1, &written, nullptr);
    CloseHandle(hFile);
}

static void ReadSection(const char* section, const char* path,
                        std::unordered_set<std::string>& out) {
    char buf[32768] = {};
    DWORD len = GetPrivateProfileSectionA(section, buf, sizeof(buf), path);
    const char* p = buf;
    while (p < buf + len && *p) {
        std::string entry(p);
        auto eq = entry.find('=');
        if (eq != std::string::npos && eq > 0) {
            out.insert(entry.substr(0, eq));
        }
        p += entry.size() + 1;
    }
}

UnlockerConfig LoadConfig(const std::string& iniPath) {
    UnlockerConfig cfg;

    if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaultConfig(iniPath);
        return cfg;
    }

    const char* path = iniPath.c_str();

    cfg.unlockAll  = GetPrivateProfileIntA("Settings", "unlock_all", 1, path) != 0;
    cfg.logEnabled = GetPrivateProfileIntA("Settings", "log_enabled", 1, path) != 0;

    ReadSection("Blacklist", path, cfg.blacklist);
    ReadSection("DLCs", path, cfg.dlcs);

    return cfg;
}
