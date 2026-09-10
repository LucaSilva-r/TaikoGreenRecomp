// Native port of Zucchini's MIT-licensed tja2fumen writer and note tagging.
// See tools/vendor/tja2fumen/LICENSE.txt for the original license.
#include "taiko_chart_internal.h"
#include "taiko_chart_limits.h"
#include "taiko_chart_data.h"
#include <mbedtls/sha256.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace taiko_chart {
namespace fs = std::filesystem;
fs::path path(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string utf8(const fs::path& p) {
    const auto text = p.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 0 || in.tellg() > 16*1024*1024)
        throw std::runtime_error("chart is missing or exceeds 16 MiB");
    std::string bytes(size_t(in.tellg()), '\0');
    in.seekg(0);
    if (!in.read(bytes.data(), bytes.size())) throw std::runtime_error("cannot read chart");
    return bytes;
}
std::string hash(std::string_view bytes) {
    unsigned char digest[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest, 0))
        throw std::runtime_error("SHA-256 failed");
    std::string out;
    for (auto b : digest) { out += "0123456789abcdef"[b>>4]; out += "0123456789abcdef"[b&15]; }
    return out;
}
std::string revision(std::string_view bytes, int osu_level) {
    std::string recipe(TAIKO_CHART_RECIPE);
    recipe.append(bytes); recipe += char(osu_level);
    return hash(recipe);
}
std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n")-first+1);
}
std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        size_t end = value.find(delimiter, at);
        out.push_back(trim(std::string(value.substr(at, end == value.npos ? value.size()-at : end-at))));
        if (end == value.npos) return out;
        at = end+1;
    }
}
double number(const std::string& value) {
    double v = 0;
    const char* first = value.data();
    const char* end = first + value.size();
    if (first != end && *first == '+') ++first;
    auto result = std::from_chars(first,end,v);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(v)) throw std::runtime_error("invalid chart number: " + value);
    return v;
}
int integer(const std::string& value) {
    int v = 0;
    const char* first = value.data();
    const char* end = first + value.size();
    if (first != end && *first == '+') ++first;
    auto result = std::from_chars(first,end,v);
    if (result.ec != std::errc{} || result.ptr != end) throw std::runtime_error("invalid chart integer: " + value);
    return v;
}
bool combo(int t) { return (t >= 1 && t <= 5) || t == 7 || t == 8 || t == 11 || t == 13; }
static bool small(int t) { return t >= 1 && t <= 5; }
static void tag_notes(Fumen& f) {
    std::map<double,int> counts;
    for (auto& m : f.measures) ++counts[m.bpm];
    const double bpm = std::max_element(counts.begin(), counts.end(), [](auto a, auto b) {return a.second < b.second;})->first;
    const int quarter = int(60000/bpm), eighth = int(30000/bpm);
    for (int b = 0; b < 3; ++b) {
        std::vector<Note*> notes;
        for (auto& m : f.measures) for (auto& n : m.branches[b].notes) if (combo(n.type)) {
            n.absolute = m.offset + n.pos + 240000/m.bpm; notes.push_back(&n);
        }
        std::stable_sort(notes.begin(), notes.end(), [](auto a, auto b) {return a->absolute < b->absolute;});
        std::set<int> diffs;
        for (size_t i = 0; i < notes.size(); ++i) {
            if (i+1 < notes.size()) notes[i]->diff = int(notes[i+1]->absolute - notes[i]->absolute);
            if (notes[i]->diff < quarter) diffs.insert(notes[i]->diff);
        }
        std::vector<std::set<int>> groups(1);
        for (int d : diffs) { if (d < eighth) groups[0].insert(d); else groups.push_back({d}); }
        struct Cluster { std::vector<Note*> notes; bool grouped = false; };
        std::vector<Cluster> clusters;
        for (auto n : notes) clusters.push_back({{n},false});
        for (auto& group : groups) {
            std::vector<Cluster> next; Cluster current{{},true};
            auto flush = [&] { if (!current.notes.empty()) {next.push_back(std::move(current)); current = {{},true};} };
            for (auto& item : clusters) {
                if (item.grouped) {flush(); next.push_back(std::move(item));}
                else if (group.count(item.notes[0]->diff)) current.notes.push_back(item.notes[0]);
                else if (!current.notes.empty()) {current.notes.push_back(item.notes[0]); flush();}
                else next.push_back(std::move(item));
            }
            flush(); clusters = std::move(next);
        }
        for (auto& cluster : clusters) {
            auto& ns = cluster.notes;
            for (auto n : ns) if (small(n->type) && !n->manual) n->type = n->type <= 3 ? 2 : 5;
            bool dons = std::all_of(ns.begin(), ns.end(), [](auto n) {return n->type <= 3;});
            if (dons && ns.size()%2) for (size_t i = 1; i < ns.size(); i += 2) if (!ns[i]->manual) ns[i]->type = 3;
            bool fast4 = ns.size() == 4 && std::all_of(ns.begin(), ns.end()-1, [&](auto n) {return n->diff < eighth;});
            auto n = ns.back();
            if (!fast4 && small(n->type) && !n->manual) n->type = n->type <= 3 ? 1 : 4;
        }
    }
}
void finish(Fumen& f, int notes) {
    if (f.measures.empty() || f.measures.size() > TAIKO_MAX_FUMEN_MEASURES) throw std::runtime_error("chart exceeds the 16384-measure limit");
    f.header[2] = f.course == 0 ? 6000 : f.course < 3 ? 7000 : 8000;
    if (notes > 0 && notes <= 2500) {
        const int l = std::clamp(f.level, 1, 10);
        int col = f.course == 0 ? (l == 1 ? 0 : l < 4 ? 1 : 2) :
                  f.course == 1 ? (l < 3 ? 7 : l == 3 ? 8 : l == 4 ? 9 : 10) :
                  f.course == 2 ? (l < 3 ? 3 : l == 3 ? 4 : l == 4 ? 5 : 6) :
                                  (l < 8 ? 11 : l == 8 ? 12 : 13);
        static const auto hp = [] {
            std::vector<std::array<int,42>> rows;
            auto lines = split(TAIKO_HP_VALUES, '\n');
            for (size_t i = 1; i < lines.size(); ++i) if (!lines[i].empty()) {
                auto vals = split(lines[i], ',');
                if (vals.size() != 42) throw std::runtime_error("invalid embedded HP table");
                std::array<int,42> row;
                for (int j = 0; j < 42; ++j) row[j] = integer(vals[j]);
                rows.push_back(row);
            }
            return rows;
        }();
        for (int i = 0; i < 3; ++i) f.header[3+i] = hp.at(notes-1)[col+14*i];
    }
    f.header[20] = int(f.measures.size());
    tag_notes(f);
}
static std::string serialize(const Fumen& f, unsigned lead) {
    std::string out;
    auto u32 = [&](uint32_t v) {for (int s = 24; s >= 0; s -= 8) out += char(v >> s);};
    auto u16 = [&](int v) {if (v < 0 || v > 65535) throw std::runtime_error("fumen field exceeds 16 bits"); out += char(v>>8); out += char(v);};
    auto real = [&](double v) {if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max()) throw std::runtime_error("invalid fumen timing"); u32(std::bit_cast<uint32_t>(float(v)));};
    for (int i = 0; i < 36; ++i) {
        real(f.course < 2 ? 41.7083358764648 : 25.0250015258789);
        real(f.course < 2 ? 108.441665649414 : 75.075004577637);
        real(f.course < 2 ? 125.125 : 108.441665649414);
    }
    for (int v : f.header) u32(v);
    for (auto& m : f.measures) {
        real(m.bpm); real(m.offset+lead); out += char(m.gogo); out += char(m.barline); u16(0);
        for (int v : m.condition) u32(v);
        u32(0);
        for (auto& b : m.branches) {
            u16(int(b.notes.size())); u16(0); real(b.speed);
            for (auto& n : b.notes) {
                u32(n.type); real(n.pos); u32(0); real(0);
                u16(n.hits ? n.hits : std::min(n.score,65535));
                u16(n.hits ? 0 : int(std::min(int64_t(n.score_diff)*4,int64_t(65535))));
                real(n.duration);
                if (n.type == 6 || n.type == 9) {u32(0); u32(0);}
            }
        }
    }
    return out;
}
void convert(const TaikoCatalogSong& song) {
    if (song.genre == "NIJIIRO") { convert_nijiiro(song); return; }
    auto raw = read(path(song.tja_path));
    const int level = song.genre == "OSU! LAZER" ? song.stars[3] : 0;
    if (revision(raw, level) != song.custom_revision) throw std::runtime_error("chart changed since discovery; restart to refresh the library");
    auto fumens = level ? std::vector<Fumen>{osu_fumen(raw, level)} : tja_fumens(raw);
    double earliest = std::numeric_limits<double>::infinity();
    for (auto& f : fumens) for (auto& m : f.measures) for (auto& b : m.branches) for (auto& n : b.notes)
        earliest = std::min(earliest, m.offset + 240000/m.bpm + n.pos);
    double lead = std::isfinite(earliest) ? std::max(0., std::nearbyint(2000-earliest)) : 0;
    if (lead > 60000) throw std::runtime_error("chart requires more than 60 seconds of lead-in");
    const auto cache = path(song.custom_cache);
    fs::create_directories(cache);
    auto publish = [&](const fs::path& dest, const std::string& bytes) {
        auto tmp = dest; tmp += ".tmp";
        { std::ofstream out(tmp, std::ios::binary|std::ios::trunc);
          if (!out.write(bytes.data(), bytes.size()) || !out.flush()) throw std::runtime_error("cannot write chart cache"); }
        // Preparation is serialized; ready is published last and validated by hash.
        std::error_code ec; fs::remove(dest, ec); fs::rename(tmp, dest);
    };
    std::string marker = song.custom_revision + "\n" + std::to_string(unsigned(lead)) + "\n";
    for (auto& f : fumens) {
        const std::string suffix(1, "enhmx"[f.course]);
        auto bytes = serialize(f, unsigned(lead));
        publish(cache/(suffix+".bin"), bytes);
        marker += suffix + " " + std::to_string(bytes.size()) + " " + hash(bytes) + "\n";
    }
    publish(cache/"ready", marker);
}
}
