# Player Entry reversing

The goal is to replace the lifted Player Entry and Song Select controllers with
readable title-specific implementations while retaining their guest-memory ABI.
Lumen is treated as a view consumer, not as the authoritative player state.

## Current trace points

The first verified controller boundary is now instrumented directly:

- `func_001ED698`: Player Entry state transition.  Its first argument is the
  controller object, `object + 0x10` is the previous state, `object + 0x14` is
  the current state, and `r4` is the requested state.  With
  `TAIKO_ENTRY_TRACE=1`, each invocation prints an `[entry-state]` line with
  the old/new numeric values, recovered names, caller `lr`, and object address.

The call is inserted at the start of the generated lifted function because
most callers invoke it directly and therefore bypass `ppu_register_function`.
It is an intentionally temporary investigation edit in
`src/recomp/ppu_recomp_000.cpp`; re-lifting will remove it.  The logger itself
lives in `src/taiko_entry_trace.cpp`.

`func_007F698C` is the state-enum-to-name switch used by the title's own
transition log.  Its recovered sparse mapping establishes these important
values:

| Value | State |
| ---: | --- |
| 0--11 | `None`, `Init`, `Load`, `State_ReadySound`, `State_Intermission`, `Start`, `EntryMain`, `CardSelect`, `Mobile`, and the three `OnSendURL_*` states |
| 15--18 | `BAID_WaitServer`, `BAID_Canceled`, `BAID_Failed`, `BAID_Succeed` |
| 20--24 | `BAID_WaitNewCostume`, both `MydonName_*` states, `UserData_WaitServer`, `State_UserData_Failed` |
| 30--35 | `State_UserData_End`, both `WaitUserResponce_*` states, `OnTouchCard`, `WaitEntrybase`, `WaitEndInterval` |
| 39--40 | `End`, `Term` |

In particular, `OnTouchCard` is state `33` (`0x21`), not a function name.
The earlier `0x00225CB8` probe was unrelated rendering/data code and has been
removed.  The contiguous references near `0x005BED78` are also unrelated C++
runtime type-registration code that happens to consume the same GOT region.

The first live validation (`taiko-entry-state-01.log`) reached one controller
object at `0x4350a440` and produced:

```text
None -> Init -> Load -> State_ReadySound -> Start -> EntryMain
```

The requesting return addresses were respectively `0x001eee20`,
`0x001efd64`, `0x001f22e8`, `0x001f08b8`, and `0x001efd64`.  The probe did not
change the controller's behavior and gives us stable call sites to decompile
for each transition.

One P1 centre hit while in `EntryMain` was also observed end-to-end.  The USIO
edge was followed by `EntryMain -> EntryMain` from return address `0x001ed8c8`.
That address is the tail of Player Entry event 12, which clears the card-side
subcontroller flag through `func_00236758(controller + 0xfec, 0)` and requests
state 6 again.  This confirms that the event dispatcher described below is on
the live input path, rather than merely being dead or alternate-version code.

## Player Entry controller dispatch

`func_001EF214` is the main Player Entry state dispatcher.  It reads the state
number at `controller + 0x14` and dispatches states 0 through 39 through the
jump table at `0x001ef2b8`; state 40 and out-of-range values take the teardown
path.  The controller owns or embeds several cooperating objects at offsets
`+0xf08`, `+0xf60`, `+0xfec`, `+0x1048`, and `+0x1074`.

The important state-handler addresses recovered so far are:

| State | Handler | Notes |
| ---: | ---: | --- |
| 6--8 | `0x001f0458` | Shared `EntryMain`, `CardSelect`, and `Mobile` handler |
| 33 | `0x001eff14` | `OnTouchCard` |
| 34 | `0x001ef710` | `WaitEntrybase`; currently a default/no-op handler |
| 35 | `0x001effe8` | `WaitEndInterval` |
| 39 | `0x001ef724` | `End` |

`OnTouchCard` is deliberately transient.  At `0x001eff28` it immediately asks
the state setter for state 34 (`WaitEntrybase`), then calls
`func_0022ce08(controller + 0xf08)` and evaluates the player/card flags needed
for the following account flow.

