# Lumen layer ordering: game mechanism and PPU VMX fix

## Result

The title does not use display depth as the primary ordering mechanism for its
2D Lumen UI. It builds a CPU-side display list, attaches one floating-point
sort key to each command, stable-sorts the list in ascending key order, and
then emits the resulting RSX command stream. RPCS3 preserves that stream order.
The D3D12 backend must do the same; reversing equal-Z runs or globally sorting
nested Lumen groups changes the game's scene hierarchy.

The broken order in the recomp was caused before RSX submission. Most sort
keys were NaNs because `lvewx` was lifted with the wrong AltiVec element
semantics. The game's stable merge sort compares `left <= right`; a NaN makes
that false in both operand orders, so the merge repeatedly selects the right
half. This looked like a consistent reversal on simple screens but became
unreliable as soon as a scene contained multiple nested movies.

## Oracle observations

- `func_0050C0C8` appends `{command pointer, float sort key}`.
- `func_005096B8` forwards the key in `f1`.
- `func_0050BAD4` is the stable ascending merge sort; equality selects the
  left item.
- A broken recomp Song Select queue had 367 NaNs among 398 entries.
- After the VMX fix, the same queue had zero NaNs and clean finite runs.
- RPCS3 and the recomp then agreed on the finite key groups and their order;
  differences in the number of commands per flush are batching, not a reason
  to reorder commands in the backend.

The decisive scalar comes from the sequence around `func_00515354`:

```text
lvsl -> lvewx -> vperm -> vspltw -> vcfux -> vrefp
```

For the sampled count `4`, RPCS3 produces `4.0` and then reciprocal `0.25`.
The old lift zeroed the destination vector and always copied the loaded word to
byte zero. With a non-zero effective-address slot, `vperm` therefore selected a
zero word and `vrefp` produced infinity; later bounds/layer arithmetic produced
NaNs.

## Correct element semantics

`lvebx`, `lvehx`, and `lvewx` select the destination element from the low bits
of the effective address and preserve all other vector bytes. The memory
address is naturally aligned to the element width. The corresponding `stve*`
instruction selects the matching source element and stores it to that aligned
address.

The source fix is in `ps3recomp/tools/ppu_lifter.py`, with emission regression
coverage in `ps3recomp/tools/tests/test_ppu_lifter_vmx.py`. The current generated
`src/recomp/` snapshot was mechanically updated too, because it is gitignored;
a future re-lift must be made with the fixed lifter.

Normal launches intentionally leave both `RTT_UNREVERSE` and
`RTT_SORT_LUMEN_GROUPS` unset. They remain backend diagnostics only.

## Song Select: distinct remaining geometry issue

The corrected Song Select capture has stable, correctly ordered 2D header,
song card, category columns, arrows, instructions, and lower backgrounds. The
missing Don-chan/3D content is not evidence of another ordering failure.

RPCS3's frame capture for the 600x600 pass at RT `0x01B89000` records the same
index stream and RSX array semantics, but also captures a populated main-memory
position array. Representative first draw:

```text
indices: 448, 367, 369, 366, 368, 379, 380, 390
a0: main 0x00E38640, float3, stride 32, captured bytes 42028
a8: local 0x0718C994, short2, stride 8
```

The recomp's corresponding draw uses a valid main-memory offset and the same
indices, but the selected position slots contain allocator fill `0xCD`; its UV
array is populated. That places the remaining fault upstream of D3D12 vertex
address decoding: the guest-side position producer did not fill the slots that
the index buffer references (or their lifetime/synchronization is wrong).
Do not compensate by changing draw order, using depth, remapping the vertex
address, or treating the command buffer as vertex data.

`tools/parse_rpcs3_rrc.py` prints raw array/base/index state and captured memory
blocks from RPCS3 version-6 `.rrc.gz` files for continuing that investigation.
