/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX fragment shader (NVIDIA NV40 "nvfx" fragment program
 * encoding) into an HLSL pixel shader source string suitable for D3DCompile.
 *
 * See docs/RSX_FRAGMENT_PROGRAM.md for the instruction encoding reference
 * (sourced from Mesa/nouveau nvfx_shader.h). This module is self-contained:
 * it has no D3D12 dependency and can be built/tested standalone.
 */

#ifndef PS3RECOMP_RSX_FP_DECOMPILER_H
#define PS3RECOMP_RSX_FP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read one fragment-program word from guest (PS3) memory: big-endian load
 * followed by a 16-bit half-word swap, yielding a host-order instruction
 * word. The decoder operates on host-order words. */
u32 rsx_fp_read_word(const u8* p);

/* Decompile an NV40 fragment program into an HLSL pixel shader.
 *
 *   ucode     : pointer to the fragment-program bytecode in guest memory
 *               (big-endian, half-word-swapped — i.e. exactly as the RSX
 *               reads it; this function applies rsx_fp_read_word internally).
 *   max_bytes : safety bound on how far to read (program stops at PROGRAM_END
 *               or when this many bytes are consumed).
 *   out       : caller buffer for the generated HLSL (NUL-terminated).
 *   out_size  : size of `out` in bytes.
 *
 * Returns the number of instructions decoded (>=0) on success, or -1 on
 * error (null args, output overflow). The emitted entry point is
 * `float4 main(PSInput input) : SV_TARGET`, matching the backend's
 * placeholder PSInput { float4 position : SV_POSITION; float4 col : COLOR; }.
 *
 * Texture sampling and full input plumbing are partial — see the doc's
 * "Integration TODO". Unhandled opcodes emit a comment and a safe default so
 * the shader still compiles. */
/* exports32: colour outputs come from r0/r2/r3/r4 (SET_SHADER_CONTROL bit
 * 0x40) instead of h0/h4/h6/h8 (half-precision programs). */
int rsx_fp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size,
                     int exports32);

/* SDL_GPU keeps one compiled shader while the guest updates NV40 inline
 * constants. This variant emits those slots as rsx_fp_constants[] in a
 * second fragment uniform buffer instead of baking their values into HLSL. */
#define RSX_FP_MAX_INLINE_CONSTANTS 128u
/* color_targets (1..4) trims the emitted SV_Target outputs to what the pass
 * actually has attached; the discarded ones otherwise keep registers live. */
int rsx_fp_decompile_dynamic(const u8* ucode, u32 max_bytes,
                             char* out, u32 out_size, int exports32,
                             unsigned color_targets);

/* Cache identity excluding inline CONST payload values, plus extraction of
 * those values in instruction order for the dynamic uniform buffer. */
u64 rsx_fp_program_structure_hash(const u8* ucode, u32 max_bytes);
u32 rsx_fp_extract_constants(const u8* ucode, u32 max_bytes,
                             float* output, u32 max_constants);

/* Return the mnemonic for an NV40 fragment opcode (or "?" if unknown).
 * Useful for disassembly/logging. */
const char* rsx_fp_opcode_name(u32 opcode);

/* Total byte length of the fragment program at `ucode` (including inline
 * CONST slots), up to and including the PROGRAM_END instruction, bounded by
 * `max_bytes`. Returns 0 if no terminator is found within the bound. Shared
 * walk logic so callers (e.g. a shader cache hashing the bytecode) agree with
 * the decompiler on where a program ends. */
u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_FP_DECOMPILER_H */
