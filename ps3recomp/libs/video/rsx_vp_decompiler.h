/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX vertex shader (NVIDIA NV40 "nvfx" vertex program, a VLIW
 * that co-issues one vector ALU op and one scalar ALU op per instruction) into
 * an HLSL vertex shader.
 *
 * Encoding reference: RPCS3 RSXVertexProgram.h (D0..D3 bitfields) + Mesa
 * nouveau nvfx_shader.h opcode tables. Words are stored little-endian (RPCS3
 * native order), 4 words / 16 bytes per instruction, no inline constants
 * (VP constants live in a separate bank), program ends at the D3.end bit.
 *
 * Self-contained: no D3D12 dependency, build/test standalone.
 */
#ifndef PS3RECOMP_RSX_VP_DECOMPILER_H
#define PS3RECOMP_RSX_VP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of instructions in the program at `ucode` (each 16 bytes), up to and
 * including the one with the D3.end bit, bounded by max_bytes. 0 if none. */
u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes);

/* Decompile an NV40 vertex program into an HLSL vertex shader.
 *   ucode    : VP bytecode (little-endian words, as in RPCS3's shader cache).
 *   max_bytes: safety bound.
 *   out      : caller buffer for generated HLSL (NUL-terminated).
 *   out_size : size of out in bytes.
 * Returns instruction count (>=0), or -1 on error (null args / overflow).
 * input_mask: bit n set = vertex input v[n] arrives in the vertex buffer, as
 *   the mask'th float4 of the packed vertex; a cleared bit reads the RSX
 *   default (0,0,0,1) as a literal. Pass 0xFFFF for the full sixteen-slot
 *   layout. Use rsx_vp_input_mask() so the producer and the pipeline agree.
 * Emits `VSOutput main(VSInput input)`: one float4 input per mask bit
 * (ATTR0..ATTRn in packed order), a
 * `cbuffer VPConst : register(b0)` with 512 vec4 constants + vp_posscale/
 * vp_posoffset (the RSX viewport transform mapped to D3D clip space; the
 * caller computes them per draw), SV_Position + COLOR0/1 + FOG + TEXCOORD0..7
 * varyings routed per the NV40 output register map (o0/o1/o2/o5/o7..o14).
 * Not modeled: condition-code tests, flow control (BRA/CAL/...), TXL. */
int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size,
                     u32 input_mask);

/* Bitmask of the vertex inputs v[0..15] the program reads. The vertex buffer
 * needs to carry only these, which is what RSX_VERTEX_LAYOUT_PACKED does; a
 * program that reads none still gets bit 0 so the vertex stride stays valid.
 * Scans the same instruction range and honours the same end bit as
 * rsx_vp_decompile, so both always agree on the layout. */
u32 rsx_vp_input_mask(const u8* ucode, u32 max_bytes);

/* Mark the statically addressed vertex constants read by a program. `used`
 * has one byte per vp_c[0..511] and is cleared by the function. Returns 0
 * when all reads are static, 1 when an address-register-indexed constant read
 * requires the complete bank, or -1 for invalid input. This deliberately
 * follows the same implemented opcodes as rsx_vp_decompile: operands encoded
 * in unused instruction fields do not force a false full-bank fallback. */
int rsx_vp_constant_usage(const u8* ucode, u32 max_bytes, u8 used[512]);

/* Mnemonics for the vector / scalar opcode fields ("?" if unknown). */
const char* rsx_vp_vec_name(u32 op);
const char* rsx_vp_sca_name(u32 op);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VP_DECOMPILER_H */
