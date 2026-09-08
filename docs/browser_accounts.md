# Browser account integration (in progress)

The browser left column has only a Song Select heading, with P1 and P2 cards
stacked directly below it, P1 above P2. Each card reserves space internally for
per-player settings and native costume rendering. Account names are copied from the native session map when PC mode starts;
the UI never retains guest pointers. Joining and account identity remain separate.
The existing difficulty selection stays per player. Keys 1 and 2 toggle P1/P2
participation, preserving their accounts and clearing readiness after a lineup
change. Both players can leave, and either can rejoin. Press B to start browser login, pair using the displayed PIN, then choose P1
or P2 with 1/2 or that player's centre hit. Login can replace the selected
slot's account; duplicate assignment to both slots is rejected. Explicit logout
to guest remains unimplemented.

`BrowserAccounts` holds a staged transaction generation: waiting for a card,
choosing a destination, loading, then publication. Failure and cancellation retain
the prior account. Tests cover late completion after cancellation, late failure
after success, invalid slots, and incomplete authentication. Generation checks
protect host state; they do not cancel native network requests or make it safe to
free native callback storage.

## Native integration findings

Entry methods use TOC `0x01037a88`; network requests and profile constructors/
assignment use `0x01027c58`.

- Entry records are `0x4f0` bytes at controller `+0x38`, `+0x528`, and staging
  `+0xa18`. Their embedded profile begins at `+0x20` (size `0x4d0`).
- Session manager player map at `+0x370` contains `0x7a8`-byte entries:
  slot at `+0`, profile at `+8`, round at `+0x4d8`. Authentication byte is
  entry `+0x395`; BAID is entry `+0x40`. Do not replace a profile while the
  previous round is still waiting to be serialized into the score queue.
- `00225CB8` and `00226A9C` are Entry UI callbacks, not standalone login APIs.
  They dereference Entry singletons after accessing the selected record, so
  invoking them after Entry destruction is unsafe.
- User-data controller lives at Entry `+0x1048`. Its target record is `+8`,
  state `+0x0c`, success byte `+1`, protocol result `+0x1c`, and service owner
  `+0x28`. `00233820` binds it; `00233804` unbinds it. Response callbacks use
  the controller stored at `vm_read32(0x01033f08) + 8`.
- `000A06CC` initializes card fields from a native reader record;
  `000A1138` sends BAID lookup. `00233254` arms its receiving controller.
  `0023468C` handles the decoded BAID response.
- `002332FC` advances the native player-data/crowns sequence. Requests include
  `000A0CA4` (existing-user data), `000A0814`, and `000A0998`. Responses advance its state; calling
  the update method alone does not complete a request.
- `001EDF48` applies loaded crown data to session song vectors. It must run
  only at successful commit, since it mutates the shared session.
- The inline constructor at `007063B4` initializes each Entry record's
  profile with `00626E30`. It initializes the receiving controller to zero,
  sets `+0x14` to `0xb4` (a limit, not the state), and stores its manager
  argument at controller `+0x28`. `002331C4` writes the actual state at `+0x0c`.
- `000A163C` assigns Entry records and delegates profile assignment to
  `006285B8`. This provides an assignment operation for an already constructed
  profile; `0062AB7C` is the copy-construction path.
- `000A0B1C` takes a three-word request wrapper, not merely a record pointer:
  wrapper `+0` is the callback record, and `+4` / `+8` supply request fields.
  State 5 of `002332FC` obtains these through a manager service lookup and
  `005C60AC`. Reconstruct that wrapper before issuing userdata directly.
- `0062AB7C` copies a native profile. Profiles and Entry records own dynamic
  strings/vectors: copying raw bytes is not an ownership transfer.

Next: establish constructors/destructors for a standalone receiving controller
and staging records, map the reader card fields, and retain callback storage until
native requests are drained. Then connect pairing, player assignment, and final
profile/crown commit on the PPU thread. Do not expose a successful login UI until
all native load stages complete. A cancelled request must not be allowed to write
into storage reused by the following login.

## Live login observation

Trace `build-linux/online-trace/1788886678189769163.jsonl` captured normal
Entry login followed by PC-mode handoff. BAID lookup completed successfully
(the BAID handler entry breakpoint was bypassed; the following request observes
result 1 and authentication set). `000A0CA4` sends `userdata.php` for this
existing account. `00235A7C` finishes with state 7, success 1, protocol result 1.
`000A0998` then sends `crownsdata.php`; `002357AC` finishes with state 8,
success 1, result 1, and a populated 1024-element crown vector. The optional
state-6 extra-data request was skipped. Request dispatch and both observed
response handlers ran on GDB thread 33, while HTTP creation ran on thread 20.
A read-only post-handoff inspection confirmed slot 0 remained authenticated in
the session map. Browser participation is independent of this authentication.

`tools/trace_browser_login_gdb.py` adds login metadata to the existing online
trace, including response entry/finish snapshots. Missing entry breakpoints
still cannot establish that a handler did not run (optimized native callers may
inline it). The first attached process exited normally before the user's first
login attempt; tracing was reattached to the replacement process for this run.

The expanded BAID parser is `0023468C` (its response layout matches the BAID
schema, including player type and costumes). The earlier `00233EBC` identification
was incorrect: that parser belongs to Mydon entry/registration. The focused tracer
now covers both. Existing-card browser login must use `000A1138` and wait for the
expanded BAID callback to finish before issuing `000A0CA4`.

Browser costume handoff: the live trace showed selected and saved parts
`[32, 0, 0, 0, 0]` in the session map at both `007FCE6C` and the GameEnso
constructor `001E1C04`, but no `cos_032000` asset open. Stock Song Select also
calls `007F9A9C` with a pointer to the character service returned by `005C573C`,
the player slot, and its course record. That routine applies profile colors,
converts costume IDs to model IDs, and requests the shared character loader.
PC-mode launch now performs this step for every participating player, retrying
only unaccepted requests before starting the transition. Native GameEnso state
1 retains its asset-completion wait. Build and browser/PC-mode tests pass.

P1's costume was subsequently confirmed in gameplay. P2-only play requires
the additional stock `001F5F6C -> 001F7538` mapping: apply P2's course/profile
to character slot 0. The account slot and participation mask remain P2. The
launch test checks this separately from the two-player character mapping.

Two-player launch exposed another native return-value distinction: `0029CC34`
returns zero for an unchanged, already-loaded costume. `0029D474` also returns
zero when its global/per-character availability gates are closed. Launch now
waits on those gates explicitly and accepts the unchanged-model result once
available. The regression test combines an unchanged P1 model with a new P2
load and verifies that the transition proceeds after the busy gate clears.
The user confirmed P1 costumes, P2-only costumes, and two-player song launch
working with the final fixes.
