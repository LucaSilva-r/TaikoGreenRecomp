# Native score persistence trace — September 8, 2026

## Final validation

The user confirmed that consecutive PC-mode songs now save correctly online
after the RTC fix. Results returns directly to the browser, authenticated
identity survives subsequent launches, and the native serializer, persistent
queue, HTTP worker and acknowledgement path handle submission. The final
native build passed all 23 tests. Live testing used four audio lookahead
blocks. Browser login/logout and arcade session rewards remain deferred.
Final trace `1788876295781318416.jsonl` contains two one-stage requests:
song 802 scored 531870 and song 413 scored 498600. Their identity tuples
are distinct, envelope/inner identity fields match, and both responses are 1.

## Live findings

Arcade run `build-linux/taiko-online-arcade-trace.log` and private JSONL
`build-linux/online-trace-arcade/1788865698677149109.jsonl` captured a complete
authenticated two-song session. The user confirmed the scores appeared online.
Saving belongs to the session-end sequence after the last song's Results,
not the per-song Results presentation itself.

| Event | Seconds from result finalization |
| --- | ---: |
| Finalize unlocks (`0024FBA8`), two retained stages | 0.000 |
| Reward sequence starts (`000E37A0`) | 0.013 |
| Calculate/apply bonuses, collect unlocks | 0.014–0.017 |
| Build result (`0012EE34`) | 0.018 |
| Enqueue serialized result (`002B2AD0`), persist queue | 0.019–0.020 |
| POST `/v11r01/chassis/playresult.php`, 300 bytes | 1.233 |
| Native response callback (`00915D4C`), protobuf result=1 | 1.483 |
| Persist updated queue | 1.483 |
| Game Over clears session (`001F41D0`) | 20.590 |

The single request contained song 875/course 4, score 517920 (432 good,
201 OK, 17 miss), and song 285/course 5, score 598380 (760 good, 208 OK,
32 miss). Both envelope and inner identity fields matched. Session rewards
included daily/weekly/monthly flags, 550 Don medals, and 100 Katsu medals.

PC run `build-linux/taiko-online-pc-trace.log` and
`build-linux/online-trace-pc/1788866201833632734.jsonl` captured a failed
`bbkkbk` round and a successful return to the browser. No `baidcheck.php`,
`userdata.php`, `crownsdata.php`, or `playresult.php` request was observed.
That first round was anonymous; it does **not** establish an authenticated
profile overwrite.

The subsequent authenticated round in the same process resolves that concern:
the user returned to attract, presented BanaPassport, selected PC mode, and
confirmed the logged-in user remained visible during gameplay. Trace events
15–20 capture `baidcheck.php`, `userdata.php`, and `crownsdata.php`. The game
then committed `llang` (index 147, P1 difficulty 3, generation 5), entered
native Results at `0x44041e00`, and retired Results back to the host browser.
There was no `playresult.php` request or recorded reward/result-builder/queue
sequence. The user confirmed no online save. This validates authenticated
PC gameplay and isolates the missing save boundary: the browser return skips
the native session-finalization sequence. The earlier suspicion that PC mode
always replaces authenticated players with anonymous ones is not supported.

A read-only GDB snapshot at the browser found manager `0x0fefeda8`, one
player-map entry (slot zero), one retained stage at entry+`0x668`, played=0,
limit=1. The failed result therefore survives the return. The process was
resumed after this snapshot. No guest functions were invoked by the tracer.

## Native path and PC integration boundaries

`001EBFD0` advances Results and chooses `001EBEB0` (another song) or
`001E31E4` (session finished). The latter finalizes unlocks through
`0024FBA8` and constructs the reward sequencer (vtable `0x00f8b0a8`).
Its update `000E3588` calls virtual +0x40, `000E37A0`: bonus functions
`0019482C`, `00194C50`, `00194DA0`, then result builder `0012EE34`.
It can also construct the native reward-shop scene through `00714434`.

Builder `0012EE34` walks the manager's 0x7a8-byte player entries, builds
the Green result messages, and calls thunk `005C43FC` -> `002B2AD0` to
enqueue. Queue storage is at `*(u32*)0x010399d0`; capacity is 32 records,
each 0x7fc bytes including its length. `002B3178` services it at a
180-tick cadence, using the original HTTP worker. `00915D4C` parses the
response and removes a record only for protocol result 1; failure paths
rotate the record for retry. `009131E4` persists queue changes. The cache
name is `playresultinfo.bin`, not a music catalog.

