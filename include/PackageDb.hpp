#pragma once

#include "Utils.hpp"
#include "Json/json.h"

class PackageDb {
private:
    static constexpr const char* sqlitePath =
        "/data/adb/modules/LittleYouran/tool/sqlite3";

public:
    static bool query(const char* database, const char* sql, Utils& utils,
                      std::vector<char>& output) {
        char command[512];
        FastSnprintf(command, sizeof(command), "%s %s \"%s\"",
                     sqlitePath, database, sql);

        output.assign(131072, 0);
        const size_t len = utils.popenRead(command, output.data(), output.size() - 1);
        if (len == 0) {
            output.clear();
            return false;
        }
        output.resize(len);
        return true;
    }

    static bool parseGameList(const std::vector<char>& output,
                              std::vector<std::string>& packages) {
        if (output.empty()) return false;

        qlib::json_view_t json;
        if (qlib::json::parse(&json, output.data(), output.data() + output.size()) != 0) {
            return false;
        }

        try {
            const auto& list = json["game_list"];
            if (list.empty()) return false;

            std::vector<std::string> parsed;
            std::unordered_set<std::string> seen;
            for (const auto& item : list.array()) {
                const auto package = item.get<qlib::string_view_t>();
                if (package.empty()) continue;

                std::string value(package.data(), package.size());
                if (seen.insert(value).second) parsed.emplace_back(std::move(value));
            }
            if (parsed.empty()) return false;

            packages = std::move(parsed);
            return true;
        } catch (const qlib::exception&) {
            return false;
        }
    }
};
