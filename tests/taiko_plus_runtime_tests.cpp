#include "taiko_plus_runtime.h"
#include "taiko_entry_callback.h"

#include <cassert>
#include <atomic>
#include <thread>

using namespace taiko_plus;

static MatchConfig match()
{
    MatchConfig result;
    result.content.game_revision = "S111 Green";
    result.content.music_id = "test";
    result.content.chart_hash.bytes[0] = 1;
    result.content.audio_hash.bytes[0] = 2;
    result.players[0].slot = PlayerSlot::P1;
    result.players[0].role = PlayerRole::Local;
    result.players[1].slot = PlayerSlot::P2;
    result.players[1].role = PlayerRole::SyntheticRemote;
    result.players[1].anonymous = false;
    result.players[1].profile = GuestPlayerProfile{
        kContractVersion, "remote-id", "REMOTE", 0, 0, 0};
    return result;
}

int main()
{
    // Raw AVM tags differ from converted native callback tags.
    assert(taiko_entry::is_plus_marker(4, 3, 99));
    assert(taiko_entry::is_plus_marker(4, 0x80000003u, 99));
    assert(!taiko_entry::is_plus_marker(4, 2, 99)); // boolean, not integer
    assert(!taiko_entry::is_plus_marker(4, 4, 99)); // float, not integer
    assert(!taiko_entry::is_plus_marker(0, 3, 99));
    assert(!taiko_entry::is_plus_marker(2, 3, 99));
    assert(!taiko_entry::is_plus_marker(3, 3, 99));
    assert(!taiko_entry::is_plus_marker(4, 3, 1));
    Runtime runtime;
    const uint64_t activation = runtime.activate();
    assert(activation != 0);
    assert(runtime.state() == State::Browser);

    assert(runtime.enqueue_launch(match()));
    const uint64_t generation = runtime.generation();
    assert(generation > activation);
    assert(runtime.state() == State::PreparingMatch);

    HostGuestCommand command;
    assert(runtime.try_pop_command(command));
    assert(command.kind == CommandKind::ConfigureAndLaunch);
    assert(command.match.generation == generation);
    assert(command.match.players[1].role == PlayerRole::SyntheticRemote);
    assert(!runtime.try_pop_command(command));

    assert(runtime.transition(generation, State::PreparingMatch,
                              State::LaunchingGameplay,
                              EventKind::MatchAccepted));
    assert(!runtime.transition(generation, State::PreparingMatch,
                               State::LaunchingGameplay,
                               EventKind::MatchAccepted));
    HostGuestEvent event;
    assert(runtime.try_pop_event(event));
    assert(event.kind == EventKind::MatchAccepted);
    assert(!runtime.try_pop_event(event));

    runtime.fail(generation, GuestErrorCode::WrongScene, "wrong scene");
    runtime.fail(generation, GuestErrorCode::WrongScene, "duplicate");
    assert(runtime.state() == State::Browser);
    assert(runtime.try_pop_event(event));
    assert(event.kind == EventKind::Failed);
    assert(event.error.code == GuestErrorCode::WrongScene);
    assert(!runtime.try_pop_event(event));

    /* A stale producer cannot mutate or complete a newer generation. */
    assert(runtime.enqueue_launch(match()));
    const uint64_t newer = runtime.generation();
    runtime.fail(generation, GuestErrorCode::InvalidContent, "stale");
    assert(runtime.generation() == newer);
    assert(runtime.state() == State::PreparingMatch);
    assert(!runtime.try_pop_event(event));

    /* Concurrent consumers still receive a command at most once. */
    std::atomic<unsigned> received{0};
    auto consume = [&] {
        HostGuestCommand item;
        if (runtime.try_pop_command(item)) ++received;
    };
    std::thread first(consume);
    std::thread second(consume);
    first.join();
    second.join();
    assert(received == 1);

    runtime.deactivate();
    assert(runtime.state() == State::Inactive);
    assert(!runtime.try_pop_event(event));

    Runtime overflow;
    overflow.activate();
    for (std::size_t index = 0; index < kCommandCapacity; ++index)
        assert(overflow.enqueue_abort());
    assert(!overflow.enqueue_abort());
    assert(overflow.state() == State::Browser);
    assert(overflow.try_pop_event(event));
    assert(event.kind == EventKind::Failed);
    assert(event.error.code == GuestErrorCode::CommandQueueFull);
    assert(!overflow.try_pop_event(event));
    assert(!overflow.try_pop_command(command));

    Runtime invalid;
    invalid.activate();
    MatchConfig bad = match();
    bad.content.chart_hash = {};
    assert(!invalid.enqueue_launch(std::move(bad)));
    assert(invalid.state() == State::Browser);
    assert(invalid.try_pop_event(event));
    assert(event.error.code == GuestErrorCode::InvalidContent);

    Runtime invalid_player;
    invalid_player.activate();
    MatchConfig missing_identity = match();
    missing_identity.players[1].profile->player_id.clear();
    assert(!invalid_player.enqueue_launch(std::move(missing_identity)));
    assert(invalid_player.try_pop_event(event));
    assert(event.error.code == GuestErrorCode::PlayerUnavailable);

    Runtime invalid_anonymous;
    invalid_anonymous.activate();
    MatchConfig fabricated_anonymous = match();
    fabricated_anonymous.players[0].profile = GuestPlayerProfile{
        kContractVersion, "fabricated", "P1", 0, 0, 0};
    assert(!invalid_anonymous.enqueue_launch(std::move(fabricated_anonymous)));
    assert(invalid_anonymous.try_pop_event(event));
    assert(event.error.code == GuestErrorCode::PlayerUnavailable);
    return 0;
}
