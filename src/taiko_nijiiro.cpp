#include "taiko_chart.h"
#include "taiko_chart_limits.h"
#include <mbedtls/aes.h>
#include <json.hpp>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace taiko_chart {
namespace {
namespace fs = std::filesystem;
using Key = std::array<unsigned char,32>;
using Keys = std::vector<Key>;
using Assets = std::map<std::string,std::string>;
constexpr size_t limit = 16*1024*1024;
uint32_t word(std::string_view b, size_t at, bool big = false) {
    if (at > b.size() || b.size()-at < 4) throw std::runtime_error("truncated Nijiiro data");
    uint32_t v=0;
    for (int i=0;i<4;++i) v |= uint32_t(uint8_t(b[at+i])) << (big ? 24-i*8 : i*8);
    return v;
}
std::string inflate_gzip(const std::string& b) {
    z_stream z{};
    if (inflateInit2(&z,31)!=Z_OK) throw std::runtime_error("gzip initialization failed");
    struct End {z_stream* z; ~End(){inflateEnd(z);}} end{&z};
    z.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(b.data())); z.avail_in=b.size();
    std::string out;
    int result=Z_OK;
    do {
        std::array<char,32768> block;
        z.next_out=reinterpret_cast<Bytef*>(block.data()); z.avail_out=block.size();
        result=inflate(&z,Z_NO_FLUSH);
        const size_t got=block.size()-z.avail_out;
        if (out.size()+got>limit) throw std::runtime_error("decompressed Nijiiro file exceeds 16 MiB");
        out.append(block.data(),got);
        if (result!=Z_OK && result!=Z_STREAM_END) throw std::runtime_error("invalid Nijiiro gzip payload");
    } while (result!=Z_STREAM_END);
    if (z.avail_in) throw std::runtime_error("trailing bytes after Nijiiro gzip payload");
    return out;
}
bool hexkey(std::string_view s, Key& key) {
    if(s.size()!=64) return false;
    auto digit=[](unsigned char c)->int {if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(size_t i=0;i<32;++i) {int a=digit(s[2*i]),b=digit(s[2*i+1]);if(a<0||b<0)return false;key[i]=a*16+b;}
    return true;
}
Keys keys(const fs::path& root) {
    Keys out;
    for(const char* name:{"TAIKO_NIJIIRO_FUMEN_KEY","TAIKO_NIJIIRO_DATATABLE_KEY"}) {
        const auto value=std::getenv(name);
        if(value && *value) {Key k;if(!hexkey(value,k))throw std::runtime_error(std::string(name)+" must have 64 hex digits");out.push_back(k);}
    }
    // Full installations already contain their loader. Inspect its literal key
    // candidates without executing it or relying on a particular binary offset.
    const auto loader=root.parent_path().parent_path()/"Executable/Release/bnusio.dll";
    if(fs::is_regular_file(loader)) {
        const auto b=read(loader);
        for(size_t i=0;i+64<=b.size();++i) {
            Key k;
            if(hexkey(std::string_view(b).substr(i,64),k)) {
                if(out.size()>=128)throw std::runtime_error("too many loader key candidates");
                if(std::find(out.begin(),out.end(),k)==out.end())out.push_back(k);
                i+=63;
            }
        }
    }
    return out;
}
bool plausible(std::string_view b, bool table) {
    if(table) {if(b.starts_with("\xef\xbb\xbf"))b.remove_prefix(3);const auto p=b.find_first_not_of(" \r\n\t");return p!=b.npos && b[p]=='{';}
    if(b.size()<520)return false;
    if(word(b,508)==12345678 || word(b,508,true)==12345678)return true;
    if((word(b,512)>0 && word(b,512)<=TAIKO_MAX_FUMEN_MEASURES) ||
       (word(b,512,true)>0 && word(b,512,true)<=TAIKO_MAX_FUMEN_MEASURES)) {
        try {double earliest=std::numeric_limits<double>::infinity();nijiiro_fumen(std::string(b),0,earliest);return true;}
        catch(const std::exception&) {}
    }
    return false;
}
std::string unpack(const std::string& b,const Keys& candidates,bool table) {
    if(b.starts_with("\x1f\x8b"))return inflate_gzip(b);
    if(plausible(b,table))return b;
    if(b.size()<32 || b.size()%16)throw std::runtime_error("invalid encrypted Nijiiro file size");
    for(const auto& key:candidates) {
        mbedtls_aes_context aes;mbedtls_aes_init(&aes);
        auto iv=std::array<unsigned char,16>{};std::memcpy(iv.data(),b.data(),16);
        std::string plain(b.size()-16,'\0');
        int rc=mbedtls_aes_setkey_dec(&aes,key.data(),256);
        if(!rc)rc=mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_DECRYPT,plain.size(),iv.data(),
            reinterpret_cast<const unsigned char*>(b.data()+16),reinterpret_cast<unsigned char*>(plain.data()));
        mbedtls_aes_free(&aes);
        if(rc)continue;
        const auto pad=uint8_t(plain.back());
        if(!pad || pad>16 || plain.substr(plain.size()-pad)!=std::string(pad,char(pad)))continue;
        plain.resize(plain.size()-pad);
        if(plain.starts_with("\x1f\x8b"))return inflate_gzip(plain);
        if(plausible(plain,table))return plain;
    }
    throw std::runtime_error("cannot decrypt Nijiiro file; provide its AES key or the installation's arcade loader");
}
fs::path root_path() {
    const char* configured=std::getenv("TAIKO_NIJIIRO");
    if(!configured || !*configured || std::strcmp(configured,"0")==0)return {};
    auto root=fs::canonical(path(configured));
    if(root.filename()=="fumen")root=root.parent_path();
    if(fs::is_directory(root/"Data/x64"))root/="Data/x64";
    else if(fs::is_directory(root/"x64"))root/="x64";
    if(!fs::is_directory(root/"fumen") || !fs::is_directory(root/"sound"))
        throw std::runtime_error("Nijiiro folder must contain fumen and sound directories");
    return root;
}
Assets source_assets(const fs::path& dir,unsigned mask) {
    Assets out;const auto id=utf8(dir.filename());
    for(unsigned d=0;d<5;++d)if(mask & (1u<<d)) {
        const std::string suffix(1,"enhmx"[d]);
        for(const char* variant:{"","_1","_2"}) {
            const auto name=suffix+variant;
            auto file=dir/(id+"_"+name+".bin");
            if(*variant && !fs::is_regular_file(file))file=dir/(id+"_"+suffix+".bin");
            out[name]=read(file);
        }
    }
    return out;
}
std::string assets_revision(const Assets& assets) {
    std::string manifest="nijiiro-1\n";
    for(const auto& [name,bytes]:assets)manifest+=name+" "+hash(bytes)+"\n";
    return revision(manifest);
}
nlohmann::json table(const fs::path& root,const char* name,const Keys& k) {
    auto file=root/"datatable"/(std::string(name)+".json");
    if(!fs::is_regular_file(file))file.replace_extension(".bin");
    return nlohmann::json::parse(unpack(read(file),k,true)).at("items");
}
void publish(const fs::path& file,const std::string& bytes) {
    auto temp=file;temp+=".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out.write(bytes.data(),bytes.size())||!out.flush())throw std::runtime_error("cannot write Nijiiro cache");}
    std::error_code ec;fs::remove(file,ec);fs::rename(temp,file);
}
}