Existing Ghidra names such as `MusicMgrAppendSongRuntime` and
`MusicMgrOnlinePollTick` on these addresses are misleading historical
annotations. Follow the endpoint, schema, and live call sequence instead.
This ELF has multiple TOCs. `DecompAt.java` now accepts `ADDRESS:TOC`;
wrong default r2 values previously produced unrelated data and broken
switch decompilation. Use `-readOnly` with a scratch project copy.

Both PC Results destinations are intercepted before their original bodies.
`taiko_pc_mode_results_return` retires Results and returns to the browser.
`prepare_match` resets played to zero and limit to one. It looks up existing
player records and default-constructs missing slots; that code alone is not
proof that it replaces an existing authenticated record.

Do not invoke the whole Game Over path to save in PC mode: `001F41D0`
destroys the retained player records and resets manager state.

## PC score adapter

### First live PC upload: callback stack collision

The first adapter run (`online-trace-pc-save/1788868602905083596.jsonl`)
queued a valid 251-byte one-stage result for song 872, score 685310,
137 good/23 OK/0 miss, combo 160, with zero session bonuses. The screen
then remained dark at Results and no HTTP upload occurred before restart.

GDB found the builder spinning after enqueue with a corrupted player
iterator. A hardware watchpoint on its local at guest `0xcffdff90` caught
thread 29 writing it through `00535D60` -> `0052FA2C` -> `ppu_guest_call`
-> `ppu_gcm_pump`, while the result builder was on thread 13. Both callback
entry helpers used the same literal stack top `0xcffe0000`; making the
variable thread-local had not made its guest address unique.

Both helpers now use `PpuCallbackStack`: callbacks entered from guest code
use space below that thread's current guest frame, with 512 bytes reserved
for linkage/red-zone separation. Nested callbacks likewise use the current
outer frame. Calls without an active guest stack serialize the legacy
fallback area. `ppu_callback_stack_tests` covers concurrent guest callers,
nested frames, and exclusive fallback ownership. The native build and all
22 tests pass. On restart, trace
`online-trace-pc-stackfix/1788869249326192115.jsonl` recorded the native
queue sending the persisted 251-byte request, byte-identical to the captured
in-memory record. The server replied with protocol result 1 after 296 ms.
This validates persistence and retry across restart; website confirmation and
the post-fix Results-to-browser transition still needed live validation at
that point.

The next run, `online-trace-pc-stackfix/1788869358957837343.jsonl`, used
`TAIKO_AUDIO_LOOKAHEAD_BLOCKS=4` after the user reported corrupted audio
with their one-block setting (the cause of that audio issue is not established).
The user confirmed Results returned to the browser. The native builder queued
one stage for song 872: score 651000, 129 good/31 OK/0 miss, combo 160.
Its 255-byte request was sent 472 ms after builder entry; protocol result 1
arrived at 769 ms, followed by queue persistence and the host's queue-drained
marker. Identity fields matched and all session bonuses were zero.
Consecutive completed rounds in this same session and website persistence
confirmation remain to be checked.

`src/taiko_pc_mode.cpp` now calls the native builder at the intercepted
Results-to-browser boundary. It selects authenticated players who participated
in the current launch and have exactly one completed stage. Two narrow hooks
in `0012EE34`, maintained in `tools/recomp_hand_edits.json`, filter the native
player loop and report each enqueue result. Outside this host-initiated call,
the hooks leave the arcade builder unchanged.

The native builder generates a fresh timestamp and gives the existing queue
an owning serialized copy. The host tracks accepted players so retries do not
enqueue them twice. A full queue or enqueue failure retains the round and
blocks another launch until all selected scores are queued. Delivery then
continues through the native HTTP worker and its persisted retry queue while
the player browses or plays. The browser footer shows queue/save status;
"Scores saved" means the native delivery queue drained after protocol
acknowledgement, subject to the server caveats below.

Before each new match, the adapter destroys (`00621784`) and reconstructs
(`0062A318`) only the 0x2d0-byte round object at entry+`0x4d8`, leaving the
0x4d0-byte authenticated profile at entry+8 intact. This clears old stages,
round rewards, and owned round vectors, including for slots not joining the
next match. It does not construct the long reward or Game Over scenes.

