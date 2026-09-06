# Taiko+ standalone runtime

This is the implementation record for replacing Green's normal
`GameSongSelect` scene with a host-owned Taiko+ runtime while retaining stock
GameEnso gameplay and first-pass Results.

## Safety boundary

SDL, input, audio, decoder, and future network threads exchange pointer-free
semantic values only. Guest functions, guest object inspection, and guest
memory writes are restricted to the `SequenceController::update` hook on the
main PPU thread. Every match has a monotonically increasing generation;
commands and results from an older generation are rejected.

The public contracts live in `src/taiko_plus_runtime.h`. They contain a
versioned content identity, logical P1/P2 descriptions, match commands,
structured events, and errors. Commands and events are bounded. A failure is
reported once and returns the authoritative runtime to a usable browser state.

## Implemented foundation (2026-09-04)

- `TaikoPlusRuntime` owns the `Inactive -> Browser -> PreparingMatch ->
  LaunchingGameplay -> Gameplay -> Results -> Returning` state vocabulary,
  generation, and bounded command/event queues.
- Browser launch intent hashes the exact installed chart and `SONG_*.nub` with
  SHA-256. Missing content and unavailable difficulties fail before any guest
  mutation.
- The overlay no longer lends its mutable pixel buffer to the renderer.
  `taiko_host_frame_copy` queries or copies a coherent `HostFrameInfo` snapshot
  while holding the producer lock.
- Publishing host UI wakes SDL. A full-screen host frame is presented even
  when the RSX queue is empty, with redraws on expose, restore, and resize and
  a 60 Hz ceiling. Existing window letterboxing is retained.
- NUB/RIFF discovery, validation, FFmpeg decode, output-rate conversion, loop
  metadata, and the byte-bounded PCM LRU now live in one shared decoder used by
  both cellAtrac and Taiko+. Host selection resolves
  `SONG_<music_id>.nub` directly and decodes stereo float at 48 kHz.
- One external cellAudio mixer is installed for process lifetime. Standalone
  browser ownership ramps guest audio to silence over 100 ms; the gameplay
  handoff reverses the ramp without restarting the device. The host preview
  worker keeps only the newest request, debounces it for 150 ms, cancels stale
  decode work, rejects stale generations, and publishes a 100 ms crossfade.
  The callback allocates nothing, takes no lock, performs no file access or
  logging, and returns retired PCM to the worker for destruction.
- Authored RIFF `smpl` loops are retained and scaled to 48 kHz. A file without
  loop metadata stops at end of stream. The NSH cue field and `SE_SELECT` bank
  entry IDs are not yet trace-proven, so previews currently use the locked
  sample-zero fallback and UI SFX requests intentionally degrade to silence.
- `TAIKO_PLUS_STANDALONE=1` enables the host-only SequenceController freeze.
  Unset or `0` retains the GameSongSelect-backed diagnostic path. All hooks are
  additionally guarded by actual Taiko+ activation.
- Standalone interception now occurs at the beginning of the post-Entry
  destination factory `func_001FE470`, before either `GameSongSelect`
  allocation. It runs the factory's original outgoing-scene prefix first; the
  older `SequenceController::push_task` hook remains a guarded fallback.
- Until native manager/player lifetime is proved, a standalone Play command
  returns one `GuestBootstrapUnavailable` event and restores the browser. It
  intentionally does not manufacture a guest object or silently fall through
  to stock Song Select.

Automated coverage currently checks generation rejection, exactly-once
events, bounded queue overflow, concurrent command consumption, invalid
content rejection, SHA-256 hashing/error behavior, deterministic guest/host
gain ramps and preview crossfades, stale voice rejection, RIFF validation,
44.1-to-48-kHz conversion, loop scaling, cache reuse, cancellation, corrupt
input, path-safe music IDs, and direct `SONG_*.nub` discovery.

The 2026-09-04 native-Linux verification built `taiko_boot` with the normal
SDL_GPU, SDL3 audio/input, and in-process ATRAC configuration, passed all 13
CTest targets, and passed three consecutive `test-linux-headless.sh` boots and
post-attract soaks. The standalone scene interception and retained-manager
lifetime have not yet been live-validated; the feature therefore remains
explicitly opt-in and gameplay bootstrap remains closed.

### Live entry checkpoint (2026-09-06)

The user confirmed that selecting Taiko+ with the stock drum controls opens
the custom host menu. `taiko-plus-scene-commit-01.log` records
`SetNextScene` (`0x002287BC`) with a fourth raw-integer argument `99`, handoff
arming at Player Entry state 39, and standalone host activation. The native
Linux build used at most six jobs and passed all 14 CTest targets, including
the new Lumen bytecode and native-binding regressions.

This run took the later `SequenceController::push_task` fallback and supplied
an already-allocated task (`vtable=00F8BAE8`). It does NOT prove the early
destination interception, zero stock Song Select construction, or correct
native task/manager ownership. Those gates, gameplay bootstrap, synthetic P2,
and repeated gameplay/Results loops remain unfinished. Do not enable standalone
by default based solely on this entry checkpoint.

## Recovered native evidence