std::string nijiiro_fumen(std::string bytes,unsigned lead,double& earliest) {
    if(bytes.size()<520)throw std::runtime_error("truncated fumen header");
    // Some authentic charts omit the usual 12345678 marker. With a limit
    // below 65536, a valid nonzero count cannot be valid in both byte orders.
    const bool big=word(bytes,512)>TAIKO_MAX_FUMEN_MEASURES;
    const auto measures=word(bytes,512,big);
    if(!measures || measures>TAIKO_MAX_FUMEN_MEASURES)throw std::runtime_error("chart exceeds the 16384-measure limit");
    auto u32=[&](size_t p){return word(bytes,p,big);};
    auto real=[&](size_t p){float f=std::bit_cast<float>(u32(p));if(!std::isfinite(f))throw std::runtime_error("nonfinite fumen timing");return double(f);};
    std::vector<size_t> shorts,words,offsets;
    for(size_t p=0;p<520;p+=4)words.push_back(p);
    size_t p=520;
    auto require=[&](size_t count){if(p>bytes.size() || count>bytes.size()-p)throw std::runtime_error("truncated fumen measure or note");};
    for(unsigned m=0;m<measures;++m) {
        require(40);double bpm=real(p),offset=real(p+4);
        if(bpm<=0)throw std::runtime_error("invalid fumen BPM");
        if(uint8_t(bytes[p+8])>1 || uint8_t(bytes[p+9])>1)throw std::runtime_error("invalid fumen measure flags");
        offsets.push_back(p+4);words.push_back(p);words.push_back(p+4);shorts.push_back(p+10);
        for(size_t a=p+12;a<p+40;a+=4)words.push_back(a);
        p+=40;
        for(int branch=0;branch<3;++branch) {
            require(8);auto n=big ? (uint8_t(bytes[p])*256+uint8_t(bytes[p+1])) : (uint8_t(bytes[p+1])*256+uint8_t(bytes[p]));
            shorts.push_back(p);shorts.push_back(p+2);words.push_back(p+4);real(p+4);p+=8;
            for(int i=0;i<n;++i) {
                require(24);const auto type=u32(p);
                if(type<1 || type>13)throw std::runtime_error("note type "+std::to_string(type)+" has not been verified for Green");
                earliest=std::min(earliest,offset+240000/bpm+real(p+4));
                if(type==6 || type==9 || type==10 || type==12)real(p+20);
                for(size_t a=p;a<p+16;a+=4)words.push_back(a);
                shorts.push_back(p+16);shorts.push_back(p+18);words.push_back(p+20);p+=24;
                if(type==6 || type==9) {require(8);words.push_back(p);words.push_back(p+4);p+=8;}
            }
        }
    }
    if(p!=bytes.size())throw std::runtime_error("unexpected trailing fumen data");
    for(auto at:offsets) {
        float shifted=float(real(at)+lead);
        if(!std::isfinite(shifted))throw std::runtime_error("fumen offset overflow");
        const auto v=std::bit_cast<uint32_t>(shifted);
        for(int i=0;i<4;++i)bytes[at+i]=char(v>>(big?24-8*i:8*i));
    }
    if(!big) {
        for(auto at:words)std::reverse(bytes.begin()+at,bytes.begin()+at+4);
        for(auto at:shorts)std::reverse(bytes.begin()+at,bytes.begin()+at+2);
    }
    return bytes;
}

