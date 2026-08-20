# Base online: ALL.Net + MUCHA

Goal: the title reaches its arcade services and completes the online path that
gates content delivery. Custom songs, score upload, pairing, version check and
remote control are explicitly **out of scope** for this document.

## Why not port Zucchini's design

Zucchini replaces the game's entire HTTP stack and embeds mbedTLS, because it
runs *on the PS3*: the title's own SSL is pinned at TLS 1.0 and the only way to
reach a modern endpoint is to override the send/receive path in guest code.

That constraint does not exist here. `cellHttp`, `cellSsl` and `cellHttpUtil`
are HLE -- ordinary host C -- so host TLS gives 1.2/1.3 for free. We implement
the library the game already calls instead of replacing the caller.

Consequences:

- no embedded TLS in guest memory, no guest patching, no replacement stack;
- Zucchini's `allnet_proxy.c` (a loopback listener on `:18080` that catches the
  raw-socket ALL.Net POST) is unnecessary -- we own `sys_net_bnet_connect` and
  retarget the connection in place;
- the redirect is host-side and therefore configurable, which an EBOOT patch
  cannot be.

## Configuration

Everything the title talks to is sent to **one** host and port over TLS. The
settings live in `taiko_online.cfg` in the launcher's working directory, which
is the repository root (override the path with `TAIKO_ONLINE_CONFIG`); every key also has an environment override, which
wins:

```
host=127.0.0.1     # TAIKO_ONLINE_HOST   -- unset means offline, as before
port=443           # TAIKO_ONLINE_PORT
verify=0           # TAIKO_ONLINE_VERIFY -- 1 checks the server certificate
cacert=ca.pem      # TAIKO_ONLINE_CACERT -- required when verify=1
```

A private arcade server presents its own certificate, so verification is off by
default; refusing it would only mean no online at all.

With a host configured, the `TAIKO_OFFLINE_COMPLETE` spoof in
`src/taiko_usio.cpp` disables itself: forcing the "initial data unavailable"
branch would cut the online check off before the server could answer it.

`TAIKO_NET_TRACE=1` logs the first 40 socket operations (connect target,
select/poll, send/recv, whether each went through TLS).

## What is implemented

| Component | State |
|---|---|
| `cellHttp` | all 19 imported NIDs bound (`src/taiko_net.c` maps the SDK names this title uses onto `ps3recomp/libs/network/cellHttp.c`) |
| TLS | mbedTLS 3.6.4, vendored by `scripts/build_mbedtls.sh`, wrapped by `src/taiko_tls.c` |
| Redirect | connection target, SNI **and** `Host:` swapped before DNS, on both transports; method, path, body and every other header untouched |
| `sys_net` | 26 imported NIDs, 26 bound; `socketpoll`/`socketselect` honour the guest timeout |
| `cellSsl` | 3 imported NIDs (Init, CertGetNotBefore/After); the handshake is host-side, so nothing more is needed |
| `cellHttpUtil` | 1 NID (`ParseUri`) |
| `cellNetCtl` | 6 NIDs, HLE |

Endpoints, read out of the game binary via `ghidra_out/strings.json`:

```
0x00CB7510  naominet.jp                     ALL.Net host
0x00ECA108  /mucha                          MUCHA base
0x00ECA110  /mucha/arcdir
0x00ECA120  /mucha/chunk
0x00EC3670  "AllnetServerName :"            config echo
0x00EC4988  ignore_mucha_invalid_enforced=
```

## How the two transports reach the server

**cellHttp** (MUCHA, the game server) owns its own native sockets. It asks the
title layer for a target rewrite and, if one is given, hands the connected
socket to `taiko_tls_open_socket`. Method, path and body are untouched, which
is what the server routes on -- one endpoint serves every service because the
paths differ (`/sys/servlet/PowerOn`, `/mucha/...`, the game routes).

**SNI and `Host:` must both name the configured server**, and this was measured,
not assumed, against a live ALL.Net server:

- `sni=naominet.jp` -> fatal alert `-0x7780`; the server terminates TLS for its
  own name only.
- `Host: naominet.jp` -> `403 Forbidden` from the CDN in front of it, while the
  identical request with `Host: <server>` returns `stat=1`. Four hook pointers in `cellHttp.c`
