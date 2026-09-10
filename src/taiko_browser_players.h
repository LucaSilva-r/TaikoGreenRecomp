#ifndef TAIKO_BROWSER_PLAYERS_H
#define TAIKO_BROWSER_PLAYERS_H

#include <array>
#include <cstdint>

namespace taiko_plus {

// Value-only browser state. The frontend serializes access with its action lock.
struct BrowserPlayers {
    bool expanded = false;
    void collapse() { expanded = false; ready = 0; }
    void open(uint8_t available) { expanded = available != 0; ready = 0; normalize(available); }
    uint8_t cursors(unsigned course) const {
        if (!expanded) return 0;
        uint8_t result = 0;
        for (unsigned p = 0; p < 2; ++p)
            if ((joined & (1u << p)) && difficulty[p] == course) result |= 1u << p;
        return result;
    }
    uint8_t joined = 0;
    uint8_t ready = 0;
    unsigned focus = 0;
    std::array<uint8_t, 2> difficulty{3, 3};

    void join(unsigned player) {
        if (player > 1) return;
        const uint8_t bit = 1u << player;
        if (!(joined & bit)) ready = 0;
        joined |= bit;
        focus = player;
    }
    void leave(unsigned player) {
        if (player > 1) return;
        joined &= ~(1u << player);
        ready = 0; // A changed lineup must confirm again; never auto-launch.
        if (focus == player) focus = joined & 1 ? 0 : joined & 2 ? 1 : 0;
        if (!joined) expanded = false;
    }
    void song_changed(uint8_t available) {
        collapse();
        normalize(available);
    }
    void normalize(uint8_t available) {
        for (auto& value : difficulty)
            for (unsigned i = 0; i < 5 && !(available & (1u << value)); ++i)
                value = (value + 1) % 5;
    }
    void change_difficulty(unsigned player, int direction, uint8_t available) {
        if (player > 1 || !available) return;
        join(player);
        auto& value = difficulty[player];
        do { value = (int(value) + direction + 5) % 5; }
        while (!(available & (1u << value)));
        ready &= ~(1u << player);
    }
    bool confirm(unsigned player) {
        if (player > 1) return false;
        join(player);
        ready |= 1u << player;
        return ready == joined;
    }
};
}
#endif