`func_001ED74C(controller, event)` is the corresponding Player Entry event
dispatcher.  It accepts event IDs 0 through 12.  The most useful recovered
events are:

| Event | Dispatcher case | Effect |
| ---: | ---: | --- |
| 2 | `0x001ed920` | Sets `controller + 0xfec` active and requests state 33, `OnTouchCard` |
| 8 | `0x001ed840` | Selects a failed-response path, normally state 32 |
| 9 | `0x001ed870` | Requests state 32, `WaitUserResponce_Failed` |
| 10 | `0x001ed888` | Requests state 7, `CardSelect` |
| 11 | `0x001ed898` | Requests state 8, `Mobile` |
| 12 | `0x001ed8a8` | Clears the card-side flag and requests state 6, `EntryMain` |

Events 0, 1, and 3--7 have identified case addresses. Event 1 advances the
embedded entry/game-mode subcontroller at `controller + 0xf60` to state 5 by
calling `func_0022909C(..., -1)`; event 0 advances the same object to state 6
through `func_00229080(..., -1)`. Their cases begin at `0x001ed8cc`,
`0x001ed8fc`, `0x001ed7c4`, `0x001ed944`, `0x001ed980`, `0x001ed990`, and
`0x001ed9a0`, respectively. The complete jump-table mapping is relative to
`0x001ed790`; the table pointer is stored at GOT address `0x01030e6c`.

The higher-level producer of card-touch event 2 is `func_00229908`.  Its
signature is presently reconstructed as approximately:

```c
void bana_entry_result(
    BanaEntryOwner *owner,
    uint8_t player_or_slot,
    const BanaCardRecord *record,
    uint32_t result_state);
```

On result states 0, 9, and 13--15, its common success branch at `0x0022a394`
copies the incoming card data into the owner's per-player entry record.  The
copy includes a 32-bit leading field, 36 bytes beginning at input `+0x04`, 24
bytes beginning at input `+0x28`, a 16-bit field at input `+0x40`, and an
8-bit field at input `+0x42`.  It clears the three status bytes at owner
offsets `+0x30..+0x32`, loads the Player Entry controller from owner `+0x34`,
and calls `func_001ED74C(controller, 2)` at `0x0022a5b0`.

This is the first concrete replacement seam.  A native player/lobby frontend
does not need to render or author a Lumen timeline to enter the existing game
flow: it can construct the same per-player record and deliver the same event 2
contract.  Exact field meanings and the downstream account-result events must
still be named before this becomes a supported API.

## Verified no-card/offline join

`build-linux/taiko-entry-offline-record-01.log` records a complete native-Linux
P1 session without presenting a card. The record delta probe compares both
`0x4f0`-byte records at
`controller + 0x38 + player * 0x4f0` whenever an entry event or state transition
occurs.

The first P1 centre hit commits the title's own anonymous/offline record before
event 1 is delivered. Starting from the initialized default record, exactly two
bytes changed:

```text
P1 +0x000: 0 -> 1    active/joined
P1 +0x42b: 0 -> 2    joined-session field (exact meaning unresolved)
```

This is not a synthetic guest profile and does not involve the decoded BAID
record-copy path. The value at `+0x42b` must not be called an offline flag: the
authenticated host commit validated later also leaves it at 2. Event 1 then
asks the embedded subcontroller at `+0xf60` to enter state 5. Event 12 closes
the card-side presentation and requests `EntryMain` again, after which the
stock game-mode choice is active.

Confirming the highlighted performance-game choice eventually produces the
common completion sequence:

```text
EntryMain -> WaitEndInterval -> UserData_WaitServer -> state 27
          -> State_UserData_End -> End -> Term -> Song Select
```

