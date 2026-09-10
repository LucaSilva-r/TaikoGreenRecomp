#pragma once
#include "taiko_chart.h"
#include <array>
#include <map>
#include <vector>

namespace taiko_chart {
std::string trim(std::string value);
std::vector<std::string> split(std::string_view value, char delimiter);
double number(const std::string& value);
int integer(const std::string& value);
struct Note {
    int type = 0, hits = 0, score = 0, score_diff = 0, diff = 0;
    double pos = 0, duration = 0, absolute = 0;
    bool manual = false, multimeasure = false;
};
struct Branch { double speed = 0; std::vector<Note> notes; };
struct Measure {
    double bpm = 0, offset = 0, end = 0, duration = 0;
    bool gogo = false, barline = true;
    std::array<int, 6> condition{-1,-1,-1,-1,-1,-1};
    std::array<Branch, 3> branches;
};
struct Fumen {
    int course = 3, level = 1;
    std::array<int,22> header{0,10000,8000,10,5,-20,65536,65536,65536,
                            20,10,0,1,20,10,1,30,30,20,12345678,0,0};
    std::vector<Measure> measures;
};
bool combo(int type);
void finish(Fumen& fumen, int notes);
std::vector<Fumen> tja_fumens(const std::string& raw);
Fumen osu_fumen(const std::string& raw, int level);
}