This first pass submits native stage results without running the arcade
session bonus/unlock calculation. Reward policy for unlimited one-song PC
play and browser login/logout remain separate work. The server still treats
each submitted round as a credit. Native round reconstruction, packet contents,
and consecutive authenticated saves require live validation.

## Server contract inspected locally

### Consecutive-round failure: missing RTC import

The user reported that subsequent songs were absent online. Trace packets
24, 31 and 38 in `1788869358957837343.jsonl` contain distinct one-stage
results (651000, 302320, 435270) but identical identity tuples, including
the malformed datetime `373411021043707`. Each received result 1. This
matches the local server's duplicate-session acknowledgement path, rather
than proving persistence. The prior assumption that invoking the builder
necessarily generated a fresh timestamp was incorrect.

`001BCBF0` obtains a current RTC tick, adds 32400 timezone seconds, then
calls `cellRtcTickAddTicks` (NID `269A1882`) to copy/apply the native offset
into its output. That import was absent from the HLE registry and had no
implementation, leaving the output stale. `cellRtc.c/.h` now implements
raw tick addition using guest-endian memory, and the generated registration
unit includes it. The name-based generator discovers the new definition.
`cell_rtc_tests` verifies zero/negative additions, aliasing, invalid pointers,
and Green's conversion chain producing a valid changing date. All 23 tests
pass. Relaunched with tracing under `online-trace-pc-rtcfix`; consecutive
live uploads and website persistence need revalidation. Previously
acknowledged/discarded packets remain in the private trace but are no longer
in the game's retry queue.

Read-only sources in sibling `TaikOnline`:

- `protobuf/green/taiko.proto`: outer `PlayResultRequest` has identity fields
  and compressed `playresult_data`; inner `PlayResultDataRequest` has repeated
  stages, judgements, options, rewards, unlocks, and profile-related fields.
- `app/GameProtocol/Handlers/GameHandler.php::playResult`: decodes the inner
  payload and returns a protobuf response.
- `app/GameProtocol/Support/ProtocolPayloads.php`: supports gzip at offset
  zero or after a 32-byte prefix, with raw protobuf fallback.
- `app/GameProtocol/Services/PlayResultService.php`: transactionally saves
  stage history, bests, rewards and progression. Deduplication hashes BAID,
  game version, cabinet ID and play datetime. Reusing a session timestamp for
  new per-song submissions can cause later submissions to be acknowledged
  without being stored. Successful persistence also increments credit count.

These are local server-source findings, not verification of deployed source
revision. HTTP 200/result=1 alone is insufficient: this implementation also
returns 1 for an unknown player or an already-seen session. The arcade run
additionally has the user's confirmation that scores appeared online.

## Trace tools and remaining checks

`tools/trace_online_gdb.py` uses native Linux x86-64 GDB breakpoints and
captures lifecycle metadata and only playresult request/response payloads.
Packets are private files because they can contain player/session identifiers.
`tools/decode_playresult.py PACKET` summarizes stages/rewards without printing
IDs or tokens; `--response` decodes the response. Decoding was verified against
the live 300-byte request and two-byte response above.

Launch from the repo root with the launcher's DXC library path configured:

```sh
TAIKO_ONLINE_TRACE_DIR=build-linux/online-trace-pc \
  gdb build-linux/taiko_boot
```

Inside GDB:

```text
set pagination off
set print thread-events off
handle SIGSEGV nostop noprint pass
handle SIGPIPE nostop noprint pass
source tools/trace_online_gdb.py
run game/EBOOT.elf > build-linux/taiko-online-pc-trace.log 2>&1
```

Use `TAIKO_HOST_FRONTEND=0 TAIKO_PLUS_STANDALONE=0` for arcade comparison,
and both set to 1 for the PC host frontend. The runtime's selected PC mode
still matters; verify the native selection/Results markers in the log.
GDB stops perturb timing. Optimized/inlined calls can bypass entry breakpoints:
the arcade finalization was captured even though the Results destination
entry breakpoints did not fire. Missing entry events alone do not prove a
function was never executed.

The PC-mode regression test covers two-player selection, duplicate Results
callbacks, profile preservation during stage reset, P2-only submission with
a retained P1 profile, queue-full launch blocking, enqueue failure/retry,
partial two-player enqueue without duplication, and queue-drain status.
These mock native calls; they do not prove the guest ABI
or deployed service behavior. Remaining live checks: consecutive-round stage
reset and timestamps, successful/failed songs, P2, and fresh-login persistence.
