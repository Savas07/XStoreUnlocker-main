// config from xstore_unlocker.ini

#pragma once

#include <string>
#include <unordered_set>

struct UnlockerConfig {
    bool unlockAll  = true;
    bool logEnabled = true;
    std::unordered_set<std::string> blacklist;
    std::unordered_set<std::string> dlcs;
};

UnlockerConfig LoadConfig(const std::string& iniPath);
