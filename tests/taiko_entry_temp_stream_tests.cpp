/* Standalone native/MinGW smoke test for the real overlay stream helper. */
#include "../src/taiko_entry_overlay.cpp"
#include <cassert>

extern "C" const unsigned char taiko_entry_overlay_patch_data[] = {0};
extern "C" const unsigned taiko_entry_overlay_patch_size = 0;

int main()
{
    FILE* first = temporary_stream();
    FILE* second = temporary_stream();
    assert(first && second);
    const unsigned char bytes[] = {0, 10, 13, 26, 128, 255};
    assert(std::fwrite(bytes, 1, sizeof(bytes), first) == sizeof(bytes));
    assert(std::fflush(first) == 0);
    assert(std::fseek(first, 0, SEEK_SET) == 0);
    unsigned char actual[sizeof(bytes)] = {};
    assert(std::fread(actual, 1, sizeof(actual), first) == sizeof(actual));
    assert(std::memcmp(bytes, actual, sizeof(bytes)) == 0);
    assert(std::fgetc(second) == EOF);
#ifdef _WIN32
    wchar_t path[1024];
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(first)));
    DWORD length = GetFinalPathNameByHandleW(handle, path, 1024, FILE_NAME_NORMALIZED);
    assert(length && length < 1024);
#endif
    assert(std::fclose(first) == 0);
    assert(std::fclose(second) == 0);
#ifdef _WIN32
    assert(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
    assert(GetLastError() == ERROR_FILE_NOT_FOUND);
#endif
    std::puts("overlay temporary stream passed");
}
