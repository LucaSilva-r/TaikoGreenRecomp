#ifndef TAIKO_CUSTOM_SONGS_H
#define TAIKO_CUSTOM_SONGS_H
#include "taiko_catalog.h"
#include <vector>
#include <cstdio>

void taiko_custom_scan(std::vector<TaikoCatalogSong>& songs);
bool taiko_custom_identity(const TaikoCatalogSong& song, unsigned difficulty,
                           taiko_plus::ContentIdentity& identity, std::string* error);
bool taiko_custom_prepare(const TaikoCatalogSong& song, std::string& error);
FILE* taiko_custom_open(const char* guest, uint32_t flags);
int taiko_custom_stat(const char* guest, uint64_t* size);
const TaikoCatalogSong* taiko_custom_find(std::string_view id);
#endif
