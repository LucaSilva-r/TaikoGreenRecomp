#ifndef TAIKO_PLUS_RUNTIME_H
#define TAIKO_PLUS_RUNTIME_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace taiko_plus {

inline constexpr uint32_t kContractVersion = 1;
inline constexpr std::size_t kCommandCapacity = 8;
inline constexpr std::size_t kEventCapacity = 16;

enum class State : uint8_t {
    Inactive,
    Browser,
    PreparingMatch,
    LaunchingGameplay,
    Gameplay,
    Results,
    Returning,
    Error,
};

struct Sha256 {
    std::array<uint8_t, 32> bytes{};

    bool empty() const;
    friend bool operator==(const Sha256& left, const Sha256& right)
    {
        return left.bytes == right.bytes;
    }
};

struct ContentIdentity {
    uint32_t version = kContractVersion;
    std::string game_revision;
    std::string music_id;
    uint32_t unique_id = 0;
    uint8_t difficulty = 0;
    Sha256 chart_hash;
    Sha256 audio_hash;
};

enum class PlayerSlot : uint8_t { P1, P2 };
enum class PlayerRole : uint8_t { Local, SyntheticRemote };

struct GuestPlayerProfile {
    uint32_t version = kContractVersion;
    std::string player_id;
    std::string display_name;
    uint32_t body_cosmetic = 0;
    uint32_t head_cosmetic = 0;
    uint32_t drum_cosmetic = 0;
};

struct PlayerSpec {
    PlayerSlot slot = PlayerSlot::P1;
    PlayerRole role = PlayerRole::Local;
    bool anonymous = true;
    std::optional<GuestPlayerProfile> profile;
};

struct MatchConfig {
    uint32_t version = kContractVersion;
    uint64_t generation = 0;
    ContentIdentity content;
    std::array<PlayerSpec, 2> players;
};

enum class CommandKind : uint8_t { ConfigureAndLaunch, Abort };

struct HostGuestCommand {
    CommandKind kind = CommandKind::ConfigureAndLaunch;
    MatchConfig match;
};

enum class EventKind : uint8_t {
    MatchAccepted,
    GameplayStarted,
    ResultsStarted,
    ReturnedToBrowser,
    Failed,
};

enum class GuestErrorCode : uint8_t {
    None,
    CommandQueueFull,
    EventQueueFull,
    StaleGeneration,
    WrongScene,
    InvalidContent,
    PlayerUnavailable,
    GuestBootstrapUnavailable,
    PartialConstructionFailed,
};

struct GuestError {
    GuestErrorCode code = GuestErrorCode::None;
    std::string detail;
};

struct HostGuestEvent {
    EventKind kind = EventKind::Failed;
    uint64_t generation = 0;
    GuestError error;
};

/* Process-wide owner. All methods are thread-safe. Guest pointers deliberately
 * do not appear in this interface. */
class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    uint64_t activate();
    void deactivate();
    State state() const;
    uint64_t generation() const;

    bool enqueue_launch(MatchConfig match);
    bool enqueue_abort();
    bool try_pop_command(HostGuestCommand& command);
    bool try_pop_event(HostGuestEvent& event);

    bool transition(uint64_t generation, State expected, State next,
                    EventKind event);
    void fail(uint64_t generation, GuestErrorCode code, std::string detail);
    void return_to_browser(uint64_t generation);

private:
    struct Impl;
    Impl* impl_;
};

Runtime& runtime();
bool standalone_enabled();

} // namespace taiko_plus

#endif /* TAIKO_PLUS_RUNTIME_H */
