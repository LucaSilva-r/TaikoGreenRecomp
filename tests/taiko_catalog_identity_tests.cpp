#include "taiko_catalog.h"

#include <cassert>
#include <filesystem>
#include <fstream>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "taiko_catalog_identity_tests";
    std::filesystem::create_directories(root);
    const std::filesystem::path file = root / "abc.bin";
    {
        std::ofstream output(file, std::ios::binary);
        output << "abc";
    }
    taiko_plus::Sha256 hash;
    std::string error;
    assert(taiko_hash_file_sha256(file.string(), hash, &error));
    static constexpr uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    for (std::size_t index = 0; index < hash.bytes.size(); ++index)
        assert(hash.bytes[index] == expected[index]);
    assert(!hash.empty());
    assert(!taiko_hash_file_sha256((root / "missing").string(), hash,
                                   &error));
    assert(hash.empty());
    std::filesystem::remove(file);
    std::filesystem::remove(root);
    return 0;
}
