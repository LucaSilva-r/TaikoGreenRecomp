// Native port of the vendored MIT tja2fumen parser/converter.
// See tools/vendor/tja2fumen/LICENSE.txt for attribution.
#include "taiko_chart_internal.h"
#include "taiko_chart_limits.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace taiko_chart {
namespace {
std::string upper(std::string s) { for (char& c : s) if (c >= 'a' && c <= 'z') c -= 32; return s; }
std::string decode(const std::string& raw) {
    std::string result;
#ifdef _WIN32
    for (unsigned cp : {unsigned(CP_UTF8), 932u}) {
        int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), int(raw.size()), nullptr, 0);
        if (!n) continue;
        std::wstring wide(n, 0);
        MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), int(raw.size()), wide.data(), n);
        if (cp == 932) for (wchar_t& c : wide) {
            // CP932's six compatibility mappings differ from Python's JIS codec.
            switch (c) {
                case 0xff5e: c = 0x301c; break; case 0x2225: c = 0x2016; break;
                case 0xff0d: c = 0x2212; break; case 0xffe0: c = 0x00a2; break;
                case 0xffe1: c = 0x00a3; break; case 0xffe2: c = 0x00ac; break;
            }
        }
        result.resize(WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, nullptr, 0, nullptr, nullptr));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, result.data(), int(result.size()), nullptr, nullptr);
        break;
    }
#else
    for (const char* encoding : {"UTF-8", "SHIFT_JIS"}) {
        iconv_t cd = iconv_open("UTF-8", encoding);
        if (cd == (iconv_t)-1) throw std::runtime_error("chart text decoder unavailable");
        result.resize(raw.size()*4+4);
        char* input = const_cast<char*>(raw.data()); char* output = result.data();
        size_t left = raw.size(), available = result.size();
        size_t status = iconv(cd, &input, &left, &output, &available);
        iconv_close(cd);
        if (status != size_t(-1) && !left) {
            result.resize(result.size()-available);
            if (std::string_view(encoding) == "SHIFT_JIS") {
                // iconv uses JIS-Roman for ASCII 0x5c/0x7e; chart filenames and
                // Python's Shift-JIS codec use backslash and tilde instead.
                for (auto mapping : {std::pair{"\xc2\xa5", "\\"}, std::pair{"\xe2\x80\xbe", "~"}}) {
                    for (size_t pos = 0; (pos = result.find(mapping.first,pos)) != result.npos; ++pos)
                        result.replace(pos,std::char_traits<char>::length(mapping.first),mapping.second);
                }
            }
            break;
        }
        result.clear();
    }