The decisive `EntryMain -> WaitEndInterval` request is the call at
`0x001f0584` (reported return address `0x001f0588`). Immediately before it the
title resets `controller + 0xf60` with `func_002292F8` and writes a 60-frame
interval to `controller + 0x18`. The offline path enters the nominal userdata
states but makes no `userdata.php` or `crownsdata.php` HTTP request. Visual
validation confirmed that it reaches stock Song Select with P1 installed and
P2 empty.

The event callback `func_00225050` is not itself the record writer. It parses
the Lumen callback value, calls the empty compatibility stub
`func_0035C940`, then emits event 0 or event 1. Therefore the remaining
anonymous reverse target is the stock update operation that commits the two
record bytes before this presentation callback. The supported host adapter
must reproduce that operation and the completion contract, not fabricate or
copy a `GuestPlayerProfile` for anonymous play.

### Host-driven anonymous transaction and exit

The native host frontend now reproduces the complete anonymous transaction
without driving Player Entry Lumen. It runs from the beginning of
`func_001EF214`, the verified main-PPU Player Entry dispatcher, and is disabled
unless `TAIKO_HOST_FRONTEND=1` is set.

The recovered callback contracts are:

```text
func_00226A9C(callback_frame)
  raw integer arguments: { player=0, entry_kind=0 }
  result: P1 +0x000=1, P1 +0x42b=2

func_002287BC(callback_frame)
  raw integer arguments: { game_mode=1, option1=0, option2=0 }
  result: constructs one 12-byte element in the vector at P1 +0x4c0
```

The vector object uses begin, end, and capacity pointers at record offsets
`+0x4c4`, `+0x4c8`, and `+0x4cc`. A successful one-player callback therefore
has `end == begin + 12` and `capacity >= end`. It is not a group of three
session pointers.

Anonymous exit also requires the stock operation omitted by the first direct
state experiment: `func_002332C4(controller + 0x1048)` starts the embedded
userdata controller. Although a no-card player makes no userdata HTTP request,
state 23 still waits for this controller to report completion. The common
stock tail is then reproduced exactly: write 60 to `controller + 0x18`, call
`func_002292F8(controller + 0xf60)`, and request state 35 through
`func_001ED698`.

`taiko-host-frontend-anon-07.log` validates the resulting native path:

```text
EntryMain (6)
  -> WaitEndInterval (35)
  -> UserData_WaitServer (23)
  -> state 27
  -> State_UserData_End (30)
  -> End (39)
  -> Term (40)
```

The host Song Select shell switches on state 39. That is the final observable
safe dispatcher tick: the state-39 handler requests state 40 and destroys the
Player Entry controller before another `func_001EF214` call can observe 40.
The state trace confirms the state-40 transition occurs in the same update.

Three negative experiments define why the complete sequence matters:

- emitting the presentation event alone does not commit the game-mode vector;
- entering state 35 without starting the userdata controller parks forever in
  state 23;
- jumping directly to states 39/40 skips required teardown and returned the
  title to attract.

The host frontend therefore uses semantic native transactions and the stock
completion path, not a Lumen macro or an arbitrary final-state write.

### Host-driven authenticated transaction

`build-linux/taiko-entry-online-writer-01.log` captures the stock online-player
selection with the guest-byte writer and indirect callback-frame probes. The
selection invokes the same `func_00226A9C` callback and the same raw arguments
as no-card play:

```text
arg1={type=3,value=0}  arg2={type=3,value=0}
```

The profile source is therefore native manager context, not an argument. With
no BAID context the callback activates Green's initialized offline record. In
`CardSelect` after `BAID_Succeed`, it copied 151 changed bytes of the decoded
account/profile data into P1 and changed the active byte from 0 to 1. An
explicit negative test with argument 2 set to 1 returned no selectable profile
and left P1 inactive.

`build-linux/taiko-host-frontend-baid-05.log` validated the state-machine tail,
but later visual validation exposed an important false positive in its player
commit. The virtual card reached the reader, `baidcheck.php` returned success,
and costume `032000` loaded, but the host invoked only `func_00226A9C`. That is
the Card Select confirmation operation; by itself it activates the initialized
no-card record.

