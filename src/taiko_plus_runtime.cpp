#include "taiko_plus_runtime.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace taiko_plus {

namespace {

bool valid_profile(const PlayerSpec& player)
{
    if (player.anonymous)
        return !player.profile.has_value();
    if (!player.profile || player.profile->version != kContractVersion)
        return false;
    return !player.profile->player_id.empty() &&
           player.profile->player_id.size() <= 128 &&
           !player.profile->display_name.empty() &&
           player.profile->display_name.size() <= 64;
}

} // namespace

bool Sha256::empty() const
{
    for (uint8_t byte : bytes)
        if (byte) return false;
    return true;
}

struct Runtime::Impl {
    mutable std::mutex lock;
    State state = State::Inactive;
    uint64_t generation = 0;
    std::deque<HostGuestCommand> commands;
    std::deque<HostGuestEvent> events;
    uint32_t emitted_event_mask = 0;

    void push_event(HostGuestEvent event)
    {
        /* An event must never be silently lost. The host consumes events in
         * FIFO order; if it stopped doing so, replace the oldest item with one
         * explicit overflow failure rather than pretending completion. */
        if (events.size() == kEventCapacity) {
            events.pop_front();
            event.kind = EventKind::Failed;
            event.error.code = GuestErrorCode::EventQueueFull;
            event.error.detail = "host/guest event queue overflow";
        }
        events.emplace_back(std::move(event));
    }

    bool event_once(EventKind kind)
    {
        const uint32_t bit = 1u << static_cast<unsigned>(kind);
        if (emitted_event_mask & bit) return false;
        emitted_event_mask |= bit;
        return true;
    }

    void fail_locked(uint64_t failed_generation, GuestErrorCode code,
                     std::string detail)
    {
        if (failed_generation != generation ||
            !event_once(EventKind::Failed))
            return;
        state = State::Browser;
        commands.clear();
        push_event({EventKind::Failed, failed_generation,
                    {code, std::move(detail)}});
    }
};

Runtime::Runtime() : impl_(new Impl) {}
Runtime::~Runtime() { delete impl_; }

uint64_t Runtime::activate()
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    ++impl_->generation;
    impl_->state = State::Browser;
    impl_->commands.clear();
    impl_->events.clear();
    impl_->emitted_event_mask = 0;
    return impl_->generation;
}

void Runtime::deactivate()
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    ++impl_->generation;
    impl_->state = State::Inactive;
    impl_->commands.clear();
    impl_->events.clear();
    impl_->emitted_event_mask = 0;
}

State Runtime::state() const
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->state;
}

uint64_t Runtime::generation() const
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->generation;
}

bool Runtime::enqueue_launch(MatchConfig match)
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (impl_->state != State::Browser) {
        impl_->fail_locked(impl_->generation, GuestErrorCode::WrongScene,
                           "launch requested outside browser state");
        return false;
    }
    ++impl_->generation;
    impl_->emitted_event_mask = 0;
    match.version = kContractVersion;
    match.generation = impl_->generation;
    const bool content_valid =
        match.content.version == kContractVersion &&
        !match.content.game_revision.empty() &&
        match.content.game_revision.size() <= 64 &&
        !match.content.music_id.empty() &&
        match.content.music_id.size() <= 64 &&
        match.content.difficulty < 5 &&
        !match.content.chart_hash.empty() &&
        !match.content.audio_hash.empty();
    bool players_valid = match.players[0].enabled || match.players[1].enabled;
    for (unsigned slot = 0; slot < 2; ++slot) {
        const auto& player = match.players[slot];
        if (!player.enabled) continue;
        players_valid = players_valid &&
            static_cast<unsigned>(player.slot) == slot &&
            player.role == PlayerRole::Local && valid_profile(player) &&
            player.difficulty < 5 && !player.chart_hash.empty();
    }
    if (!content_valid || !players_valid) {
        impl_->fail_locked(impl_->generation,
            !content_valid ? GuestErrorCode::InvalidContent
                           : GuestErrorCode::PlayerUnavailable,
            !content_valid ? "invalid match content identity"
                           : "invalid logical player specification");
        return false;
    }
    if (impl_->commands.size() == kCommandCapacity) {
        impl_->push_event({EventKind::Failed, impl_->generation,
            {GuestErrorCode::CommandQueueFull,
             "host/guest command queue overflow"}});
        return false;
    }
    impl_->commands.push_back(
        {CommandKind::ConfigureAndLaunch, std::move(match)});
    impl_->state = State::PreparingMatch;
    return true;
}

bool Runtime::enqueue_abort()
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (impl_->state == State::Inactive)
        return false;
    if (impl_->commands.size() == kCommandCapacity) {
        impl_->fail_locked(impl_->generation,
                           GuestErrorCode::CommandQueueFull,
                           "host/guest command queue overflow");
        return false;
    }
    MatchConfig match;
    match.generation = impl_->generation;
    impl_->commands.push_back({CommandKind::Abort, std::move(match)});
    return true;
}

bool Runtime::try_pop_command(HostGuestCommand& command)
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    while (!impl_->commands.empty()) {
        HostGuestCommand next = std::move(impl_->commands.front());
        impl_->commands.pop_front();
        if (next.match.generation == impl_->generation) {
            command = std::move(next);
            return true;
        }
        impl_->push_event({EventKind::Failed, next.match.generation,
            {GuestErrorCode::StaleGeneration,
             "discarded stale host/guest command"}});
    }
    return false;
}

bool Runtime::try_pop_event(HostGuestEvent& event)
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (impl_->events.empty()) return false;
    event = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

bool Runtime::transition(uint64_t generation, State expected, State next,
                         EventKind event)
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (generation != impl_->generation || impl_->state != expected)
        return false;
    impl_->state = next;
    if (impl_->event_once(event))
        impl_->push_event({event, generation, {}});
    return true;
}

void Runtime::fail(uint64_t generation, GuestErrorCode code,
                   std::string detail)
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->fail_locked(generation, code, std::move(detail));
}

void Runtime::return_to_browser(uint64_t generation)
{
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (generation != impl_->generation) return;
    impl_->state = State::Browser;
    if (impl_->event_once(EventKind::ReturnedToBrowser))
        impl_->push_event({EventKind::ReturnedToBrowser, generation, {}});
}

Runtime& runtime()
{
    static Runtime instance;
    return instance;
}

bool standalone_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("TAIKO_PLUS_STANDALONE");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

} // namespace taiko_plus
