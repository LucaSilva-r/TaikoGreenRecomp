#include "taiko_chart.h"
#include "taiko_custom_songs.h"
#include "taiko_audio_decoder.h"
#include <mbedtls/aes.h>
#include <zlib.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace fs=std::filesystem;
static void check(bool ok,const std::string& message) {if(!ok)throw std::runtime_error(message);}
static void env(const char* name,const std::string& value) {
#ifdef _WIN32
    _putenv_s(name,value.c_str());
#else
    setenv(name,value.c_str(),1);
#endif
}
static void put(std::string& b,size_t p,uint32_t v,bool big=false) {for(int i=0;i<4;++i)b.at(p+i)=char(v>>(big?24-i*8:i*8));}
static void save(const fs::path& p,const std::string& b) {fs::create_directories(p.parent_path());std::ofstream out(p,std::ios::binary);check(bool(out.write(b.data(),b.size())),"fixture write");}
static std::string fumen(unsigned type=1) {
    std::string b(608,'\0');put(b,508,12345678);put(b,512,1);
    put(b,520,std::bit_cast<uint32_t>(120.f));put(b,524,std::bit_cast<uint32_t>(-2000.f));b[529]=1;
    b[560]=1;put(b,564,std::bit_cast<uint32_t>(1.f));put(b,568,type);
    put(b,584,0x019003e8); // distinct packed score fields, must swap separately
    return b;
}
static std::string encrypted(const std::string& plain) {
    z_stream z{};check(deflateInit2(&z,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK,"gzip init");
    std::string packed(compressBound(plain.size())+64,'\0');
    z.next_in=(Bytef*)plain.data();z.avail_in=plain.size();z.next_out=(Bytef*)packed.data();z.avail_out=packed.size();
    check(deflate(&z,Z_FINISH)==Z_STREAM_END,"gzip fixture");packed.resize(z.total_out);deflateEnd(&z);
    const auto pad=16-packed.size()%16;packed.append(pad,char(pad));
    std::string out(16+packed.size(),'\0');unsigned char key[32],iv[16]{};std::memset(key,0x42,32);
    mbedtls_aes_context aes;mbedtls_aes_init(&aes);check(!mbedtls_aes_setkey_enc(&aes,key,256),"AES fixture key");
    check(!mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_ENCRYPT,packed.size(),iv,(const unsigned char*)packed.data(),(unsigned char*)out.data()+16),"AES fixture");
    mbedtls_aes_free(&aes);return out;
}
static std::string bank(const std::string& payload) {
    // Minimal single-stream TONE, with no optional flags, and a PACK chunk.
    std::string b(0x64,'\0');b.replace(0,4,"NUS3");b.replace(8,8,"BANKTOC ");put(b,16,20);put(b,20,2);
    b.replace(24,4,"TONE");put(b,28,44);b.replace(32,4,"PACK");put(b,36,payload.size());
    b.replace(40,4,"TONE");put(b,44,44);put(b,48,1);put(b,52,12);put(b,56,32);
    b[68]=2;b[69]='s';put(b,76,8);put(b,84,payload.size());
    b.replace(92,4,"PACK");put(b,96,payload.size());b+=payload;put(b,4,b.size()-8);return b;
}
static std::string idsp() {
    std::string b(0x120,'\0');b.replace(0,4,"IDSP");
    for(auto [off,v]: {std::pair{8,2},{12,48000},{16,28},{28,16},{32,64},{36,96},{40,256},{44,16}})put(b,off,v,true);
    for(size_t p:{0x40,0xa0}) {
        put(b,p,28,true);put(b,p+4,32,true);put(b,p+8,48000,true);put(b,p+16,2,true);put(b,p+20,31,true);put(b,p+24,2,true);
    }
    // Zero coefficients + predictor zero gives exact signed nibble samples.
    for(size_t p:{0x100,0x108})for(size_t n=1;n<8;++n)b[p+n]=0x11;
    for(size_t p:{0x110,0x118})for(size_t n=1;n<8;++n)b[p+n]=char(0xff);
    return bank(b);
}
static std::string bnsf() {
    std::string b(48+640,'\0');b.replace(0,4,"BNSF");put(b,4,b.size(),true);b.replace(8,8,"IS22sfmt");put(b,16,20,true);
    put(b,20,2,true);put(b,24,48000,true);put(b,28,960,true);put(b,36,0x028003c0,true);b.replace(40,4,"sdat");put(b,44,640,true);
    return bank(b);
}
static void inspect_audio(const fs::path& file) {
    TaikoDecodedAudio audio;std::string error;
    check(taiko_audio_decode_file(taiko_chart::utf8(file),48000,nullptr,audio,error),error);
    check(audio.pcm && !audio.pcm->empty() && audio.sample_rate==48000,"decoded audio shape");
    check(std::all_of(audio.pcm->begin(),audio.pcm->end(),[](float f){return std::isfinite(f);}),"finite PCM");
    std::printf("audio: %s, %zu frames\n",taiko_chart::utf8(file.filename()).c_str(),audio.pcm->size()/2);
}
int main(int argc,char** argv) {
    try {
        auto temp=fs::temp_directory_path()/("taiko-nijiiro-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp);env("TAIKO_CUSTOM_SONGS",taiko_chart::utf8(temp/"custom"));env("TAIKO_OSU_LAZER","0");env("PS3_VFS_ROOT",taiko_chart::utf8(temp/"vfs"));
        if(argc>=3 && std::string(argv[1])=="--library") {
            env("TAIKO_NIJIIRO",argv[2]);check(taiko_catalog_load(),"load real library");
            size_t checked=0;
            for(size_t i=0;i<taiko_catalog_count();++i) {
                const auto& s=*taiko_catalog_song(i);if(s.genre!="NIJIIRO")continue;
                check(!s.title.empty(),"real library has no empty titles");
                if(argc>3 && std::string(argv[3])=="--all-charts") {taiko_chart::convert(s);++checked;continue;}
                const auto id=taiko_chart::utf8(taiko_chart::path(s.tja_path).filename());
                if(id!="07ss4r" && id!="10binz")continue;
                inspect_audio(taiko_chart::path(s.audio_path));
                TaikoDecodedAudio preview;std::string preview_error;check(taiko_audio_decode_song(s.music_id,48000,nullptr,preview,preview_error),preview_error);
                std::string error;check(taiko_custom_prepare(s,error),error);
                const auto base="/data/fumen/"+s.music_id+"/duet/"+s.music_id;
                for(const char* tail:{"_m_1.bin","_m_2.bin"}) {auto f=taiko_custom_open((base+tail).c_str(),0);check(f,"real duet overlay");std::fclose(f);}
                ++checked;
            }
            check(checked>=2,"real library integration selection");
            std::printf("Nijiiro library: %zu songs, %zu prepared\n",taiko_catalog_count(),checked);
            fs::remove_all(temp);return 0;
        }
        const auto root=temp/"install", data=root/"Data/x64";
        // The synthetic loader contains an arbitrary test key, not a game key.
        std::string key;for(int i=0;i<32;++i)key+="42";save(root/"Executable/Release/bnusio.dll","fixture:"+key+":end");
        save(data/"datatable/musicinfo.bin",encrypted(R"({"items":[{"id":"test","genreNo":5,"starMania":7},{"id":"bad","starMania":8},{"id":"zzblank","genreNo":7,"starMania":4}]})"));
        save(data/"datatable/wordlist.bin",encrypted(R"({"items":[{"key":"song_test","japaneseText":"テスト","englishUsText":"Test"},{"key":"song_test","japaneseText":"","englishUsText":" "},{"key":"song_test","japaneseText":"Wrong subtitle","englishUsText":"Wrong subtitle"},{"key":"song_zzblank","japaneseText":"","englishUsText":""},{"key":"song_sub_test","japaneseText":"副題","englishUsText":"Subtitle"}]})"));
        save(data/"fumen/test/test_m.bin",encrypted(fumen()));save(data/"fumen/test/test_m_1.bin",encrypted(fumen(7)));save(data/"fumen/test/test_m_2.bin",encrypted(fumen(8)));
        save(data/"fumen/bad/bad_m.bin",encrypted(fumen(20)));
        save(data/"fumen/zzblank/zzblank_m.bin",encrypted(fumen()));save(data/"sound/song_zzblank.nus3bank",idsp());
        save(data/"sound/song_test.nus3bank",idsp());save(data/"sound/song_bad.nus3bank",bnsf());
        inspect_audio(data/"sound/song_test.nus3bank");inspect_audio(data/"sound/song_bad.nus3bank");
        TaikoDecodedAudio audio;std::string error;check(taiko_audio_decode_file(taiko_chart::utf8(data/"sound/song_test.nus3bank"),48000,nullptr,audio,error),error);
        check(audio.pcm->size()==56,"IDSP sample count");check(std::abs((*audio.pcm)[0]-1.f/32768)<1e-9 && std::abs((*audio.pcm)[1]+1.f/32768)<1e-9,"IDSP channel samples");
        std::atomic<bool> stop{true};check(!taiko_audio_decode_file(taiko_chart::utf8(data/"sound/song_test.nus3bank"),44100,&stop,audio,error),"cancel decode");
        env("TAIKO_NIJIIRO",taiko_chart::utf8(data/"fumen"));check(taiko_catalog_load(),"fixture discovery");check(taiko_catalog_count()==2,"exclude unknown course");
        const auto& song=*taiko_catalog_song(0);check(song.title=="Test" && song.original_title=="テスト" && song.stars[3]==7 && song.custom_folder=="NAMCO ORIGINAL","metadata");
        check(taiko_audio_decode_song(song.music_id,48000,nullptr,audio,error),error);
        check(audio.pcm->size()==56,"browser preview routes to Nijiiro audio");
        const auto& blank=*taiko_catalog_song(1);check(blank.title=="zzblank" && blank.custom_folder=="CLASSICAL","blank title fallback and Classical genre");
        check(taiko_custom_prepare(song,error),error);
        auto solo=taiko_chart::read(taiko_chart::path(song.custom_cache)/"m.bin");
        check(uint8_t(solo[571])==1,"solo note preserved");
        check(solo.substr(524,4)==std::string(4,'\0'),"two second lead-in applied");
        check(solo.substr(584,4)==std::string("\x03\xe8\x01\x90",4),"packed 16-bit scores swapped separately");
        for(int player=1;player<=2;++player) {
            const auto guest="/data/fumen/"+song.music_id+"/duet/"+song.music_id+"_m_"+std::to_string(player)+".bin";
            auto file=taiko_custom_open(guest.c_str(),0);check(file,"duet overlay");std::fseek(file,571,SEEK_SET);check(std::fgetc(file)==player+6,"distinct authored duet note");std::fclose(file);
        }
        auto ready=taiko_chart::path(song.custom_cache)/"ready";auto before=fs::last_write_time(ready);
        check(taiko_custom_prepare(song,error),error);check(fs::last_write_time(ready)==before,"cache reused");
        save(taiko_chart::path(song.custom_cache)/"m_2.bin","damaged");check(taiko_custom_prepare(song,error),"damaged duet cache rebuilt");
        save(data/"fumen/test/test_m_2.bin",encrypted(fumen(3)));bool rejected=false;
        try {taiko_chart::convert(song);}catch(...){rejected=true;}check(rejected,"changed duet source rejected");
        for(auto broken:{std::string(519,'\0'),fumen(25),fumen().substr(0,600)}) {double e=INFINITY;rejected=false;try{taiko_chart::nijiiro_fumen(broken,0,e);}catch(...){rejected=true;}check(rejected,"invalid chart rejected");}
        auto many=fumen();put(many,512,16385);double earliest=INFINITY;rejected=false;try{taiko_chart::nijiiro_fumen(many,0,earliest);}catch(...){rejected=true;}check(rejected,"measure limit");
        // Plain big-endian charts retain all bytes without a lead-in.
        earliest=INFINITY;check(taiko_chart::nijiiro_fumen(solo,0,earliest)==solo,"big endian preservation");
        auto multiple=solo.substr(0,520);put(multiple,512,256,true);
        for(unsigned i=0;i<256;++i)multiple+=solo.substr(520);
        earliest=INFINITY;check(taiko_chart::nijiiro_fumen(multiple,0,earliest)==multiple,"big endian 256 measures");
        put(multiple,508,0,true);earliest=INFINITY;
        check(taiko_chart::nijiiro_fumen(multiple,0,earliest)==multiple,"chart without optional marker");
        fs::remove_all(temp);std::puts("Nijiiro tests passed");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"Nijiiro test: %s\n",e.what());return 1;}
}
