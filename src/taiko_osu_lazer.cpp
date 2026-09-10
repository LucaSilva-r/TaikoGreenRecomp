#include "taiko_chart.h"
#include <realm/db.hpp>
#include <realm/history.hpp>
#include <realm/list.hpp>
#include <realm/obj.hpp>
#include <realm/transaction.hpp>
#include <realm/unicode.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>

namespace taiko_chart {
namespace {
namespace fs = std::filesystem;
std::string env(const char* key) {auto p = std::getenv(key); return p ? p : "";}
std::string trim_path(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n");
    return first == value.npos ? "" : value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
fs::path library_root() {
    auto configured = env("TAIKO_OSU_LAZER");
    if (configured == "0") return {};
    std::vector<fs::path> candidates;
    if (!configured.empty()) candidates.push_back(path(configured));
    else {
        auto home = path(env("HOME").empty() ? env("USERPROFILE") : env("HOME"));
        auto xdg = env("XDG_DATA_HOME"), appdata = env("APPDATA");
        candidates = {(xdg.empty() ? home/".local/share" : path(xdg))/"osu",
                      home/".var/app/sh.ppy.osu/data/osu",
                      (appdata.empty() ? home/"AppData/Roaming" : path(appdata))/"osu"};
    }
    for (auto root : candidates) {
        if (root.filename() == "client.realm") root = root.parent_path();
        for (int i = 0; i < 4; ++i) {
            std::ifstream in(root/"storage.ini"); std::string line, target;
            while (std::getline(in,line)) {
                if (line.starts_with("\xef\xbb\xbf")) line.erase(0,3);
                auto eq = line.find('=');
                if (eq != line.npos && trim_path(line.substr(0,eq)) == "FullPath") target = trim_path(line.substr(eq+1));
            }
            if (target.empty() || path(target) == root) break;
            root = path(target);
        }
        if (fs::is_regular_file(root/"client.realm")) return fs::canonical(root);
    }
    if (!configured.empty()) throw std::runtime_error("configured osu!lazer folder has no client.realm");
    return {};
}
std::string filename(std::string s) {
    std::replace(s.begin(),s.end(),'\\','/');
    // Normalize separators and use Realm's UTF-8 case mapping for lookup.
    auto folded = realm::case_map(s, false);
    return folded ? *folded : s;
}
std::string string(const realm::Obj& obj, const char* name) {
    return std::string(obj.get<realm::StringData>(name));
}
fs::path asset(const fs::path& root, const std::string& hash) {
    if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdefABCDEF") != hash.npos)
        throw std::runtime_error("invalid lazer asset hash");
    return root/"files"/hash.substr(0,1)/hash.substr(0,2)/hash;
}
}
void scan_lazer(std::vector<TaikoCatalogSong>& songs) {
    auto root = library_root(); if (root.empty()) return;
    realm::DBOptions options;
    options.no_create = true;
    options.allow_file_format_upgrade = false;
    options.backup_at_file_format_change = false;
    options.to_be_deleted.clear();     // Never clean up osu's backup files.
    // Participate in Realm's interprocess lock/version protocol, then hold only
    // a read transaction. Immutable/Group reads cannot safely coexist with osu
    // saving new beatmaps. No write transaction or schema migration is issued.
    auto db = realm::DB::create(realm::make_in_realm_history(),utf8(root/"client.realm"),options);
    auto snapshot = db->start_read();
    auto sets = snapshot->get_table("class_BeatmapSet");
    if (!sets) throw std::runtime_error("osu Realm has no BeatmapSet table");
    size_t resolved = 0, skipped = 0;
    for (auto set : *sets) {
        if (set.get<bool>("DeletePending")) continue;
        std::map<std::string,fs::path> files;
        auto usages = set.get_linklist("Files");
        for (size_t i = 0; i < usages.size(); ++i) {
            auto usage = usages.get_object(i);
            auto file = usage.get_linked_object("File");
            if (!file) continue;
            files[filename(string(usage,"Filename"))] = asset(root,string(file,"Hash"));
        }
        auto maps = set.get_linklist("Beatmaps");
        auto id = set.get<realm::UUID>("ID").to_string();
        for (size_t i = 0; i < maps.size(); ++i) {
            auto map = maps.get_object(i);
            if (map.get<bool>("Hidden")) continue;
            auto rules = map.get_linked_object("Ruleset");
            if (!rules || rules.get<int64_t>("OnlineID") != 1) continue;
            auto metadata = map.get_linked_object("Metadata"); if (!metadata) continue;
            auto audio = files.find(filename(string(metadata,"AudioFile")));
            if (audio == files.end()) continue;
            try {
                auto hash = string(map,"Hash"); auto source = asset(root,hash);
                if (!fs::is_regular_file(source) || !fs::is_regular_file(audio->second)) continue;
                ++resolved;
                songs.push_back(inspect_osu(source,audio->second,hash,id,map.get<double>("StarRating")));
            } catch (const std::exception& e) {
                if (++skipped <= 5) std::fprintf(stderr,"[osu_lazer] skipped chart: %s\n",e.what());
            }
        }
    }
    std::fprintf(stderr,"[osu_lazer] resolved %zu native taiko charts (%zu skipped)\n",resolved,skipped);
}
}