The byte-writer stack in the stock authenticated capture identifies the
missing preceding transaction as `func_00225CB8`. It takes the destination
player index, copies the decoded BAID staging record from slot 2 into that
player record, and performs the associated Card Select manager updates. Stock
Card Select therefore performs `{ func_00225CB8(P1), func_00226A9C(P1, 0) }`.
The host now invokes that pair after one native Card Select update and requires
the assignment to change at least 16 player-record bytes, preventing the
two-byte no-card fallback from being reported as authenticated. The remaining
captured tail is:

`build-linux/taiko-auth-profile-copy-01.log` live-validates the corrected pair.
The profile-assignment callback changed 162 bytes, the confirmation left P1
active with 160 bytes changed relative to the original record, and the next
state snapshot observed 172 total changes including the account identity and
profile fields. The normal `userdata.php` and `crownsdata.php` requests then
completed, and Song Select reopened `cos_032000`, proving that the costume
selection propagated beyond Player Entry.

```text
CardSelect (7)
  -> WaitEndInterval (35)
  -> UserData_WaitServer (23)
  -> state 27
  -> State_UserData_End (30)
  -> End (39)
  -> Term (40)
```

After state 40, the title opened `SE_SELECT.nub`, `VO_SELECT.nub`,
`JINGLE_GENRE.nub`, and `/data/lumendata/packed/song_select/packeddata.ddp`.
This proves the original Song Select scene loads normally underneath the opaque
host Song Select shell.

The pairing worker originally replaced host Login mode 3 with its legacy
pairing-pill mode whenever it refreshed a background code; expiry then made the
host screen disappear while the frontend still owned input. Pairing updates
now leave all opaque host modes alone except BAID mode 4, where the refreshed
code is intentionally rendered in the host screen.

The older object-snapshot hooks below remain exploratory candidates rather
than verified Player Entry entry points:

`TAIKO_ENTRY_TRACE=1` wraps these indirect guest calls and records their object
state before and after the original lifted implementation runs:

- `func_000A8A3C`: Game Entry reset/setup candidate
- `func_000AD13C`: main Game Entry lifecycle candidate
- `func_000D2208`: Song Select state machine
- `func_000FA0C0`: Song Select scene-entry builder

The trace snapshots `0xA0` bytes for Game Entry and `0x100` bytes for Song
Select. The first observation prints the complete snapshot; later observations
print only changed 32-bit words. `TAIKO_ENTRY_TRACE_CALLS=1` additionally prints
unchanged calls and their incoming `r3`-`r7` values.

Use the existing input trace to align drum events with guest mutations:

```sh
TAIKO_ENTRY_TRACE=1 TAIKO_INPUT_TRACE=1 ./run-taiko.sh
```

Card presentation/removal is already logged by `taiko_card.c`. Avoid enabling
`TAIKO_CARD_TRACE` for a normal reversing run: it includes the reader's
high-frequency idle polling and is intentionally verbose.

## Controlled captures

Record separate runs for:

1. Player Entry with no input.
2. P1 joining with a drum hit.
3. P2 joining with a drum hit.
4. A BanaPassport being presented and accepted.
5. The transition from Player Entry to Song Select.
6. Song and difficulty confirmation followed by the Enso transition.

The first reconstruction target is the narrow player-data commit operation,
not the Lumen population call. Once its inputs and persistent writes are known,
a native host UI can drive that contract and leave the original gameplay
initialization intact.

## Verified card-reader boundary

The virtual reader path has now been followed from the USIO bulk response into
the title's BngRw worker.  A presentation that ultimately shows the offline
scan-failure UI still completes the card-side exchange:

1. `cellUsbdBulkTransfer` returns an active-target `0x4B` response.
2. Three `0x41` data-exchange responses complete target authentication, UID
   retrieval, and the encrypted NBGIC block read.
3. `bngrw_parse_nbgic_ident_response` (`0x00427910`) matches the `NBGIC0`--
   `NBGIC7` profile, decrypts the eight-byte identity payload, verifies its XOR
   check byte, and produces the compact parsed-card record.
