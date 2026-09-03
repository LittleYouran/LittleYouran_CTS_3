#pragma once

#include "Utils.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

class GamePackageFile {
public:
    static constexpr const char* path =
        "/storage/emulated/0/Android/CTS/game_packages.md";

    static bool write(const std::vector<std::string>& first,
                      const std::vector<std::string>& second) {
        std::unordered_set<std::string> unique(first.begin(), first.end());
        unique.insert(second.begin(), second.end());
        if (unique.empty()) {
            unlink(path);
            unlink((std::string(path) + ".tmp").c_str());
            return false;
        }

        std::vector<std::string> packages(unique.begin(), unique.end());
        std::sort(packages.begin(), packages.end());
        std::string content;
        for (const auto& package : packages) content += package + '\n';

        mkdir("/storage/emulated/0/Android/CTS", 0775);
        return Utils::writeFileAtomic(path, content);
    }

    static bool contains(const std::string& package) {
        if (package.empty()) return false;
        const size_t suffix = package.find(':');
        const std::string base = suffix == std::string::npos ? package : package.substr(0, suffix);
        std::ifstream input(path);
        if (!input) return false;
        std::string line;
        while (std::getline(input, line)) {
            if (line == package || line == base) return true;
        }
        return false;
    }
};