#endif
    if (result.empty() && !raw.empty()) throw std::runtime_error("chart is neither UTF-8 nor Shift-JIS");
    if (result.starts_with("\xef\xbb\xbf")) result.erase(0,3);
    return result;
}
int course_id(std::string value) {
    value = upper(value);
    for (int i = 0; i < 5; ++i)
        if (value == std::to_string(i) || value == std::array{"EASY","NORMAL","HARD","ONI","URA"}[i]) return i;
    if (value == "EDIT") return 4;
    throw std::runtime_error("unsupported TJA course: " + value);
}
struct Course {
    int id = 3, level = 0, score = 0, score_diff = 0;
    std::vector<int> balloons;
    std::vector<std::string> lines;
};
struct Tja {
    double bpm = 0, offset = 0;
    std::map<std::string,std::string> metadata;
    std::array<Course,15> courses;
};
Tja parse(const std::string& raw) {
    Tja out;
    const auto lines = split(decode(raw), '\n');
    bool initial = true;
    for (auto line : lines) {
        if (line.starts_with("#START")) initial = false;
        auto colon = line.find(':');
        if (colon != line.npos && initial) out.metadata.try_emplace(upper(trim(line.substr(0,colon))),trim(line.substr(colon+1)));
    }
    // The global timing values are the first definitions, as in tja2fumen.
    bool bpm = false, offset = false;
    for (auto line : lines) {
        line = trim(line.substr(0,line.find("//")));
        if (!bpm && line.starts_with("BPM:")) {out.bpm = number(trim(line.substr(4))); bpm = true;}
        if (!offset && line.starts_with("OFFSET:")) {out.offset = number(trim(line.substr(7))); offset = true;}
    }
    if (!bpm || out.bpm <= 0) throw std::runtime_error("TJA needs a positive BPM");
    int current = -1, base = -1;
    for (int i = 0; i < 15; ++i) out.courses[i].id = i%5;
    for (auto line : lines) {
        line = trim(line.substr(0,line.find("//")));
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon != line.npos && line[0] != '#') {
            auto key = upper(trim(line.substr(0,colon))), value = trim(line.substr(colon+1));
            if (key == "COURSE") current = base = course_id(value);
            else if (current >= 0) {
                auto& c = out.courses[current];
                if (key == "LEVEL") c.level = std::clamp(integer(value),1,10);
                else if (key == "SCOREINIT") c.score = value.empty() ? 0 : integer(split(value, ',').back());
                else if (key == "SCOREDIFF") c.score_diff = value.empty() ? 0 : integer(split(value, ',').back());
                else if (key == "BALLOON" && !value.empty()) {
                    c.balloons.clear(); for (auto& v : split(value, ',')) if (!v.empty()) c.balloons.push_back(integer(v));
                } else if (key == "STYLE" && value == "Single") current = base;
            }
        } else if (line.starts_with("#START")) {
            if (base < 0) throw std::runtime_error("#START before COURSE");
            auto value = trim(line.substr(6));
            if (value == "P1" || value == "1P" || value == "P2" || value == "2P") {
                current = base + (value.find('1') != value.npos ? 5 : 10);
                out.courses[current] = out.courses[base]; out.courses[current].lines.clear();
            } else if (!value.empty()) throw std::runtime_error("invalid #START player");
            out.courses[current].lines.push_back("#START");
        } else if (current >= 0) out.courses[current].lines.push_back(line);
    }
    for (int i = 0; i < 5; ++i) if (out.courses[i].lines.empty()) out.courses[i] = out.courses[i+5];
    return out;
}
struct Event { std::string command, value; int pos = 0; };
struct RawMeasure { int subdivisions = 0; std::string notes; std::vector<Event> events; };
using Branches = std::array<std::vector<RawMeasure>,3>;
bool splits(const std::string& s) {return s == "BPMCHANGE" || s == "SCROLL" || s == "GOGOSTART" || s == "GOGOEND" || s == "SENOTECHANGE";}
int note_type(char c) {
    static const std::string chars = "0123456789ABCDEFGHI";
    static const int types[] = {0,1,4,7,8,6,9,10,-1,12,11,13,0,6,11,4,13,9,6};
    const auto i = chars.find(c); return i == chars.npos ? 0 : types[i];
}
Branches branches(Course& course) {
    Branches bs;
    for (auto& b : bs) b.resize(1);
    bool branched = std::any_of(course.lines.begin(), course.lines.end(), [](auto& l) {return l.starts_with("#BRANCH");});
    int current = branched ? -1 : 0;
    size_t measure = 0, branchstart = 0;
    std::string condition;
    std::array<std::vector<bool>,3> balloons;
    auto pad = [&](int b, size_t count) { if (bs[b].size() < count) bs[b].resize(count); };
    auto equalize = [&](int b) {
        int longest = b; for (int j = 0; j < 3; ++j) if (bs[j].size() > bs[longest].size()) longest = j;
        while (bs[b].size() < bs[longest].size()) bs[b].push_back(bs[longest][bs[b].size()]);
    };
    auto each = [&](auto fn) {for (int b = 0; b < 3; ++b) if (current == -1 || current == b) fn(b);};
    const std::set<std::string> events{"GOGOSTART","GOGOEND","BARLINEON","BARLINEOFF","DELAY","SCROLL","BPMCHANGE","MEASURE","LEVELHOLD","SENOTECHANGE","SECTION","BRANCHSTART"};
    for (size_t i = 0; i < course.lines.size(); ++i) {
        auto line = course.lines[i];
        if (line[0] != '#') {
            // Also accept multiple comma-separated measures on one source line.
            size_t at = 0;
            for (;;) {
                size_t comma = line.find(',',at); auto notes = line.substr(at, comma == line.npos ? line.size()-at : comma-at);
                each([&](int b) {
                    pad(b, measure+1); bs[b][measure].notes += notes;
                    for (char n : notes) if (n == '7' || n == '9') balloons[b].push_back(current == -1);
                    if (comma != line.npos) bs[b].emplace_back();
                });
                if (comma == line.npos) break;
                ++measure; at = comma+1;
                if (at == line.size()) break;
            }
            continue;
        }
        size_t space = 1;
        while (space < line.size() && ((line[space] >= 'A' && line[space] <= 'Z') ||
               (line[space] >= 'a' && line[space] <= 'z') || (line[space] >= '0' && line[space] <= '9'))) ++space;
        std::string cmd = upper(line.substr(1,space-1));
        std::string value = space < line.size() && (line[space] == ' ' || line[space] == '\t') ? trim(line.substr(space+1)) : "";
        if (events.count(cmd)) {
            int pos = 0; each([&](int b) {pad(b,measure+1); pos = int(bs[b][measure].notes.size());});
            if (cmd == "SECTION") {
                if ((i+1 < course.lines.size() && course.lines[i+1].starts_with("#BRANCHSTART")) || condition.empty()) current = -1;
                else {cmd = "BRANCHSTART"; value = condition;}
            } else if (cmd == "BRANCHSTART") {
                current = -1; condition = value;
                for (int b = 0; b < 3; ++b) equalize(b);
                branchstart = measure;
            }
            each([&](int b) {pad(b,measure+1); bs[b][measure].events.push_back({cmd,value,pos});});
        } else if (cmd == "N" || cmd == "E" || cmd == "M") {
            current = cmd == "N" ? 0 : cmd == "E" ? 1 : 2; measure = branchstart;
        } else if (cmd == "START" || cmd == "END") current = branched ? -1 : 0;
        else if (cmd == "BRANCHEND") current = -1;
    }
    for (auto& b : bs) if (!b.empty() && b.back().notes.empty() && b.back().events.empty()) b.pop_back();
    for (int b = 0; b < 3; ++b) if (!bs[b].empty()) equalize(b);
    for (auto& b : bs) for (auto& m : b) {
        m.subdivisions = int(m.notes.size());
        int pos = 0;
        for (char n : m.notes) if (std::string_view("0123456789ABCDEFGHI").find(n) != std::string_view::npos) {
            if (int t = note_type(n)) m.events.push_back({"NOTE", std::to_string(t), pos});
            ++pos;
        }
        std::stable_sort(m.events.begin(), m.events.end(), [](auto& a, auto& b) {return a.pos < b.pos;});
    }
    // Match the reference's duplication of shared balloons across branches.
    if (std::all_of(balloons.begin(), balloons.end(), [](auto& b) {return !b.empty();})) {
        auto original = course.balloons;
        if (std::all_of(balloons.begin(), balloons.end(), [&](auto& b) {return b.size() == original.size();})) {
            course.balloons.insert(course.balloons.end(), original.begin(), original.end());
            course.balloons.insert(course.balloons.end(), original.begin(), original.end());
        } else {
            size_t total = 0; bool any = false;
            for (auto& b : balloons) {total += b.size(); any |= std::any_of(b.begin(),b.end(),[](bool v){return v;});}
            if (any && original.size() < total) {
                course.balloons.clear(); size_t at = 0; std::vector<int> duplicates;
                auto pop = [&] {return at < original.size() ? original[at++] : 1;};
                for (bool dupe : balloons[0]) {int hits = pop(); course.balloons.push_back(hits); if (dupe) duplicates.push_back(hits);}
                for (int b = 1; b < 3; ++b) {size_t d = 0; for (bool dupe : balloons[b]) course.balloons.push_back(dupe && d < duplicates.size() ? duplicates[d++] : pop());}
            }
        }
    }
    return bs;
}
void align(Branches& bs) {
    if (bs[1].empty() || bs[2].empty()) return;
    using Fraction = std::pair<int,int>;
    for (size_t i = 0; i < bs[0].size(); ++i) {
        std::array<std::map<Fraction,int>,3> counts;
        std::map<Fraction,int> all;
        for (int b = 0; b < 3; ++b) {
            auto& m = bs[b].at(i);
            for (auto& e : m.events) if (splits(e.command) && e.pos && m.subdivisions) {
                int g = std::gcd(e.pos,m.subdivisions);
                ++counts[b][{e.pos/g,m.subdivisions/g}];
            }
            for (auto [f,n] : counts[b]) all[f] = std::max(all[f],n);
        }
        for (int b = 0; b < 3; ++b) {
            auto& m = bs[b][i]; int divisions = std::max(1,m.subdivisions);
            std::vector<Fraction> missing;
            for (auto [f,n] : all) for (int j = counts[b][f]; j < n; ++j) {
                missing.push_back(f);
                int64_t next = int64_t(divisions / std::gcd(divisions,f.second))*f.second;
                if (next > 16000000) throw std::runtime_error("branch subdivisions exceed supported range");
                divisions = int(next);
            }
            if (missing.empty()) continue;
            int factor = divisions/std::max(1,m.subdivisions);
            if (factor > 1) {for (auto& e : m.events) e.pos *= factor; m.subdivisions = divisions;}
            for (auto f : missing) m.events.push_back({"SPLIT","",int(int64_t(f.first)*divisions/f.second)});
            std::stable_sort(m.events.begin(),m.events.end(),[](auto& a,auto& b){return a.pos < b.pos;});
        }
    }
}
struct Segment {
    double bpm = 0, scroll = 1, delay = 0;
    int numerator = 4, denominator = 4, subdivisions = 0, start = 0, end = 0, senote = 0;
    bool gogo = false, barline = true, hold = false;
    char branch = 0;
    std::array<double,2> threshold{};
    std::vector<Event> notes;
};
std::vector<Segment> process(const std::vector<RawMeasure>& branch, double bpm) {
    std::vector<Segment> out; Segment state; state.bpm = bpm;
    for (auto& m : branch) {
        Segment current = state; current.subdivisions = m.subdivisions;
        for (auto& e : m.events) {
            auto& cmd = e.command;
            if (cmd == "NOTE") current.notes.push_back(e);
            else if (cmd == "DELAY") current.delay = number(e.value)*1000;
            else if (cmd == "BRANCHSTART") {
                auto p = split(e.value, ',');
                if (p.size() != 3 || (upper(p[0]) != "P" && upper(p[0]) != "R")) throw std::runtime_error("invalid #BRANCHSTART");
                current.branch = upper(p[0])[0];
                for (int j = 0; j < 2; ++j) current.threshold[j] = number(p[j+1])/(current.branch == 'P' ? 100 : 1);
            } else if (cmd == "LEVELHOLD") current.hold = true;
            else if (cmd == "BARLINEON" || cmd == "BARLINEOFF") current.barline = state.barline = cmd == "BARLINEON";
            else if (cmd == "MEASURE") {
                auto p = split(e.value,'/');
                if (p.size() != 2) continue;
                current.numerator = state.numerator = integer(p[0]); current.denominator = state.denominator = integer(p[1]);
                if (current.denominator <= 0 || current.numerator <= 0) throw std::runtime_error("invalid #MEASURE");
            } else if (splits(cmd) || cmd == "SPLIT") {
                int senote = 0;
                if (cmd == "BPMCHANGE") {state.bpm = number(e.value); if (state.bpm <= 0) throw std::runtime_error("invalid BPM");}
                if (cmd == "SCROLL") state.scroll = number(e.value);
                if (cmd == "GOGOSTART" || cmd == "GOGOEND") state.gogo = cmd == "GOGOSTART";
                if (cmd == "SENOTECHANGE") {senote = integer(e.value); if (senote < 1 || senote > 5) throw std::runtime_error("invalid #SENOTECHANGE");}
                if (e.pos == 0) {
                    if (cmd == "BPMCHANGE") current.bpm = state.bpm;
                    if (cmd == "SCROLL") current.scroll = state.scroll;
                    if (cmd == "GOGOSTART" || cmd == "GOGOEND") current.gogo = state.gogo;
                    if (senote) current.senote = senote;
                } else {
                    current.end = e.pos; out.push_back(std::move(current));
                    current = state; current.start = e.pos; current.subdivisions = m.subdivisions; current.senote = senote;
                }
            }
        }
        current.end = m.subdivisions; out.push_back(std::move(current));
        if (out.size() > TAIKO_MAX_FUMEN_MEASURES) throw std::runtime_error("chart exceeds the 16384-measure limit");
    }
    return out;
}
Fumen build(Tja& tja, Course course) {
    auto bs = branches(course); align(bs);
    std::array<std::vector<Segment>,3> processed;
    for (int b = 0; b < 3; ++b) processed[b] = process(bs[b],tja.bpm);
    Fumen f; f.course = course.id; f.level = course.level;
    f.measures.resize(processed[0].size());
    f.header[0] = !processed[1].empty() && !processed[2].empty();
    std::array<int,3> totals{}; size_t balloon = 0;
    bool any_condition = false, rolls_only = true, percentages_only = true;
    for (int b = 0; b < 3; ++b) {
        if (processed[b].empty()) continue;
        if (processed[b].size() != f.measures.size()) throw std::runtime_error("branches have different measure counts");
        int total_points = 0, measure_points = 0; bool hold = false;
        size_t roll_measure = 0, roll_note = 0; bool rolling = false;
        any_condition = false; rolls_only = true; percentages_only = true;
        for (size_t i = 0; i < f.measures.size(); ++i) {
            auto& s = processed[b][i]; auto& m = f.measures[i];
            m.bpm = s.bpm; m.gogo = s.gogo; m.branches[b].speed = s.scroll;
            int length = s.end-s.start;
            m.duration = (240000/m.bpm)*(double(s.numerator)/s.denominator)*(s.subdivisions ? double(length)/s.subdivisions : 1);
            m.offset = i == 0 ? -tja.offset*1000-240000/m.bpm : f.measures[i-1].end+s.delay+240000/f.measures[i-1].bpm-240000/m.bpm;
            m.end = m.offset+m.duration;
            if (!s.barline || (length < s.subdivisions && s.start != 0)) m.barline = false;
            if (s.branch) {
                for (int j = 0; j < 2; ++j) {
                    double p = s.threshold[j];
                    m.condition[2*b+j] = hold ? (b == 0 || (b == 1 && j == 1) ? 999 : 0) :
                        s.branch == 'P' ? (p > 1 ? 999 : p > 0 ? int(total_points*p) : 0) : int(p);
                }
                total_points = 0; hold = false; any_condition = true;
                rolls_only &= s.branch == 'R' || (s.threshold[0] == 0 && s.threshold[1] == 0) || (s.threshold[0] > 1 && s.threshold[1] > 1);
                percentages_only &= s.branch != 'R';
            }
            total_points += measure_points; measure_points = 0;
            if (s.hold) hold = true;
            for (auto& e : s.notes) {
                if (!length) throw std::runtime_error("note in zero-length measure");
                double pos = m.duration*(double(e.pos-s.start)/length);
                int type = integer(e.value);
                if (type == -1) {
                    if (rolling) {
                        auto& roll = f.measures[roll_measure].branches[b].notes[roll_note];
                        roll.duration = std::trunc(roll.duration + pos - (roll.multimeasure ? 0 : roll.pos));
                        rolling = false;
                    }
                    continue;
                }
                if (type == 12 && rolling) continue;
                Note n; n.type = type; n.pos = pos; n.score = course.score; n.score_diff = course.score_diff;
                if (s.senote) {n.type = s.senote; n.manual = true; s.senote = 0;}
                if (n.type == 6 || n.type == 9 || n.type == 10 || n.type == 12) {
                    rolling = true; roll_measure = i; roll_note = m.branches[b].notes.size();
                    if (n.type == 10 || n.type == 12) n.hits = balloon < course.balloons.size() ? course.balloons[balloon++] : 1;
                }
                if (combo(n.type)) {++totals[b]; measure_points += 20;}
                else if (n.type == 10 || n.type == 12) measure_points += 30;
                m.branches[b].notes.push_back(n);
            }
            if (rolling) {
                auto& roll = f.measures[roll_measure].branches[b].notes[roll_note];
                roll.duration += m.duration - (roll.multimeasure ? 0 : roll.pos); roll.multimeasure = true;
            }
        }
    }
    if (any_condition && rolls_only) for (int index : {9,10,13,14,16,17}) f.header[index] = 0;
    if (any_condition && percentages_only) f.header[12] = f.header[15] = 0;
    for (int b = 1; b < 3; ++b) if (totals[b]) f.header[6+b] = int(65536*(double(totals[0])/totals[b]));
    finish(f, totals[0]); return f;
}
}
TaikoCatalogSong inspect_tja(const std::filesystem::path& source, const std::filesystem::path& root) {
    auto raw = read(source); auto tja = parse(raw); TaikoCatalogSong song;
    auto wave = tja.metadata["WAVE"];
    if (wave.empty()) throw std::runtime_error("missing WAVE audio reference");
    std::replace(wave.begin(),wave.end(),'\\','/');
    auto audio = std::filesystem::weakly_canonical(source.parent_path()/path(wave));
    if (!std::filesystem::is_regular_file(audio)) throw std::runtime_error("missing chart audio: " + wave);
    for (int i = 0; i < 5; ++i) if (!tja.courses[i].lines.empty()) {song.stars[i] = tja.courses[i].level; song.difficulty_mask |= 1<<i;}
    if (!song.difficulty_mask) throw std::runtime_error("no supported solo course");
    auto demo = tja.metadata["DEMOSTART"];
    double preview = demo.empty() ? 0 : std::max(0.,std::nearbyint(number(demo)*1000));
    if (preview > UINT32_MAX) throw std::runtime_error("DEMOSTART exceeds supported range");
    song.preview_ms = uint32_t(preview);
    song.title = tja.metadata["TITLE"]; if (song.title.empty()) song.title = utf8(source.stem());
    song.custom_subtitle = tja.metadata["SUBTITLE"];
    if (song.custom_subtitle.starts_with("--") || song.custom_subtitle.starts_with("++")) song.custom_subtitle.erase(0,2);
    auto relative = utf8(source.lexically_relative(root));
    std::replace(relative.begin(),relative.end(),'\\','/');
    song.music_id = "tc" + hash(relative).substr(0,12);
    song.tja_path = utf8(std::filesystem::canonical(source)); song.audio_path = utf8(audio);
    song.custom_revision = revision(raw);
    return song;
}
std::vector<Fumen> tja_fumens(const std::string& raw) {
    auto tja = parse(raw); std::vector<Fumen> out;
    for (int i = 0; i < 5; ++i) if (!tja.courses[i].lines.empty()) out.push_back(build(tja,tja.courses[i]));
    if (out.empty()) throw std::runtime_error("no supported TJA courses");
    return out;
}
}