4. `func_00420488` commits that record to the BngRw request object at offsets
   `+0x128`, `+0x12c`, and `+0x12e`.
5. `func_00421930` invokes the request completion callback with the result and
   the request's parsed-result area at `request + 0xa8`.

The observed failure is after this boundary.  During the capture the title's
network state was already `auth=0xffffff3a`, `online_state=0`, `ready=0` before
the card was presented.  Therefore the failure does not imply that the virtual
USIO data was dropped; it is the later account/service consumer rejecting or
being unable to resolve the parsed card while the game is offline.

The transport stack below the parser is also identified:

- `0x004188f4`: asynchronous `usbio::UsbConnection` bulk transfer wrapper
- `0x004193ac`: USB completion shim which stores result/count and signals the
  waiting guest object
- `0x0041724c`: `usj::SerialPort` 16-byte response-header reader
- `0x004173b0`: `usj::SerialPort` response-payload reader

These functions provide framing and synchronization only.  They should not be
used as the Player Entry interception point.  The useful seams are the parsed
BngRw completion record and the higher-level `card::BanaPassport` state change
which consumes it.

## Live card-to-controller validation

The first instrumented card presentation in `taiko-entry-state-01.log`
validated the complete boundary.  The reader returned one active-target
response (`0x4b`) followed by the authentication, UID, and encrypted `NBGIC7`
data-exchange responses (`0x41`).  Immediately afterward the controller at
`0x4350a440` followed this path:

```text
EntryMain
  -> OnTouchCard
  -> WaitEntrybase
  -> WaitUserResponce_Failed
  -> WaitEntrybase
  -> EntryMain
```

The setter return addresses were `0x001ed940`, `0x001eff28`, `0x001ed884`,
`0x001eff10`, and `0x001ed8c8`.  The first two prove that successful card
decoding delivered event 2 and exercised the exact `OnTouchCard` seam recovered
statically.  The final reset is event 12.

The transition to `WaitUserResponce_Failed` cannot yet be assigned to event 8
or event 9 from the setter trace alone.  Both cases share the setter call at
`0x001ed880` and therefore report return address `0x001ed884`; event 8 falls
through into event 9's common state-setting tail.  A second temporary probe at
the entry to `func_001ED74C` now prints `[entry-event]` with the incoming event
ID and the original producer return address.  The next controlled scan will
disambiguate that downstream failure source.

The first native-Linux scan (`build-linux/taiko-entry-event-01.log`) exercised
the authenticated path against the configured chassis service.  The network
state was ready (`auth=0x67`, `online_state=2`, `service=2`, `ready=1`), the
title posted a 90-byte request to `/v11r01/chassis/baidcheck.php`, and received
HTTP 200 with a 570-byte response.  The resulting controller path was:

```text
EntryMain
  -> OnTouchCard
  -> WaitEntrybase
  -> BAID_WaitServer
  -> state 19 (the title has no enum-name string for this value)
  -> BAID_WaitNewCostume
  -> BAID_Succeed
  -> WaitUserResponce_Succeed
  -> WaitEntrybase
  -> CardSelect
```

The event trace resolves the important producers precisely:

- Event 2 comes from `0x0022a5b4`, the success branch in
  `func_00229908` after it copies the decoded card record.
- `WaitEntrybase -> BAID_WaitServer` is an internal state request from
  `0x001f3638`, after the entry-base/card subcontroller becomes ready.
- Event 7 comes from `0x001ef378` after costume loading completes; its case
  requests `WaitUserResponce_Succeed`.
- Event 10 comes from `0x00225b04` and requests `CardSelect`.

This validates that the original authenticated player-entry contract can be
driven without replacing its Lumen view.  At minimum, a native frontend must
provide the decoded per-player card/profile record, deliver event 2, and either
allow the original BAID/user-data sequence to run or synthesize the same final
player record and successful events.

## Card Select and the native player record

The authenticated native-Linux run was continued through the visible Card
Select choice.  P1 left-rim followed by P1 centre selected the online player.
The observed tail was:

