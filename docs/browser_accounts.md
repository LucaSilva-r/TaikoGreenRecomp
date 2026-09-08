# Browser account integration (in progress)

The browser left column has only a Song Select heading, with P1 and P2 cards
stacked directly below it, P1 above P2. Each card reserves space internally for
per-player settings and native costume rendering. Account names are copied from the native session map when PC mode starts;
the UI never retains guest pointers. Joining and account identity remain separate.
The existing difficulty selection stays per player. Browser card scanning,
logout, and replacement are not connected yet.

`BrowserAccounts` holds a staged transaction generation: waiting for a card,
choosing a destination, loading, then publication. Failure and cancellation retain
the prior account. Tests cover late completion after cancellation, late failure
after success, invalid slots, and incomplete authentication. Generation checks
protect host state; they do not cancel native network requests or make it safe to
free native callback storage.

## Native integration findings

Use Entry TOC `0x01037a88`, not session/profile TOC `0x01027c58`.

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
  state `+0x14`, success byte `+1`, protocol result `+0x1c`, and service owner
  `+0x28`. `00233820` binds it; `00233804` unbinds it. Response callbacks use
  the controller stored at `vm_read32(0x01033f08) + 8`.
- `000A06CC` initializes card fields from a native reader record;
  `000A1138` sends BAID lookup. `00233254` arms its receiving controller.
  `00233EBC` handles the decoded BAID response.
- `002332FC` advances the native player-data/crowns sequence. Requests include
  `000A0B1C`, `000A0814`, and `000A0998`. Responses advance its state; calling
  the update method alone does not complete a request.
- `001EDF48` applies loaded crown data to session song vectors. It must run
  only at successful commit, since it mutates the shared session.
- `0062AB7C` copies a native profile. Profiles and Entry records own dynamic
  strings/vectors: copying raw bytes is not an ownership transfer.

Next: establish constructors/destructors for a standalone receiving controller
and staging records, map the reader card fields, and retain callback storage until
native requests are drained. Then connect pairing, player assignment, and final
profile/crown commit on the PPU thread. Do not expose a successful login UI until
all native load stages complete. A cancelled request must not be allowed to write
into storage reused by the following login.
