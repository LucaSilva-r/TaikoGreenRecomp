# Taiko+ standalone runtime

This is the implementation record for replacing Green's normal
`GameSongSelect` scene with a host-owned Taiko+ runtime while retaining stock
GameEnso gameplay and first-pass Results.

## Safety boundary

SDL, input, audio, decoder, and future network threads exchange pointer-free
semantic values only. Guest functions, guest object inspection, and guest
memory writes are restricted to the native task update on the main PPU thread. Every match has a monotonically increasing generation;
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
- `TAIKO_PLUS_STANDALONE=1` enables the standalone path. Unset or `0`
  retains the GameSongSelect-backed diagnostic path. Native frame processing,
  including deferred scene destruction, continues in both modes.
- Standalone interception runs after `GameSongSetup` populates the session,
  before it removes itself or allocates Song Select. The completed setup task
  remains as an idle session anchor. Its update services the semantic command
  queue with the current frame's native scene-owner facade.
- Play resolves the music ID in the live song vector, preserves native P1,
  obtains guest P2 through the native player-map default constructor, commits
  selection, requests the shared transition and waits the native 120-frame interval, then queues
  and configures GameEnso. This implementation is undergoing live validation.
- Gameplay releases frontend input and ramps audio back to the guest. Both
  Results destinations remove Results without constructing Song Select and
  reacquire the host browser/audio. Repeated live loops remain unvalidated.
- The development P2 is anonymous. Versioned remote profile import is not
  implemented and is rejected explicitly; the frontend no longer advertises
  an authenticated `REMOTE` profile that the guest has never received.

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

The original fallback suppressed an already allocated task with vtable
`0x00f8bae8`. RTTI and its update body identify this as **GameSongSetup**, not
Song Select. Suppressing it prevented the native catalog from being built.

The next live run (`build-linux/taiko-plus-setup-owner-02.log`) proved that
allowing setup populated `manager+0x434..0x438` with 853 native song records.
It also exposed the session lifetime rule: removing setup without a replacement
scene caused the parent to return to attract and clear the catalog before
Play. Keeping only the embedded manager address is therefore insufficient.
The retained setup anchor addresses this lifecycle condition; it still needs
live validation. No successful standalone gameplay is claimed by this run.

## Recovered native evidence

Post-Player Entry destination factory `func_001FE470(outgoing, scene_owner)`
first calls the owner membership predicate at vtable `+0x14` with
`(scene_owner, outgoing)`. If its byte result is nonzero, it calls the owner
removal operation at `+0x0c` once with the same arguments. Only then does it
inspect `outgoing + 0x1c`, allocate either normal Song Select (`0xf6c`,
`func_005C593C`) or Waiwai Song Select (`0xf84`, `func_005C5C4C`), and enqueue
it through the owner virtual at `+0x08`.
This is an alternate destination path, not the observed Player Entry route.
The observed route is `GameSongSetup::update` (`0x000e4608`): after native
catalog/player preparation, interception precedes the membership/removal
prefix and all Song Select allocation. The default legacy path is unchanged.

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
and shared-manager identity. Live tracing identifies the manager as embedded
storage at the sequence runtime plus `0xd8`; there is no independent refcount
to retain. A live session task must remain present to prevent parent reset.

The existing validated Song Select commit remains authoritative:
`func_007FCE6C(args, manager)` receives a live-vector index, native P1/P2
difficulty objects, and the mode flag. XML ordering is never used as that
index.

The stock Song Select dispatcher `func_001F5CA8` uses state 10 for that
selection commit, state 11 (`0x001F6338`) to request the shared Lumen transition,
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
must be used while a session anchor remains alive. Removing the last session
task causes the parent to clear the manager and enter attract.

## Remaining native validation

The native launch and Results adapters are implemented but require live proof
of gameplay startup, player objects, audio/Lumen handoff, and repeated returns.
Remote profile import and synthetic-player recreation are still pending.
Native default construction is used; no raw Player Entry copy or fabricated
vtable is permitted. An idle setup task owns the browser's place in the native
task tree, and only its current update facade may queue gameplay.

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

## September 6 follow-up: dispatcher audit

The prior `0x008DA730` update identification was incorrect. Its native body
recursively removes task-tree entries and accumulates retired tasks; it is
not the frame command-consumption boundary. Registering an override there
also misses direct lifted calls. This explains a Play command remaining queued
while the frontend disables navigation.

The actual scene transaction is `func_0026D530(runtime)`. It walks the task
tree through `func_008D87D4`, invokes each task's update virtual at `+0x10`,
then completes native retirement and initialization. A tracked entry injection
observes the embedded session. Commands run from the retained setup task update
inside this traversal, so parent/current-task context is valid. Native retirement
must keep running.

The `0x00f9ae70` scene-owner object is a **temporary facade** at this routine's
`sp + 0x15c`, initialized with active/pending task containers and the tree.
Never persist the owner passed to Player Entry or Results destinations across
frames. Any later launch must use a fresh native facade during its lifetime.

The runtime's embedded session is `runtime + 0xd8`, supplied to
native scene constructors in the outer lifecycle. The lifetime trace now
compares it with Player Entry's manager and reports active/pending task counts
and the PPU thread. The live run confirmed equality: runtime `0x0fefecd0`, manager `0x0fefeda8`.
Song Select's destructor does not release its plain manager field at `+0x0c`;
its refcount operations belong to other subobjects. Do not infer a manager
retain operation from those unrelated decrements.

Frontend launch latching now precedes command publication, so a failure
consumed immediately by the PPU cannot be overwritten by the producer setting
the latch afterward. Native bootstrap and Results adapters are now present; these corrections alone
do not establish gameplay, remote-profile P2, or Results-loop acceptance.


### Transition audit and preview path (September 6)

`taiko-plus-anchor-03.log` proves that the session anchor retains all 853 song
records and that Play commits `saoali` at live index 106 with native P1/P2.
The subsequent wait exposed another incorrect label: `005c583c` resolves to
`0015c4c4`, which **starts a shared Lumen transition** when its state is idle
(`-1`). It is not a pure preload predicate. Success changes its state; the
adapter must latch that acceptance and count 120 frames without calling it
again. The regression mock now switches the service to busy after acceptance.

Host previews also used lowercase catalog IDs as literal NUB filenames, while
Green stores uppercase `SONG_*.nub` banks. The decoder now normalizes validated
ASCII IDs, matching content identity lookup on case-sensitive native Linux.

### Live gameplay checkpoint (September 6)

`build-linux/taiko-plus-transition-04.log` records transition acceptance,
GameEnso construction, both player charts, gameplay Lumen assets, and native
song audio loading. The user accidentally entered service during the first
attempt, then re-entered TAIKO+, finished `mikugv` on Hard, and confirmed the
Results screen. The runtime detected native Results at `0x43d13e00`.
This validates standalone gameplay through Results; the service interruption
is not counted as a successful browser-return loop. The same run then recorded Results retirement without Song Select allocation
and a subsequent `naraku` launch from the custom browser. This confirms one
complete return cycle and a second launch in the same native session. The
20-loop soak and authenticated/remote-profile cases remain unvalidated.

All 15 native CTest targets pass. The new native-call adapter test uses active
checks in Release builds and covers missing IDs, one-shot transition
acceptance, the 120-frame wait, native player pointers, duplicate Results
callbacks, and a second launch. A real `saoali` decode with a lowercase ID
also passes after the preview path fix.
