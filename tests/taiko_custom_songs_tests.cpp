#include "taiko_custom_songs.h"
#include "taiko_audio_decoder.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cassert>
#undef NDEBUG
#undef assert
#define assert(x) do {if(!(x)) {std::fprintf(stderr,"assertion at %d: %s\n",__LINE__,#x);std::abort();}} while(0)
namespace fs=std::filesystem;
void env(const char* key,const std::string& value) {
#ifdef _WIN32
    _putenv_s(key,value.c_str());
#else
    setenv(key,value.c_str(),1);
#endif
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--catalog") {
        assert(taiko_catalog_load());
        std::printf("native catalog: %zu songs\n", taiko_catalog_count());
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--lazer") {
        assert(taiko_catalog_load());
        const TaikoCatalogSong* selected = nullptr;
        size_t selected_index = 0;
        for (size_t i = 0; i < taiko_catalog_count(); ++i) {
            const auto* song = taiko_catalog_song(i);
            if (song->music_id == argv[2]) { selected = song; selected_index = i; break; }
        }
        assert(selected && selected->genre == "OSU! LAZER");
        TaikoDecodedAudio preview; std::string error;
        assert(taiko_audio_decode_song(selected->music_id, 48000, nullptr, preview, error));
        assert(preview.pcm && !preview.pcm->empty());
        taiko_plus::ContentIdentity identity;
        if (!taiko_catalog_content_identity(selected_index, 3, identity, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str()); return 1;
        }
        const auto guest = "/data/fumen/" + selected->music_id + "/solo/" + selected->music_id + "_m.bin";
        FILE* chart = taiko_custom_open(guest.c_str(), 0); assert(chart); std::fclose(chart);
        std::printf("lazer integration passed: %s (%zu preview frames)\n", selected->title.c_str(), preview.pcm->size()/2);
        return 0;
    }
    env("TAIKO_OSU_LAZER", "0");
    env("TAIKO_PYTHON", "/nonexistent/python");
    env("TAIKO_DOTNET", "/nonexistent/dotnet");
    const auto root=fs::temp_directory_path()/("taiko-custom-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root/"TJA/Anime/Song");
    env("TAIKO_CUSTOM_SONGS",root.string());env("PS3_VFS_ROOT",root.string());
    std::vector<uint8_t> wav(44+48000*4,0);
    auto put=[&](size_t off,uint32_t v){for(unsigned i=0;i<4;++i)wav[off+i]=uint8_t(v>>(i*8));};
    std::memcpy(wav.data(),"RIFF",4);put(4,wav.size()-8);
    std::memcpy(wav.data()+8,"WAVEfmt ",8);put(16,16);wav[20]=1;wav[22]=2;
    put(24,48000);put(28,192000);wav[32]=4;wav[34]=16;
    std::memcpy(wav.data()+36,"data",4);put(40,wav.size()-44);
    // Constant nonzero samples make the prepended silence and exact boundary observable.
    for(size_t i=44;i<wav.size();i+=2)wav[i+1]=16;
    {std::ofstream out(root/"TJA/Anime/Song/audio.wav",std::ios::binary);out.write((char*)wav.data(),wav.size());}
    {std::ofstream out(root/"TJA/Anime/Song/song.tja");out<<"TITLE:Integration test\nBPM:120\nOFFSET:0\nWAVE:audio.wav\nCOURSE:Oni\nLEVEL:7\n#START\n1000,\n#END\n";}
    assert(taiko_catalog_load());assert(taiko_catalog_count()==1);
    const auto* song=taiko_catalog_song(0);assert(song->genre=="CUSTOM TJA");
    assert(song->custom_folder=="Anime");
    TaikoDecodedAudio preview;std::string error;
    assert(taiko_audio_decode_song(song->music_id,48000,nullptr,preview,error));
    assert(preview.pcm->size()==96000);
    taiko_plus::ContentIdentity identity;
    assert(taiko_catalog_content_identity(0,3,identity,&error));
    const auto guest="/data/fumen/"+song->music_id+"/solo/"+song->music_id+"_m.bin";
    uint64_t size=0;assert(taiko_custom_stat(guest.c_str(),&size));assert(size>0);
    FILE* chart=taiko_custom_open(guest.c_str(),0);assert(chart);std::fclose(chart);
    assert(!taiko_custom_open(guest.c_str(),1));
    assert(!taiko_custom_open((guest+"/../e.bin").c_str(),0));
    std::string upper=song->music_id;for(char& c:upper)if(c>='a'&&c<='z')c-=32;
    const auto audio="/data/sound/bgm/nub/SONG_"+upper+".nub";
    assert(taiko_custom_stat(audio.c_str(),&size));
    FILE* bank=taiko_custom_open(audio.c_str(),0);assert(bank);
    std::fseek(bank,2048,SEEK_SET);std::vector<uint8_t> prefix(8192);
    assert(std::fread(prefix.data(),1,prefix.size(),bank)==prefix.size());std::fclose(bank);
    std::vector<uint8_t> riff;std::string source;
    assert(taiko_audio_resolve_riff(0,prefix,riff,source,error));assert(source.find("SONG_")==0);
    TaikoDecodedAudio gameplay;assert(taiko_audio_decode_riff(riff,0,nullptr,gameplay,error));
    assert(gameplay.sample_rate==48000);assert(gameplay.pcm->size()==3*96000);
    assert((*gameplay.pcm)[2*96000-1]==0);assert((*gameplay.pcm)[2*96000]>0.1f);
    assert(!gameplay.has_loop);
    // Disk cache must be reused on a second launch.
    const auto stamp=fs::last_write_time(fs::path(song->custom_cache)/"m.bin");
    assert(taiko_catalog_content_identity(0,3,identity,&error));
    assert(fs::last_write_time(fs::path(song->custom_cache)/"m.bin")==stamp);
    fs::remove_all(root);
}