void scan_nijiiro(std::vector<TaikoCatalogSong>& songs) {
    const auto root=root_path();if(root.empty())return;
    const auto k=keys(root);
    const auto music=table(root,"musicinfo",k), words=table(root,"wordlist",k);
    std::map<std::string,std::pair<std::string,std::string>> titles;
    for(const auto& entry:words) {
        auto jp=entry.value("japaneseText",std::string{}),en=entry.value("englishUsText",std::string{});
        if(jp.find_first_not_of(" \t\r\n")==jp.npos)jp.clear();
        if(en.find_first_not_of(" \t\r\n")==en.npos)en.clear();
        // Megamix includes duplicate keys, sometimes blank or containing a
        // subtitle in later entries. Keep the first usable title for each key.
        if(!jp.empty() || !en.empty())
            titles.try_emplace(entry.at("key").get<std::string>(),en.empty()?jp:en,jp.empty()?en:jp);
    }
    const char* levels[]={"starEasy","starNormal","starHard","starMania","starUra"};
    const char* genres[]={"J-POP","ANIME","CHILDREN'S SONGS","VOCALOID","GAME MUSIC","NAMCO ORIGINAL","VARIETY","CLASSICAL"};
    size_t skipped=0,rejected_courses=0;
    std::set<std::string> seen;
    for(const auto& entry:music) {
        try {
            const auto id=entry.at("id").get<std::string>();
            if(id.empty() || id.size()>64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=id.npos)
                throw std::runtime_error("invalid Nijiiro song ID");
            if(!seen.insert(id).second)continue;
            const auto dir=root/"fumen"/id,audio=root/"sound"/("song_"+id+".nus3bank");
            if(!fs::is_regular_file(audio))throw std::runtime_error("missing song audio");
            TaikoCatalogSong song;song.genre="NIJIIRO";song.tja_path=utf8(dir);song.audio_path=utf8(audio);
            song.music_id="nj"+hash(utf8(root)+"/"+id).substr(0,12);
            song.title=id;song.original_title=id;
            if(auto it=titles.find("song_"+id);it!=titles.end()) {song.title=it->second.first;song.original_title=it->second.second;}
            if(auto it=titles.find("song_sub_"+id);it!=titles.end())song.custom_subtitle=it->second.first;
            const int genre=entry.value("genreNo",-1);
            song.custom_folder=genre>=0 && genre<8 ? genres[genre] : "OTHER";
            Assets accepted;
            for(unsigned d=0;d<5;++d) {
                if(!fs::is_regular_file(dir/(id+"_"+std::string(1,"enhmx"[d])+".bin")))continue;
                try {
                    const int level=entry.value(levels[d],0);
                    if(level<0 || level>10)throw std::runtime_error("invalid course rating");
                    auto assets=source_assets(dir,1u<<d);
                    double earliest=std::numeric_limits<double>::infinity();
                    for(const auto& [name,b]:assets)nijiiro_fumen(unpack(b,k,false),0,earliest);
                    if(earliest < -58000)throw std::runtime_error("course needs more than 60 seconds of lead-in");
                    accepted.merge(assets);song.difficulty_mask|=1u<<d;song.stars[d]=level;
                } catch(const std::exception& e) {
                    ++rejected_courses;
                    if(rejected_courses<=12)std::fprintf(stderr,"[nijiiro] %s/%c skipped: %s\n",id.c_str(),"enhmx"[d],e.what());
                }
            }
            if(!song.difficulty_mask)throw std::runtime_error("no compatible courses");
            song.custom_revision=assets_revision(accepted);
            // The 39.06 single-song bank template stores its preview cue here.
            // Guard the template; other layouts keep the safe start-of-song cue.
            std::ifstream bank(audio,std::ios::binary);std::string header(0x750,'\0');bank.read(header.data(),header.size());
            if(bank && header.substr(0,4)=="NUS3" && header.substr(0x748,4)=="PACK" &&
                header.substr(0x5ec,4)=="TONE" && word(header,0x5f0)==0x140) {
                const auto cue=word(header,0x6c4);if(cue<3600000)song.preview_ms=cue;
            }
            songs.push_back(std::move(song));
        } catch(const std::exception& e) {
            if(++skipped<=12)std::fprintf(stderr,"[nijiiro] song skipped: %s\n",e.what());
        }
    }
    std::fprintf(stderr,"[nijiiro] indexed %zu songs; skipped %zu songs and %zu incompatible courses\n",songs.size(),skipped,rejected_courses);
}

void convert_nijiiro(const TaikoCatalogSong& song) {
    const auto dir=path(song.tja_path),root=dir.parent_path().parent_path();
    auto assets=source_assets(dir,song.difficulty_mask);
    if(assets_revision(assets)!=song.custom_revision)throw std::runtime_error("Nijiiro charts changed since discovery; restart to refresh the library");
    const auto k=keys(root);double earliest=std::numeric_limits<double>::infinity();
    for(auto& [name,b]:assets) {b=unpack(b,k,false);nijiiro_fumen(b,0,earliest);}
    const double padding=std::isfinite(earliest)?std::max(0.,std::nearbyint(2000-earliest)):0;
    if(padding>60000)throw std::runtime_error("Nijiiro charts need more than 60 seconds of lead-in");
    const auto lead=unsigned(padding);const auto cache=path(song.custom_cache);fs::create_directories(cache);
    std::string marker=song.custom_revision+"\n"+std::to_string(lead)+"\n";
    for(auto& [name,b]:assets) {
        b=nijiiro_fumen(std::move(b),lead,earliest);publish(cache/(name+".bin"),b);
        marker+=name+" "+std::to_string(b.size())+" "+hash(b)+"\n";
    }
    publish(cache/"ready",marker);
}
}
