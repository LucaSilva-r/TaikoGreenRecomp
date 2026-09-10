#include "taiko_chart.h"
#include <realm/db.hpp>
#include <realm/history.hpp>
#include <realm/list.hpp>
#include <realm/transaction.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
static void check(bool value, const char* message) {if (!value) throw std::runtime_error(message);}
static void write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p,std::ios::binary); out.write(bytes.data(),bytes.size()); check(bool(out),"fixture write");
}
int main() {
    auto root = fs::temp_directory_path()/("taiko-lazer-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root);
        const auto file = root/"client.realm";
        std::vector<std::string> hashes;
        auto asset = [&](const std::string& bytes) {
            auto hash = taiko_chart::hash(bytes);
            write(root/"files"/hash.substr(0,1)/hash.substr(0,2)/hash,bytes);
            return hash;
        };
        const auto audio_hash = asset("original audio bytes");
        for (int i = 0; i < 4; ++i) hashes.push_back(asset("osu file format v14\n[General]\nMode:1\nPreviewTime:1234\n[Metadata]\nTitle:Song\nVersion:Difficulty "+std::to_string(i)+"\n[HitObjects]\n256,192,1000,1,0\n"));
        {
            auto db = realm::DB::create(realm::make_in_realm_history(),taiko_chart::utf8(file));
            auto tx = db->start_write();
            auto files = tx->add_table("class_RealmFile"); files->add_column(realm::type_String,"Hash");
            auto usages = tx->add_table("class_RealmNamedFileUsage",realm::Table::Type::Embedded);
            usages->add_column(realm::type_String,"Filename"); usages->add_column(*files,"File");
            auto rules = tx->add_table("class_RulesetInfo"); rules->add_column(realm::type_Int,"OnlineID");
            auto metadata = tx->add_table("class_BeatmapMetadata",realm::Table::Type::Embedded);
            metadata->add_column(realm::type_String,"AudioFile");
            auto maps = tx->add_table("class_BeatmapInfo");
            maps->add_column(realm::type_Bool,"Hidden"); maps->add_column(realm::type_String,"Hash");
            maps->add_column(realm::type_Double,"StarRating"); maps->add_column(*rules,"Ruleset"); maps->add_column(*metadata,"Metadata");
            auto sets = tx->add_table("class_BeatmapSet"); sets->add_column(realm::type_Bool,"DeletePending");
            sets->add_column(realm::type_UUID,"ID"); sets->add_column_list(*maps,"Beatmaps"); sets->add_column_list(*usages,"Files");
            auto set = sets->create_object().set("ID",realm::UUID("01234567-89ab-cdef-0123-456789abcdef"));
            auto audio = files->create_object().set("Hash",audio_hash);
            auto usage = set.get_linklist("Files").create_and_insert_linked_object(0);
            usage.set("Filename","Audio.ogg").set("File",audio.get_key());
            auto taiko = rules->create_object().set("OnlineID",int64_t(1));
            auto standard = rules->create_object().set("OnlineID",int64_t(0));
            for (int i = 0; i < 4; ++i) {
                auto map = maps->create_object().set("Hash",hashes[i]).set("Hidden",i == 2).set("StarRating",2.+i);
                map.set("Ruleset",(i == 3 ? standard : taiko).get_key());
                map.create_and_set_linked_object(maps->get_column_key("Metadata")).set("AudioFile","audio.ogg");
                set.get_linklist("Beatmaps").add(map.get_key());
            }
            tx->commit();
        }
#ifdef _WIN32
        _putenv_s("TAIKO_OSU_LAZER",taiko_chart::utf8(root).c_str());
#else
        setenv("TAIKO_OSU_LAZER",taiko_chart::utf8(root).c_str(),1);
#endif
        auto before = taiko_chart::read(file);
        std::vector<TaikoCatalogSong> songs; taiko_chart::scan_lazer(songs);
        check(songs.size() == 2,"hidden and non-taiko maps excluded");
        check(songs[0].osu_group == songs[1].osu_group,"shared audio/set grouped");
        check(songs[0].music_id != songs[1].music_id,"difficulty identities retained");
        check(songs[0].stars[3] == 3 && songs[0].preview_ms == 1234,"rating and preview metadata");
        check(taiko_chart::read(file) == before,"reader modified database");
        // A future database with a usable old backup must be rejected intact.
        int version = uint8_t(before[20+(uint8_t(before[23])&1)]);
        auto backup = root/("client.v"+std::to_string(version)+".backup.realm"); write(backup,before);
        before[20] = before[21] = char(99); write(file,before);
        bool rejected = false;
        try {songs.clear(); taiko_chart::scan_lazer(songs);} catch (const std::exception&) {rejected = true;}
        check(rejected,"unsupported Realm version must fail");
        check(taiko_chart::read(file) == before && fs::is_regular_file(backup),"reader restored or removed a backup");
        fs::remove_all(root); std::puts("native lazer reader tests passed"); return 0;
    } catch (const std::exception& e) {fs::remove_all(root); std::fprintf(stderr,"%s\n",e.what()); return 1;}
}