```text
CardSelect
  P1 left rim
  P1 centre
  event 12 from 0x00224e8c
  -> EntryMain
  event 0 from 0x0022511c
```

Event 12 is only the common Card Select close operation.  Its producer is the
ActionScript/Lumen callback at `0x00224df8`, which parses a truthy value, makes
the Card Select controller inactive with `func_0022DB2C(..., 0)`, and delivers
event 12.  The false/default callback path additionally calls the UI wrapper
at `0x005C533C(..., 1)` before performing the same close.  That extra call is a
presentation operation, not a player-record commit.

The adjacent callback at `0x00224c54` parses two numeric values and calls
`func_0022C708(card_select_controller, player_index, status)`.  The callee
packages those two integers and invokes a method on the Card Select Lumen
object.  It is a native-to-Lumen presentation update rather than the selection
commit.  The same helper is used during Card Select initialization with status
0 or 4 according to the existing per-player record.

The real per-player record accessor is now exact:

```c
PlayerEntryRecord *get_player_record(PlayerEntryController *controller,
                                     int player)
{
    return (PlayerEntryRecord *)((uint8_t *)controller + 0x38 + player * 0x4f0);
}
```

In the live post-selection snapshot, controller `0x42f71980` therefore held P1
at `0x42f719b8` and P2 at `0x42f71ea8`.  P1 byte `+0x00` was 1 while P2 byte
`+0x00` was 0, establishing this byte as the active/joined flag.  The profile
data copied by the authenticated card flow was present in P1, including the
UTF-8 player name and costume-related fields; P2 retained the default offline
record.  P1 byte `+0x01` was 0 in this online-player capture, so it must not be
named an online/authenticated flag merely from the Card Select initialization
test.

The callbacks at `0x00224620` and `0x00224864` write only the transient input
payload at player-record offsets `+0x6c..+0x78`.  The four-value form clears
`+0x6c` and stores three values at `+0x70`, `+0x74`, and `+0x78`; the two-value
form clears those three fields and stores its value at `+0x6c`.  The live P1
record retained `+0x6c = 32` after the centre hit.  These fields are input/UI
messages, not persistent account selection state.

The post-close event 0 is also presentation-state bookkeeping.  Its dispatcher
case runs only in `EntryMain` and calls `func_00229080(controller + 0xf60, -1)`,
which writes subcontroller state 6.  Event 1 similarly selects subcontroller
state 5.  Neither copies or changes the authenticated player record.

The second native-Linux capture (`taiko-entry-event-02.log`) resolves the exit
gate.  Immediately after Card Select closed, the embedded object at controller
`+0x1178` still had state word `+0x117c == 0`.  One P1 centre hit on the returned
Entry Main screen produced event 1 from `0x002250c4`.  The Lumen callback
repeated that event four times while its confirmation animation advanced.  The
event-1 dispatcher case calls `func_0022909C(controller + 0xf60, -1)`, selecting
subcontroller state 5.  Once the embedded completion object became ready, the
common states-6--8 handler at `0x001f0458` performed the final UI shutdown and
requested state 35 at `0x001f0584`.

The complete verified exit sequence is:

```text
EntryMain
  P1 centre
  event 1 from 0x002250c4
  -> WaitEndInterval                    (setter LR 0x001f0588)
  POST /v11r01/chassis/userdata.php     (29-byte body, HTTP 200/824 bytes)
  POST /v11r01/chassis/crownsdata.php   (29-byte body, HTTP 200/68 bytes)
  -> UserData_WaitServer                (setter LR 0x001f354c)
  -> state 27                           (setter LR 0x001f2e78)
  -> State_UserData_End
  -> End
  -> Term
  -> Song Select load
```

This leaves no unknown Player Entry transition between an authenticated native
player record and Song Select.  A host-controlled replacement can either call
the recovered native operations in this order and retain the title's account
requests, or populate the equivalent final records and enter the post-Entry
scene directly.  The Lumen callbacks are presentation and command sources;
they are not the authoritative storage for the player/profile state.
