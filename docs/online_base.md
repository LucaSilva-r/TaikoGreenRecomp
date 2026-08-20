# Base online: ALL.Net + MUCHA

Goal: the title reaches its arcade services and completes the online path that
gates content delivery. Custom songs, score upload, pairing, version check and
remote control are explicitly **out of scope** for this document.

## Why not port Zucchini's design

Zucchini replaces the game's entire HTTP stack and embeds mbedTLS, because it
runs *on the PS3*: the title's own SSL is pinned at TLS 1.0 and the only way to
reach a modern endpoint is to override the send/receive path in guest code.

That constraint does not exist here. `cellHttp`, `cellSsl` and `cellHttpUtil`
are HLE — ordinary host C — so host TLS gives 1.2/1.3 for free. We implement
the library the game already calls instead of replacing the caller.

Consequences:

- no embedded TLS in guest memory, no guest patching, no replacement stack;
- Zucchini's `allnet_proxy.c` (a loopback listener on `:18080` that catches the
  raw-socket ALL.Net POST) is unnecessary — we own `sys_net_bnet_connect` and
  can retarget the connection in place;
- the redirect is host-side and therefore configurable, which an EBOOT patch
  cannot be.

## Current state

Measured, not assumed:

| Component | State |
|---|---|
| `cellHttp` | **0 of 19** imported NIDs implemented; `src/gen/cellHttp_stubs.c` prints `UNIMPLEMENTED` |
| `cellSsl` | 138 lines, error-code shells, **no TLS** |
| `cellHttps` | 128 lines; the game imports **0** NIDs from it — HTTPS runs through `cellHttp` + `cellSsl` |
| `cellHttpUtil` | 1 NID imported (`ParseUri`) |
| `sys_net` | 26 NIDs; BSD sockets present. `select`/`poll` return instantly instead of blocking for the guest timeout |
| `cellNetCtl` | 6 NIDs, HLE |
| `src/taiko_net.c` | DNS loopback only (`TAIKO_DNS_LOOPBACK`) |

Endpoints, read out of the game binary via `ghidra_out/strings.json`:

```
0x00CB7510  naominet.jp                     ALL.Net host
0x00ECA108  /mucha                          MUCHA base
0x00ECA110  /mucha/arcdir
0x00ECA120  /mucha/chunk
0x00EC3670  "AllnetServerName :"            config echo
0x00EC4988  ignore_mucha_invalid_enforced=
```

This matches Zucchini's redirect model: **the hostname and port are swapped for
every service and the path distinguishes them** (`http_client.c:883` — method,
path, body and caller headers are left untouched, and the swap happens before
DNS, SNI and the `Host:` header so the rewritten name goes end to end).

## Stages

Each stage ends in something runnable; do not start the next until the current
one is validated.

### 1. `select`/`poll` honour the guest timeout

`sys_net` currently returns instantly, so any polling loop above it spins.
Prerequisite for everything below. Upstream ps3recomp has this fix; port it.

*Validated by:* the existing headless gate, no behaviour change expected yet.

### 2. TLS transport under `cellSsl`

Pick the library (see Open decisions), wire connect/handshake/read/write/close,
and expose it to `cellHttp`. Certificate validation must be *configurable* —
a private server will not present a chain the title would accept.

*Validated by:* a standalone host test that completes a handshake against the
configured server, in `tests/`, following `fair_mutex_tests.cpp` — no game
required.

### 3. `cellHttp` over that transport

Implement the 19 imported NIDs, driven by what the title actually calls rather
than by the full API. Expect roughly: client/transaction create+destroy, URI
set, method, header add/get, send request, read response, status code,
content length, timeouts, abort. `cellHttpUtilParseUri` backs onto the same URI
parser.

*Validated by:* the title's own log — `UNIMPLEMENTED` lines for cellHttp
disappear and requests reach the server.

### 4. Redirect + configuration

Host-side rule, matching Zucchini exactly: swap host and port, leave method,
path, body and headers alone, apply before DNS/SNI/`Host:`. Configuration for
enable, host, port — plus the dongle serial, which currently lives as a
constant in `tools/recomp_hand_edits.json` and wants the same config file.

### 5. ALL.Net raw-socket path

The PowerOn POST bypasses `cellHttp` and speaks raw sockets to `naominet.jp:80`.
Retarget it in `sys_net_bnet_connect` (`src/taiko_net.c`) rather than running a
loopback proxy. Note the game may expect plain HTTP here while the backend is
HTTPS — that upgrade happens in our connect hook.

*Validated by:* a live boot reaching the online-enabled state with the service
threads progressing past their current stubbed answers (`[taiko_netstate]`).

## Open decisions

- **TLS library.** Recommendation: **mbedTLS** — it is what the server already
  speaks, vendors cleanly, and gives one code path for Linux and MinGW. A
  handshake bug that reproduces on only one platform is expensive to chase.
  Alternatives: OpenSSL (already on the Linux host, but another cross-build to
  maintain like FFmpeg), or Schannel on Windows plus OpenSSL on Linux (no
  vendored dependency, two code paths through the most delicate layer).
- **Config format.** A file is needed for server host/port and the dongle
  serial. `config.toml` is build-time metadata, so this should be a separate
  runtime file next to the executable.

## Reference

Zucchini's implementation, for behaviour comparison only — do not port its
structure: `/home/silvaluca/Documents/git/Zucchini/TaikoZucchini/network/`,
chiefly `http_client.c` (the redirect at line 883), `allnet_proxy.c` and
`uri.c`.
