// Native port of Zucchini-connector's osu!taiko conversion recipe.
#include "taiko_chart_internal.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace taiko_chart {
namespace {
struct Timing {double offset, beat; int meter; bool red, kiai;};
struct Osu {
    int mode = 0;
    double slider = 1.4;
    std::map<std::string,std::string> metadata;
    std::vector<Timing> timing;
    std::vector<std::vector<std::string>> objects;
};
struct Hit {double start, duration = 0; int type, hits = 0;};
Osu parse(const std::string& raw) {
    Osu out; std::string section;
    auto text = raw.starts_with("\xef\xbb\xbf") ? raw.substr(3) : raw;
    for (auto& line : split(text,'\n')) {
        if (line.empty() || line.starts_with("//")) continue;
        if (line.front() == '[' && line.back() == ']') {section = line.substr(1,line.size()-2); continue;}
        try {
            if (section == "TimingPoints") {
                auto p = split(line,','); if (p.size() < 2) continue;
                out.timing.push_back({number(p[0]), number(p[1]), p.size() > 2 ? std::max(1,integer(p[2])) : 4,
                                      p.size() <= 6 || p[6] != "0", p.size() > 7 && bool(integer(p[7])&1)});
            } else if (section == "HitObjects") {
                auto p = split(line,','); if (p.size() >= 5) out.objects.push_back(std::move(p));
            } else {
                auto colon = line.find(':'); if (colon == line.npos) continue;
                auto key = trim(line.substr(0,colon)), value = trim(line.substr(colon+1));
                if (section == "General" && key == "Mode") out.mode = integer(value);
                else if (section == "Difficulty" && key == "SliderMultiplier") out.slider = std::max(.01,number(value));
                if (section == "Metadata" || section == "General") out.metadata[key] = value;
            }
        } catch (const std::exception&) { /* The source parser ignores malformed optional fields. */ }
    }
    std::stable_sort(out.timing.begin(),out.timing.end(),[](auto& a, auto& b){return a.offset < b.offset;});
    return out;
}
const Timing& active(const std::vector<Timing>& reds, double at) {
    auto it = std::upper_bound(reds.begin(),reds.end(),at+.001,[](double v,const Timing& t){return v < t.offset;});
    return it == reds.begin() ? reds.front() : *--it;
}
std::pair<double,bool> effects(const std::vector<Timing>& timing, double at) {
    double scroll = 1; bool kiai = false;
    for (auto& t : timing) {
        if (t.offset > at+.001) break;
        kiai = t.kiai;
        if (t.red) scroll = 1;
        else if (t.beat < 0) scroll = std::max(.01,-100/t.beat);
    }
    return {scroll,kiai};
}
int type(int sound) {return sound&(2|8) ? (sound&4 ? 8 : 4) : (sound&4 ? 7 : 1);}
std::vector<Hit> hits(const Osu& osu, const std::vector<Timing>& reds) {
    std::vector<Hit> out;
    for (auto& p : osu.objects) {
        try {
            double start = number(p[2]); int kind = integer(p[3]), sound = integer(p[4]);
            if (kind&1) out.push_back({start,0,type(sound)});
            else if ((kind&2) && p.size() >= 8) {
                int spans = std::max(1,integer(p[6])); double pixels = number(p[7]);
                auto& red = active(reds,start); auto [scroll,kiai] = effects(osu.timing,start);
                double span = pixels*red.beat/(100*osu.slider*std::max(.01,scroll));
                double duration = span*spans;
                if (!std::isfinite(duration) || duration < 0 || spans > 1000000) throw std::runtime_error("invalid slider");
                if (duration < 2*red.beat) {
                    std::vector<int> edges;
                    if (p.size() > 8 && !p[8].empty()) {
                        try {for (auto& e : split(p[8],'|')) edges.push_back(integer(e));} catch (...) {edges.clear();}
                    }
                    for (int i = 0; i <= spans; ++i) out.push_back({start+span*i,0,type(size_t(i) < edges.size() ? edges[i] : sound)});
                } else out.push_back({start,duration,sound&4 ? 9 : 6});
            } else if ((kind&8) && p.size() >= 6) {
                double duration = std::max(0.,number(p[5])-start);
                out.push_back({start,duration,10,int(std::clamp(std::trunc(duration/122),1.,65535.))});
            }
        } catch (const std::exception&) {continue;}
        if (out.size() > 1000000) throw std::runtime_error("osu chart has too many hit objects");
    }
    std::stable_sort(out.begin(),out.end(),[](auto& a,auto& b){return a.start == b.start ? a.duration < b.duration : a.start < b.start;});
    return out;
}
std::set<double> sample(const std::set<double>& values, int limit) {
    if (limit <= 0) return {};
    if (values.size() <= size_t(limit)) return values;
    std::vector<double> ordered(values.begin(),values.end());
    if (limit == 1) return {ordered[ordered.size()/2]};
    std::set<double> out;
    for (int i = 0; i < limit; ++i) out.insert(ordered[size_t(std::nearbyint(double(i)*(ordered.size()-1)/(limit-1)))]);
    return out;
}
int64_t time_key(double v) {
    if (std::abs(v) > 1e12) throw std::runtime_error("osu timing exceeds supported range");
    return int64_t(std::nearbyint(v*1000));
}
std::pair<std::vector<double>,std::set<int64_t>> boundaries(const Osu& osu, const std::vector<Timing>& reds, const std::vector<Hit>& events) {
    double start = std::min(reds[0].offset,events[0].start), last = start;
    for (auto& e : events) last = std::max(last,e.start+e.duration);
    auto& final = active(reds,last); double end = last+final.beat*final.meter;
    std::set<double> required{start,end}, natural{start}; std::set<int64_t> bars{time_key(start)};
    for (size_t i = 0; i < reds.size(); ++i) {
        const auto& r = reds[i]; if (r.offset > end) break;
        double segment_end = std::min(end, i+1 < reds.size() ? reds[i+1].offset : end);
        double length = r.beat*r.meter;
        if (length < .001 || !std::isfinite(length)) throw std::runtime_error("invalid osu measure length");
        for (double cursor = std::max(start,r.offset); cursor < segment_end-.001; cursor += length) {
            natural.insert(cursor); bars.insert(time_key(cursor));
            if (natural.size() > 1000000) throw std::runtime_error("osu chart exceeds measure scan limit");
        }
        required.insert(segment_end);
        if (i+1 < reds.size() && reds[i+1].offset <= end) bars.insert(time_key(reds[i+1].offset));
    }
    bool kiai = effects(osu.timing,start).second;
    for (auto& t : osu.timing) {
        if (t.offset <= start+.001) continue; if (t.offset >= end-.001) break;
        if (t.kiai != kiai) required.insert(t.offset);
        kiai = t.kiai;
    }
    if (required.size() > 301) throw std::runtime_error("osu chart needs more than 300 BPM/kiai sections");
    for (double t : required) natural.erase(t);
    auto structural = required; auto sampled = sample(natural,301-int(required.size()));
    structural.insert(sampled.begin(),sampled.end());
    std::set<double> scrolls; double scroll = effects(osu.timing,start).first;
    for (auto& t : osu.timing) {
        if (t.offset <= start+.001) continue; if (t.offset >= end-.001) break;
        double next = t.red ? 1 : t.beat < 0 ? std::max(.01,-100/t.beat) : scroll;
        if (std::abs(next-scroll) > std::max(1e-9,1e-9*std::max(std::abs(next),std::abs(scroll)))) {
            if (!structural.count(t.offset)) scrolls.insert(t.offset);
            scroll = next;
        }
    }
    sampled = sample(scrolls,301-int(structural.size())); structural.insert(sampled.begin(),sampled.end());
    if (structural.size() < 2) throw std::runtime_error("cannot construct osu measures");
    return {{structural.begin(),structural.end()},bars};
}
}
TaikoCatalogSong inspect_osu(const std::filesystem::path& source, const std::filesystem::path& audio,
                            const std::string& chart_hash, const std::string& set_id, double rating) {
    auto raw = read(source); auto osu = parse(raw);
    if (osu.mode != 1 || osu.objects.empty()) throw std::runtime_error("not a playable native osu!taiko chart");
    int level = std::isfinite(rating) && rating >= 0 ? int(std::clamp(std::floor(rating*1.5+.5),1.,10.)) : 1;
    TaikoCatalogSong song;
    song.music_id = "tc"+chart_hash.substr(0,12); song.title = osu.metadata["Title"];
    song.original_title = osu.metadata["TitleUnicode"];
    if (song.title.empty()) song.title = song.original_title;
    if (song.title.empty()) song.title = utf8(source.filename());
    if (song.original_title.empty()) song.original_title = song.title;
    song.custom_subtitle = osu.metadata["ArtistUnicode"];
    if (song.custom_subtitle.empty()) song.custom_subtitle = osu.metadata["Artist"];
    song.osu_group = set_id+":"+utf8(audio.filename()); song.osu_difficulty = osu.metadata["Version"];
    if (song.osu_difficulty.empty()) song.osu_difficulty = "Taiko";
    song.tja_path = utf8(source); song.audio_path = utf8(audio); song.stars[3] = level; song.difficulty_mask = 8;
    song.custom_revision = revision(raw,level);
    auto preview = osu.metadata["PreviewTime"];
    if (!preview.empty()) song.preview_ms = uint32_t(std::clamp(number(preview),0.,double(UINT32_MAX)));
    return song;
}
Fumen osu_fumen(const std::string& raw, int level) {
    auto osu = parse(raw); if (osu.mode != 1) throw std::runtime_error("chart is not native osu!taiko");
    std::vector<Timing> reds;
    for (auto& t : osu.timing) if (t.red && t.beat > 0) reds.push_back(t);
    if (reds.empty()) throw std::runtime_error("osu chart has no uninherited timing point");
    auto events = hits(osu,reds);
    if (events.empty()) throw std::runtime_error("osu chart has no supported hit objects");
    auto [bounds,bars] = boundaries(osu,reds,events);
    Fumen f; f.level = level;
    for (size_t i = 0; i+1 < bounds.size(); ++i) {
        auto& red = active(reds,bounds[i]); auto [scroll,kiai] = effects(osu.timing,bounds[i]);
        Measure m; m.bpm = 60000/red.beat; m.offset = bounds[i]-240000/m.bpm; m.end = bounds[i+1]-240000/m.bpm;
        m.duration = bounds[i+1]-bounds[i]; m.gogo = kiai; m.barline = bars.count(time_key(bounds[i])); m.branches[0].speed = scroll;
        f.measures.push_back(m);
    }
    int count = 0;
    for (auto& e : events) {
        size_t i = std::clamp<std::ptrdiff_t>(std::upper_bound(bounds.begin(),bounds.end()-1,e.start)-bounds.begin()-1,0,f.measures.size()-1);
        Note n; n.type = e.type; n.pos = std::max(0.,e.start-bounds[i]); n.duration = std::max(0.,e.duration); n.hits = e.hits;
        f.measures[i].branches[0].notes.push_back(n); count += combo(n.type);
    }
    int score = count ? int(std::min(65535.,std::ceil(1000000./count/10)*10)) : 0;
    for (auto& m : f.measures) for (auto& n : m.branches[0].notes) if (combo(n.type)) n.score = score;
    finish(f,count); return f;
}
}
