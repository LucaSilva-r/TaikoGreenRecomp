# Online multiplayer roadmap

Status: implementation in progress; Player Entry replacement validated
Initial development branch: `full-override`
Last updated: 2026-08-29

This document is the durable project roadmap for turning Green into a
server-backed PC-style game while retaining the original gameplay engine. It
records the decisions already made so work can be interrupted and resumed
without reopening the architectural discussion.

Detailed reverse-engineering notes live in
[`player_entry_reversing.md`](player_entry_reversing.md) and
[`song_select_reversing.md`](song_select_reversing.md). The repeat transition
is recorded in [`results_reversing.md`](results_reversing.md).

## V1 target

V1 supports two Linux clients in a server-authored 1v1 room. Each machine owns
one local player, both players use the same stock song and difficulty, and the
opponent is presented through the original P2 gameplay lane.

The host-native UI owns login, lobby, Song Select, and Results. The guest owns
the actual Taiko gameplay scene. Integration uses reconstructed game functions
and manager contracts; it must not automate Lumen or rely on undocumented bulk
copies of guest memory.

Locked decisions:

- Linux first; keep interfaces portable for Windows later.
- One local player per client and exactly two players per V1 match.
- Existing silvaserv-backed profiles provide identity and appearance.
- Stock Green songs only; both clients must match the chart and audio hashes.
- The room owner proposes one song and difficulty; both clients must become
  ready before the server starts the match.
- Judgement is immediate and local. The server validates the event stream,
  recomputes the canonical result, and relays accepted events.
- Each client maps itself to P1 and applies the opponent to the original P2
  lane.
- Login, lobby, Song Select, and Results are an in-window host UI.
- Results are not persisted in V1. Arcade scores, crowns, rewards, and
  progression remain untouched.
- A disconnect after match start aborts the match and returns the surviving
  client to the lobby. Reconnection is out of scope for V1.
- The original Player Entry controller may perform its invisible setup and
  teardown, but profile data is applied through internal manager functions and
  its interactive Lumen flow is skipped.
- Anonymous play uses Green's genuine no-card/offline join operation and its
  initialized default player record. It must not construct a fake guest
  profile.

## Architectural boundary

The permanent seam is:

```
host UI/network -> host/guest command queue -> safe main-PPU dispatch
                                            -> original guest managers/gameplay

original guest managers/gameplay -> host/guest event queue -> host UI/network
```

SDL, UI, and network threads must never call guest functions or mutate guest
objects directly. Commands execute only from a verified safe point in the
guest's main flow. Every command is scene-guarded and reports success or a
structured failure back to the host.

## Macrotasks

### 1. Host/guest control plane

- Add bounded, thread-safe command and event queues.
- Establish a safe main-PPU dispatch point.
- Detect the active scene and reject commands in invalid states.
- Define semantic commands instead of exposing arbitrary guest reads/writes.
- Leave the bridge disabled by default so the arcade flow remains unchanged.

Completion gate: native code can query state and issue repeatable guarded guest
commands without races, reentrancy, or UI-thread guest-memory access.

### 2. Player session and Player Entry replacement — complete

- Finish reconstructing the BAID, userdata, and crowns decoded-result
  application path.
- Preserve the native anonymous/no-card join as a separate path; do not route
  it through `GuestPlayerProfile`.
- Identify the canonical player managers and required Player Entry setup and
  teardown invariants.
- Add the minimal host-facing silvaserv profile endpoint needed by login.
- Convert the server response into a stable `GuestPlayerProfile` and apply it
  through original internal functions.
- Install the local user as P1 and leave P2 empty until a match is configured.
- Skip the visible and interactive Player Entry Lumen states.

Completion checkpoint (2026-08-29): the opt-in host frontend presents the
Anonymous/BanaPassport menu. Anonymous invokes Green's native no-card callback;
BanaPassport retains the virtual reader, `baidcheck.php`, decoded profile, and
costume-loading path, then invokes the captured native online-player commit.
Both paths invoke the native game-mode callback, userdata controller, and
normal Player Entry teardown. Live native-Linux runs verified
`6/7 -> 35 -> 23 -> 27 -> 30 -> 39 -> 40`; the title then loads its original
Song Select scene underneath the opaque host Song Select shell. No Player Entry
or Card Select Lumen interaction is required.

Completion gate: from Player Entry, the host UI offers Anonymous and
BanaPassport. Anonymous performs the stock no-card join; BanaPassport uses the
existing card/server profile path. Either choice installs a valid P1 and reaches
the host-owned Song Select shell without interacting with Player Entry or Song
Select Lumen.

### 3. Host-owned stock Song Select — current milestone

- Expose the installed Green catalog to the host.
- Give each chart a stable identity containing song ID, difficulty, chart hash,
  and audio hash.
- Reverse the authoritative Song Select-to-gameplay configuration contract.
- Apply song and difficulty directly through guest managers.
- Bypass original selection timers, credit consumption, and song-count limits.

Implementation checkpoint (2026-08-29): the host browser loads 800 installed
and playable entries from `musicinfo.xml` plus the fumen tree. A guarded hook
in normal `game::GameSongSelect` resolves the textual music ID against the
title's live 853-record manager vector, writes the existing P1 difficulty
object, and invokes the stock state-10 commit routine. A native-Linux run
selected `evedrm` Oni and opened `evedrm_m.bin` plus `SONG_EVEDRM.nub`, then
completed gameplay and entered Results. The remaining work is the repeatable
Results-to-host-browser loop, content hashes, and broader song/difficulty
validation; this macrotask is not complete yet.