(`g_http_redirect_target`, `g_http_tls_open/send/recv/close`) keep the runtime
library game-agnostic; `src/taiko_net.c` installs them only when a server is
configured, so an unconfigured build behaves exactly as it did.

**The ALL.Net PowerOn POST** does not use cellHttp: it opens a socket and
writes HTTP itself. `net_connect` in `src/taiko_net.c` retargets that
connection and wraps it in TLS, so the guest keeps writing plain HTTP into a
TLS session without knowing. Because the guest composes those headers itself,
`net_rewrite_host_header` replaces the single `Host:` line on the way out and
changes nothing else. The socket is non-blocking, so the handshake is deferred
to the first send or recv on it.

`gethostbyname` hands out a distinct `127.1.0.N` per name so the connect hook
can report which service a connection was meant for; the connection itself and
its SNI go to the configured host either way.

## Defects this shook out of the vendored cellHttp

All three were invisible until a real server answered:

- `cellHttpUtilParseUri` took its arguments as host pointers and `memset` the
  guest address -- an instant segfault the first time the title parsed the URL
  ALL.Net returns. It now works in guest memory and writes the real
  `CellHttpUri` layout (five pointers into the caller's pool, then the port).
- `cellHttpCreateClient`/`CreateTransaction` handed out slot index **0** as the
  first handle. The title tests handles against zero (`if (*transId != 0)`), so
  it read that as "no transaction", skipped the request, and then walked the
  header count it had never filled in -- an unbounded `[tty]` write and abort.
  Slot 0 is now never allocated.
- `CellHttpUri.path` was read from offset +8, which is `username`. Every
  request would have gone to `/`.

- **The status query has to read the response.** The title never calls
  `cellHttpRecvResponse` for these service requests: `FUN_0090E9F4` sends, asks
  `cellHttpResponseGetStatusCode`, then closes the connection. On real firmware
  that query blocks for the response head, so `cellHttpGetStatusCode` (and
  `GetResponseContentLength`) now read and parse it on demand. Before that,
  every service read status 0 and retried forever.
- `cellHttpTransactionCloseConnection` was aliased onto `AbortTransaction`,
  which marks the transaction aborted and would reject any later use of it. It
  now closes the connection and leaves the transaction alive.

Also added, because the arcade URIs need them: HTTP Basic credentials taken
from the URI (`https://vschassis:...@host/v01r00/chassis/startupauth.php`), and
a `cellHttpRequestGetAllHeaders` that writes its out-parameters instead of
returning CELL_OK and leaving the caller's stack untouched.

## Chassis operator flags

The dump's `data/config/S11100-1/chassisinfo.xml` is a list of `<Info>` records
keyed by a numeric dongle serial; the cabinet's own serial (`ABDN0000000`, the
constant the dongle bypass leaves in place) is `268410000000`, the first
record. The loader copies the matching record into an 18-byte flag block whose
address is `*(u32*)(TOC + 0x5F1C)`, byte 0 being `is_registered`.

`src/taiko_usio.cpp` dumps that block once per boot as `[taiko_chassis]` and
can override it: with a server configured it clears `force_offline` (a cabinet
talking to a server is not an offline cabinet), and `TAIKO_CHASSIS_FLAGS`
overrides any flag by name, e.g.
`TAIKO_CHASSIS_FLAGS=force_offline=0,ignore_network_connection=1`.

**The override lands late**, though: it runs from the USIO poll, by which time
MuchaMain has already read the flags at init. For anything the boot path reads
once, edit the XML record instead -- the file is a Boost text archive, so keep
the element order and change only the digit.

Measured on this dump: `is_registered=1`, `ignore_mucha_invalid_enforced=1`
already (so that is *not* what fails the third boot network service), and
`force_offline=1`, which is what has to be cleared for an online cabinet.

## Card login without a reader

The card reader is part of the USIO bulk stream, so it is emulated in the same
place: `src/taiko_usio.cpp` frames PN53x traffic on channel 0 register 0x7000,
and `src/taiko_card.c` owns the card itself -- the MIFARE image an access code
encodes to, plus the poll (0x4A) and read (0x40) commands that serve it.

