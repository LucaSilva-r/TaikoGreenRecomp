# Green chart capacity

TaikoRecomp raises the imported chart limit from 300 to 16,384 measures. This
requires a guest allocation change as well as converter changes. The executable
and original fumen files are not patched on disk.

## Guest layout and hook

The binary-fumen lane built by `func_0040BC44` contains:

| Lane offset | Meaning |
| --- | --- |
| `+0x0004` | 300 embedded measure records, 128 bytes each |
| `+0x9604` | Number of records constructed |
| `+0x9608` | Pointer to the 88-byte course header (after timing windows) |
| `+0x960c` | Measure-pointer interface |
| `+0x9610` | Dynamic measure-pointer vector |

The course header's `+0x50` field is the chart measure count (file offset
`0x200`). `func_0040BC44` already reserves that many pointer slots. The append
function `func_0040BB54` previously calculated `lane + 4 + index * 128` for every
record, so record 300 overwrote the lane's count, header pointer and vector.
There is no need to change those member offsets or grow every stock object.

The address calculation now calls `taiko_fumen_measure_address`. Indices below
300 use their original embedded addresses. On first overflow, the helper
allocates `(measure_count - 300) * 128` bytes through the guest chart allocator
(`0040E440`, OPD `0100F048`). The original record constructor, branch parsing and
pointer-vector insertion then run unchanged. The helper rejects an out-of-range
index or a count above the shared limit before returning an overflow address.

The guest destroys individual records through its pointer list before lane
teardown. The three lane destructor variants (`0040B8A0`, `0040B948`, `0040BA0C`)
only dispose of the pointer-vector storage; hooks release the overflow region
through the matching guest allocator (`0040E480`, OPD `0100F050`). Constructor
reuse also clears any previous registered pool. A mutex protects simultaneous
player/course owners. The allocator uses the existing scoped guest-heap locks.

The lv2 `sys_memory_allocate` path is deliberately avoided here: its runtime
implementation advances a bump pointer that `sys_memory_free` does not reclaim.
Using that path per song would eventually exhaust the address range despite
apparently balanced allocation/free calls.

The five hooks are recorded in `tools/recomp_hand_edits.json` and reapplied by
`tools/apply_recomp_patches.py`. `src/taiko_chart_limits.h` is shared by the guest
helper and the TJA/osu/Nijiiro importers. Its contents participate in cache recipe
hashing. The retained Python converters use the same 16,384 limit.

## Conversion behavior

osu conversion retains all natural bars, BPM/kiai transitions and inherited
scroll-speed transitions. It no longer chooses a subset to fit 300 records.
Notes retain their absolute timestamps; their positions are relative to the
complete set of measure boundaries. Over-limit charts fail rather than losing
boundaries. The native/Python parity suite includes a 1,200-measure TJA and an
osu chart with roughly 1,500 timing/scroll transitions.

## Validation

`taiko_fumen_pool_tests` exercises two independent lanes, the 299/300 boundary,
5,000 and 16,384 records, stable record addresses, metadata preservation,
constructor reuse and balanced teardown. It substitutes the guest allocator
and memory primitives; it does not execute the game's record destructors.
`taiko_nijiiro_tests` covers binary byte order, separate duet assets, encrypted
metadata/charts with a synthetic key, cache corruption/revision handling and
native IDSP/BNSF decoding. `test_native_charts.py` checks conversion parity.

These automated checks do not establish live gameplay synchronization or prove
that every long community chart uses only features implemented by Green. Live
loading/playback validation must be reported separately from converter tests.
