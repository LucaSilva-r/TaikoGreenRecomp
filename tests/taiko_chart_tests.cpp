#include "taiko_chart_internal.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char* message) {if (!ok) throw std::runtime_error(message);}
int main(int argc, char** argv) {
    try {
        // Developer comparison entry point; no interpreter is used by this binary.
        if (argc >= 4 && std::string(argv[1]) == "convert") {
            TaikoCatalogSong song; song.tja_path = argv[2]; song.custom_cache = argv[3];
            int level = argc > 4 ? std::stoi(argv[4]) : 0;
            if (level) {song.genre = "OSU! LAZER"; song.stars[3] = level;}
            song.custom_revision = taiko_chart::revision(taiko_chart::read(taiko_chart::path(song.tja_path)),level);
            taiko_chart::convert(song); return 0;
        }
        const std::string tja = "TITLE:Native\nBPM:120\nOFFSET:0.25\nCOURSE:Oni\nLEVEL:7\nSCOREINIT:1000\nSCOREDIFF:100\nBALLOON:5\n#START\n1000,\n7008,\n#END\n";
        auto fs = taiko_chart::tja_fumens(tja);
        check(fs.size() == 1 && fs[0].measures.size() == 2,"TJA measure count");
        check(fs[0].measures[0].offset == -2250,"TJA offset");
        auto n = fs[0].measures[1].branches[0].notes[0];
        check(n.type == 10 && n.hits == 5 && n.duration == 1500,"TJA balloon duration/hits");
        auto branched = taiko_chart::tja_fumens("BPM:120\nCOURSE:Oni\nLEVEL:5\n#START\n#BRANCHSTART p,50,80\n#N\n1,\n1,\n#E\n1,\n#M\n1,\n#BRANCHEND\n#END\n");
        check(branched[0].measures.size() == 2,"missing branch filled");
        for (const auto& m : branched[0].measures) for (const auto& b : m.branches)
            check(b.notes.size() == 1,"missing branch must not duplicate shared notes");
        auto split = taiko_chart::tja_fumens("BPM:120\nCOURSE:Oni\nLEVEL:5\n#START\n#BRANCHSTART p,50,80\n#N\n10\n#SCROLL 2\n10,\n#E\n1,\n#M\n1,\n#BRANCHEND\n#END\n");
        check(split[0].measures.size() == 2,"mid-measure branch split aligned");
        check(split[0].measures[0].duration == 1000 && split[0].measures[1].duration == 1000,"aligned split timings");
        const std::string osu = "osu file format v14\n[General]\nMode:1\n[Difficulty]\nSliderMultiplier:1.4\n[TimingPoints]\n0,500,4,2,0,100,1,0\n[HitObjects]\n256,192,1000,1,0\n256,192,2000,8,0,3000\n";
        auto f = taiko_chart::osu_fumen(osu,7);
        check(f.measures[0].offset == -2000,"osu fumen lead measure");
        check(f.measures[0].branches[0].notes[0].pos == 1000,"osu hit timestamp");
        check(f.measures[0].branches[0].notes[0].score == 65535,"osu score fallback");
        bool rejected = false;
        try {taiko_chart::tja_fumens("BPM:0\nCOURSE:Oni\n#START\n1,\n#END\n");} catch (...) {rejected = true;}
        check(rejected,"invalid BPM rejection");
        std::string long_tja = "BPM:120\nCOURSE:Oni\nLEVEL:1\n#START\n";
        for (int i = 0; i < 16385; ++i) long_tja += "1,\n";
        rejected = false;
        try {taiko_chart::tja_fumens(long_tja+"#END\n");} catch (...) {rejected = true;}
        check(rejected,"16384 measure limit");
        auto root = std::filesystem::temp_directory_path()/("taiko-chart-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        auto source = root/"shift-jis.tja";
        std::filesystem::create_directories(root/"audio");
        {std::ofstream audio(root/"audio/song~.wav"); audio << "unchanged audio";}
        {std::ofstream chart(source,std::ios::binary); chart << "TITLE:\x83\x65\x83\x58\x83\x67\nWAVE:audio\\song~.wav\n" << tja;}
        auto song = taiko_chart::inspect_tja(source,root);
        check(song.title == "テスト","Shift-JIS title decoding");
        song.custom_cache = taiko_chart::utf8(root/"cache");
        {std::ofstream chart(source,std::ios::app); chart << "\n// changed\n";}
        rejected = false;
        try {taiko_chart::convert(song);} catch (...) {rejected = true;}
        check(rejected && !std::filesystem::exists(root/"cache/ready"),"changed-source rejection before publication");
        auto osu_source = root/"song.osu";
        for (const auto& metadata : {std::string("Title:Kawaki wo Ameku\nTitleUnicode:カワキヲアメク\n"),
                                     std::string("Title:Kawaki wo Ameku\n"),
                                     std::string("TitleUnicode:カワキヲアメク\n")}) {
            {std::ofstream chart(osu_source); chart << osu << "[Metadata]\n" << metadata;}
            auto imported = taiko_chart::inspect_osu(osu_source,root/"audio.ogg","hash","set",3);
            check(imported.title == (metadata.find("Title:") != std::string::npos ? "Kawaki wo Ameku" : "カワキヲアメク"),
                  "osu display prefers romanized title with Unicode fallback");
            check(imported.original_title == (metadata.find("TitleUnicode:") != std::string::npos ? "カワキヲアメク" : "Kawaki wo Ameku"),
                  "osu original title retained for search");
        }
        std::filesystem::remove_all(root);
        std::puts("native chart tests passed"); return 0;
    } catch (const std::exception& e) {std::fprintf(stderr,"%s\n",e.what()); return 1;}
}
