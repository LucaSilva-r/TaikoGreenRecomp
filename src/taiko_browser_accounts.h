#ifndef TAIKO_BROWSER_ACCOUNTS_H
#define TAIKO_BROWSER_ACCOUNTS_H

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace taiko_plus {

// Account identity is independent of joining a match. Only completed native
// login transactions replace it; input and renderer threads own no guest data.
struct BrowserAccount {
    std::string name;
    bool authenticated = false;
};

enum class AccountAction { None, Login, Logout, Leave };
enum class AccountPhase { Idle, WaitingForCard, ChoosePlayer, Loading, Failed };

struct BrowserAccounts {
    std::array<BrowserAccount, 2> players;
    AccountPhase phase = AccountPhase::Idle;
    int panel = -1;
    unsigned selection = 0;
    int destination = -1;
    std::string status;
    uint64_t generation = 0;
    // Native callbacks have no host generation field. A cancelled load must
    // retain exclusive ownership of its receiving storage until it drains.
    uint64_t native_generation = 0;
    bool native_pending = false;

    bool native_started(uint64_t token) {
        if (native_pending || token != generation || phase != AccountPhase::Loading) return false;
        native_pending = true;
        native_generation = token;
        return true;
    }
    bool native_finished(uint64_t token) {
        if (!native_pending || token != native_generation) return false;
        native_pending = false;
        return true;
    }

    bool busy() const { return native_pending || (phase != AccountPhase::Idle && phase != AccountPhase::Failed); }
    void open(unsigned slot) {
        if (slot > 1 || busy()) return;
        panel = static_cast<int>(slot);
        selection = 0;
        status.clear();
    }
    uint64_t begin() {
        if (busy()) return 0;
        phase = AccountPhase::WaitingForCard;
        destination = -1;
        status = "Present BanaPassport";
        return ++generation;
    }
    bool card_ready(uint64_t token) {
        if (token != generation || phase != AccountPhase::WaitingForCard) return false;
        phase = AccountPhase::ChoosePlayer;
        status = "Assign card: P1 or P2";
        return true;
    }
    bool assign(unsigned slot) {
        if (slot > 1 || phase != AccountPhase::ChoosePlayer) return false;
        destination = static_cast<int>(slot);
        phase = AccountPhase::Loading;
        status = "Loading player data...";
        return true;
    }
    bool complete(uint64_t token, BrowserAccount account) {
        if (token != generation || phase != AccountPhase::Loading || destination < 0 || destination > 1 ||
            !account.authenticated) return false;
        players[destination] = std::move(account);
        phase = AccountPhase::Idle;
        panel = -1;
        status = "Player logged in";
        return true;
    }
    void fail(uint64_t token, std::string detail) {
        if (token != generation || !busy()) return;
        phase = AccountPhase::Failed;
        status = std::move(detail);
    }
    void cancel() {
        ++generation;
        phase = AccountPhase::Idle;
        destination = -1;
        panel = -1;
        status.clear();
    }
};
}
#endif