Post-Player Entry destination factory `func_001FE470(outgoing, scene_owner)`
first calls the owner membership predicate at vtable `+0x14` with
`(scene_owner, outgoing)`. If its byte result is nonzero, it calls the owner
removal operation at `+0x0c` once with the same arguments. Only then does it
inspect `outgoing + 0x1c`, allocate either normal Song Select (`0xf6c`,
`func_005C593C`) or Waiwai Song Select (`0xf84`, `func_005C5C4C`), and enqueue
it through the owner virtual at `+0x08`.
Standalone mode mirrors only that exact pre-allocation prefix and returns to
the host dispatcher. This interception still requires live lifetime testing;
the default legacy flag executes the complete original factory.

Normal Results continuation `func_001EBEB0(results, scene_owner)` performs:

1. read the music/session manager from `results + 0x0c`;
2. allocate `0xf6c` bytes through `func_0035D1A0`;
3. construct normal `GameSongSelect` through thunk `func_005C593C`, whose body
   transfers to `func_0062F0B8(new_scene, manager)`;
4. call the scene-owner virtual at vtable `+0x08` with
   `(scene_owner, new_scene, 0)`;
5. call the scene-owner membership predicate at `+0x14` with
   `(scene_owner, results)` and, on its nonzero byte return, the removal
   operation at `+0x0c` with the same pair.

`func_0062F0B8` stores the supplied manager at Song Select `+0x0c`, `+0xe28`,
and `+0xe64` during construction. This establishes the constructor argument
and shared-manager identity, but not an independently callable retain. No
standalone code retains this raw pointer yet.

The existing validated Song Select commit remains authoritative:
`func_007FCE6C(args, manager)` receives a live-vector index, native P1/P2
difficulty objects, and the mode flag. XML ordering is never used as that
index.

The stock Song Select dispatcher `func_001F5CA8` uses state 10 for that
selection commit, state 11 (`0x001F6338`) for the manager preload predicate,
state 12 for the 60/120-frame wait, and state 13 (`0x001F65C0`) for the actual
destination factory. For the normal two-player gameplay subtype, the factory
branch at `0x001F6AA0` performs this exact transaction:

1. call manager accessor thunk `func_005C544C(manager)` and, when nonnull,
   call `func_005C535C(returned_object, 0)`;
2. allocate `0x178` bytes with `func_0035D1A0`;
3. construct `GameEnso` with `func_001E1C04(new_scene, manager)`;
4. enqueue it with scene-owner virtual `+0x08(owner, new_scene, 0)`;
5. initialize the native stack configuration at `sp + 0x104`, seed its
   manager input at `sp + 0x78`, call
   `func_00251C08(sp + 0x78, sp + 0x104)`, then call
   `func_001DE520(new_scene, sp + 0x104)`;
6. query Song Select membership with scene-owner virtual
   `+0x14(owner, old_scene)` and, on a nonzero byte result, remove it with
   virtual `+0x0c(owner, old_scene)`.

`func_001E1C04` installs the `GameEnso` vtable address point `0x00f92c08`,
stores the supplied manager directly at object `+0x04`, initializes the
remaining subobjects, and performs no manager refcount increment. Its primary
destructor entries resolve to `func_001E10E4` and `func_001E1430`. Queue
acceptance is therefore not a boolean returned by `+0x08`; the stock code
unconditionally proceeds after the call and treats the nonnull allocation as
the construction-success condition. A standalone implementation must mirror
that ordering and use the native exception/cleanup paths rather than inventing
an extra acceptance convention.

The concrete `SequenceController` address point is `0x00f9ae70`. Its relevant
OPDs resolve `+0x08` to `func_008DA500` (queue task), `+0x0c` to
`func_008DDD30` (remove task), and `+0x14` to `func_008D427C` (test whether a
task is present in the controller's active container). This establishes the
standalone Results teardown primitive independently of `func_001EBEB0`; it
does not establish that the session manager remains owned after the last
guest scene referencing it is removed.

## Remaining native gate

The standalone gameplay transaction must not be enabled until live tracing
proves all of the following as one ownership chain:

- the manager's retain and release operations across Song Select destruction;
- the normal state-11 GameEnso allocation, constructor, queue acceptance,
  rollback, and ownership transfer;
- lower-level P1 preservation and P2 default/profile construction after Player
  Entry has been destroyed;
- the Results task-removal operation that does not allocate another
  `GameSongSelect`.

Each probe must use native destructors and complete a construct/destroy cycle
with no invalid free, TOCBAD, unresolved indirect, leaked task, or stale
pointer. A raw `0x4f0` Player Entry copy, fabricated vtable, or guest call from
a host thread is not an acceptable shortcut.

## Subsequent milestones

After the native gate passes:

1. Confirm the NSH preview-cue field across several stock traces and map move,
   difficulty, confirm, and cancel in `SE_SELECT`; then add the minimal bank
   entry extractor and fixed SFX voice pool.
2. Implement `GuestPlayerFactory` with Green's default/profile helpers and a
   development synthetic P2 fixture.
3. Reproduce the native state-11 GameEnso transaction behind the semantic
   command queue, hiding the host frame only after queue acceptance.
4. Detect stock Results and use the recovered removal path at both Results
   destinations, then reacquire browser/audio ownership.
5. Run the native-Linux acceptance suite: anonymous and authenticated P1,
   every difficulty including Ura, rapid 100-song browsing, ten-minute idle,
   synthetic P2 recreation, and 20 complete loops beyond the arcade limit.

Networking, custom songs, host-owned Results, remote judgement playback, and
Windows/Pi runtime validation remain outside this milestone.