A BanaPassport's block 1 carries the card id encrypted with a Blowfish variant
whose per-profile keys and cipher seeds are constants **inside the game image**,
so a card is only accepted when it is encoded with the tables the running build
carries. `taiko_card.c` finds them by their own `NBGIC0`..`NBGIC7` tags in guest
memory (measured: `0x00CC9640` in this build) and inverts the printed access
code back to the card id, rejecting any code no profile issued.

Two ways to get a card onto the reader:

```sh
# 1. A code you already know: swiped the first time the game polls the reader.
TAIKO_CARD_CODE=00000000000000000000 ./run-taiko-linux.sh

# 2. Six-digit pairing, the way Zucchini does it: the server hands out a code,
#    whoever types it into the web UI picks the card.
TAIKO_PAIRING_TOKEN=<token> ./run-taiko-linux.sh
#   [taiko_pairing] pairing code 163388 -- enter it on <server> within 30s
```

`pairing_token` and `cabinet_id` can also live in `taiko_online.cfg`. The
cabinet id defaults to eight hex digits derived from the host name, so it is
stable without a state file.

Pairing polls `POST /api/zucchini/pairing` (`cabinet_id`, `state`, `accepting`,
and the session/ack tokens) only while the game is actually polling the reader
and no card is on it -- the same gate Zucchini takes from the reader's own
"waiting for a card" signal. `state` is always `unknown`: the recomp has no
scene classifier, and the server documents that value for exactly that case.
Anything else, including `shop` in the wrong case, comes back `status=closed`.

## Known gaps

- The pairing code is printed to the log, not drawn on screen. The overlay
  Zucchini renders would need a text renderer in the SDL_GPU backend.
- The dongle serial is still a constant in `tools/recomp_hand_edits.json`
  (`serial=ABDN0000000` in the PowerOn body); it belongs in `taiko_online.cfg`
  with the rest.
- The cab reports `ip=127.1.0.N` in its PowerOn body -- it takes that from the
  address it resolved for `bbrouter.loc`, which is now a redirect placeholder.
  Harmless unless a server keys sessions on it.
- `_sys_net_errno_loc` publishes sysNet's single global errno, not a per-thread
  one.
- `cellHttpRequestGetAllHeaders` and `cellHttpClientCloseAllConnections` return
  CELL_OK without doing anything; nothing reads their results before the first
  request.

## Validation

- `build-linux/net_select_tests` -- guest big-endian pollfd/fd_set/timeval
  marshalling, the `CellHttpHeader` unpack, `inet_ntop`. Setting
  `TAIKO_TLS_LIVE_HOST` (and optionally `TAIKO_TLS_LIVE_PORT`) adds a real
  handshake against that server, which is the only way to prove the mbedTLS
  wrapper moves bytes.
- `scripts/test-linux-headless.sh` -- three boots to attract with online
  unset, to catch a regression in the offline path.
- A live boot with `TAIKO_ONLINE_HOST` set and `TAIKO_NET_TRACE=1`.

Measured on a live boot against a private ALL.Net server (2026-08-20):

```
[taiko_online] socket 0 connect 127.1.0.3:80 (naominet.jp) -> <server>:443 over TLS
[taiko_online] socket 0 TLS session established (sni=<server>)
[taiko_net]    send fd=0 len=340 over TLS      (POST /sys/servlet/PowerOn)
[taiko_net]    recv fd=0 297 bytes: stat=1&uri=...&host=...&place_id=...
[cellHttpUtil] ParseUri('https://vschassis:...@127.0.0.1:54430/v01r00/chassis/startupauth.php')
[cellHttp]     redirect 127.0.0.1:54430 -> <server>:443
[cellHttp]     GetStatusCode(trans=1) -> 200
[taiko_netstate] auth=00000067 online_state=00000002 ready=1
```

`online_state=2 ready=1` is the success state the title's own initial-data
callback leaves behind, and the chassis loop (startupauth, playresult,
initialdatacheck, tournamentcheck, heartbeat, bookkeeping) then runs against
the server, each returning 200.

## Reference

Zucchini's implementation, for behaviour comparison only -- do not port its
structure: `/home/silvaluca/Documents/git/Zucchini/TaikoZucchini/network/`,
chiefly `http_client.c` (the redirect at line 883), `allnet_proxy.c` and
`uri.c`.