Repeat-loop checkpoint (2026-08-30): normal Results now retains its complete
presentation and cleanup, then Green's native “another song” operation creates
the next `GameSongSelect` with the same manager while the host browser
reacquires input. An authenticated live run completed `kr01` Oni and launched
`kim69` Oni without another coin, Player Entry, or card authentication. After
song 2 reached `played=2 limit=2`, the arcade-limit/final-session branch was
redirected through that same native operation and successfully launched a
third `kr01` Oni. Both Results destinations are live-validated.

Browser UX checkpoint (2026-08-30): the lightweight overlay now follows the
osu!lazer Song Select composition with a nine-row carousel, persistent song
and difficulty details, match counts, keyboard/mouse fast navigation, random
selection, title/genre/music-ID text filtering, alphabetic sorting, and Green's
nine genre folders. The folders are now a distinct first-level selection, and
opened folders repeat a selectable return-to-categories row after every ten
songs. Search launched from the folder screen spans the complete catalog;
search launched inside a folder remains folder-scoped. FreeType-rendered titles
accept optional music-ID-keyed English
overrides without changing the native selection identity. The cabinet
rim/centre path and the native Green commit transaction are unchanged.

Completion gate: a host command can repeatedly start any installed stock song
at the requested difficulty with the correct fumen and audio.

### 4. Gameplay lifecycle and telemetry

- Identify gameplay preload completion, clock/audio start, judgement commit,
  song end, abort, and cleanup boundaries.
- Add a pre-start barrier so the scene can load without starting its clocks.
- Export each local judgement exactly once with a stable note index and
  match-relative timestamp.
- Reconstruct the internal operation that applies a judgement to P2.
- Export final local totals independently of the original Results screen.

Completion gate: one client can start on a scheduled monotonic deadline, export
a complete judgement trace, replay a recorded trace into P2, and abort safely.

### 5. Host-owned Results and repeatable local loop

- Preserve original gameplay teardown while bypassing its Results presentation.
- Convert guest totals into a stable `PlayerResult`.
- Display Results in the host UI.
- Return to the host Song Select without consuming credits or returning to
  attract.

Completion gate: at least 20 consecutive local songs complete the
select-gameplay-results-select loop without stale player/chart state.

### 6. Server lobby and match protocol

- Extend silvaserv with authenticated rooms, membership, ownership, song
  selection, readiness, and server-authored match phases.
- Use HTTPS for login/profile operations and a persistent TLS WebSocket for
  lobby and match traffic.
- Treat clients as intent senders; the server owns room revisions, player
  assignments, locked match configuration, and final state.
- Add session and match IDs, sequencing, idempotency, heartbeat, clock
  synchronization, and content-hash negotiation.
- After both clients report gameplay preloaded, publish a common future start
  deadline.

Completion gate: two synthetic clients can create and join a room, lock matching
content, become ready, and receive the same deterministic start configuration.

### 7. Synchronized 1v1 gameplay

- Map the local player to P1 and the remote player to P2 on each client.
- Release both preloaded scenes at the server-scheduled deadline.
- Send local per-note judgements without delaying local gameplay.
- Validate note identity, ordering, duplicates, timing bounds, and score
  progression on the server.
- Relay accepted events and apply them to the opponent's P2 lane.
- Detect stale, duplicate, incomplete, or mismatched streams.
- Abort cleanly on disconnect or content mismatch.

Completion gate: under simulated latency and jitter, starts stay within one
60 Hz frame, local timing is unaffected, P2 receives every accepted event once,
and both clients agree on the event stream.

### 8. Server-finalized Results and online match loop

- Recompute the canonical result from accepted judgement events.
- Finalize only after both event streams complete or the match aborts.
- Deliver the same immutable `MatchResult` to both clients.
- Support rematch and return-to-selection flows.
- Add protocol mismatch, timeout, server-loss, and shutdown UI.
- Soak repeated matches with injected latency, jitter, delayed messages,
  duplicates, and disconnects.

Completion gate: two real clients can log in, join a room, choose a song, play
with live P2 presentation, receive identical Results, and repeat reliably.

## Stable interfaces to introduce

- `HostGuestCommand`: apply profile, configure/preload match, arm start, apply
  remote judgement, finish/abort, and return to the host shell.
- `HostGuestEvent`: guest ready, profile applied, gameplay loaded/started, local
  judgement, gameplay ended, local result, and guest error.
- `GuestPlayerProfile`: authenticated server identity plus only the game-facing
  fields proven necessary for name, appearance, and gameplay configuration.
  Anonymous play deliberately has no `GuestPlayerProfile`.
- `ContentIdentity`: game revision, song ID, difficulty, chart hash, and audio
  hash.
- `MatchConfig`: match ID, player assignments, content identity, and scheduled
  start.
- `JudgementEvent`: match ID, player ID, sequence number, note index, judgement,
  and match-relative timestamp.
- `MatchResult`: the server-finalized per-player totals and outcome.

All bridge and network structures are versioned and validated before use.

## Resume checkpoint

The next planning/implementation session begins macrotask 3, with macrotask
1's minimum dispatch mechanism continuing alongside it. Player Entry
replacement is validated for Anonymous and BanaPassport. The immediate
questions are:

1. Live-validate the reconstructed normal `GameSongSelect` state-10 commit for
   arbitrary stock songs and difficulties.
2. Determine the native cleanup/re-entry contract needed to repeat gameplay
   without returning to arcade Song Select or attract.
3. How should the current scene-local dispatcher become the reusable command
   boundary needed by gameplay and Results?

Known verified Player Entry addresses, transitions, and live traces are kept in
`player_entry_reversing.md`; do not rediscover them from scratch.

## Post-V1 work

Custom songs and distribution, persistent scores/progression, more than two
players, independent difficulties, spectators, reconnect support, native
Windows validation, original Results-screen compatibility, and stronger
anti-cheat are intentionally deferred until the V1 loop is stable.
