#!/usr/bin/env python3
"""Re-apply hand fixes to the generated PPU code in src/recomp/.

src/recomp/ is gitignored (millions of generated lines), so hand edits there are
lost whenever the lifter is re-run. This script re-applies them idempotently.
Run it after regenerating, before building.

    python3 tools/apply_recomp_patches.py

Only *functional fixes* belong here. Debug probes are deliberately not
preserved -- re-add them ad hoc when investigating.
"""

import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
TARGET = REPO / "src" / "recomp" / "ppu_recomp_002.cpp"
ARCHIVE_TARGET = REPO / "src" / "recomp" / "ppu_recomp_004.cpp"

# The guest's XML tokenizer object is process-global and several PPU loader
# threads parse fumen composition.xml concurrently during the boot scan.
#
# Lifted fragments tail-return continuations via g_trampoline_fn, driven by
# DRAIN_TRAMPOLINE at the call site. A std::lock_guard scoped to the C++
# function body therefore releases the moment the fragment returns a
# continuation -- while tokenizing is still in flight -- and the remainder of
# the chain runs unlocked. Another loader thread then mutates the tokenizer
# mid-token and the parser aborts with "[xml-fatal] token=5".
#
# The guard must span call + DRAIN_TRAMPOLINE at every call site. An earlier
# session did this for func_005ABE50; these are the five func_005AB5BC
# (tokenizer) call sites that were missed.
TOKENIZER_CALL_SITES = [
    "0x005ABC00",
    "0x005ABF60",
    "0x005AC470",
    "0x005AC5DC",
    "0x005AC810",
]

GUARD_COMMENT = """\
            /* Tokenizer fragment tail-returns continuations, so its own
             * lock_guard drops while tokenizing is still in flight and the
             * trampoline chain runs unlocked. Another PPU loader thread then
             * mutates the process-global tokenizer mid-token and the parser
             * aborts ([xml-fatal] token=5). Hold the lock across the chain. */
"""


def patch_tokenizer_locks(text):
    applied = skipped = 0
    for lr in TOKENIZER_CALL_SITES:
        # Anchor on the newline: an already-wrapped site is indented deeper, and
        # the 8-space form is a substring of the 12-space one, so an unanchored
        # match happily wraps the call a second time.
        bare = "\n        ctx->lr = %s; func_005AB5BC(ctx); DRAIN_TRAMPOLINE(ctx);" % lr
        if bare not in text:
            # Already wrapped (or the lifter emitted something different).
            skipped += 1
            continue
        wrapped = (
            "\n        {\n"
            + GUARD_COMMENT
            + "            std::lock_guard<std::recursive_mutex> xml_token_guard(\n"
            "                g_taiko_xml_tokenizer_mutex);\n"
            "            ctx->lr = %s; func_005AB5BC(ctx); DRAIN_TRAMPOLINE(ctx);\n"
            "        }" % lr
        )
        text = text.replace(bare, wrapped, 1)
        applied += 1
    return text, applied, skipped


ARCHIVE_SIGNATURE_MARKER = "[boost-archive] invalid signature"


def patch_archive_signature_diagnostic(text):
    """Make Boost XML archive failures identify the bytes it actually saw.

    The guest throws archive_exception(code=3) at 0x009B8FAC when the archive
    signature differs from boost::archive::BOOST_ARCHIVE_SIGNATURE().  The
    exception later reaches the guest unwinder and used to surface only as the
    misleading ``[xml-fatal] token=5`` message.  Logging here preserves the
    first useful state without adding any normal-path release-build chatter.
    """
    if ARCHIVE_SIGNATURE_MARKER in text:
        return text, False

    anchor = "loc_009B8F44:\n        ctx->gpr[31] = ctx->gpr[1] + (int64_t)(0x94);"
    if anchor not in text:
        raise RuntimeError("Boost archive signature throw site not found")

    diagnostic = r'''loc_009B8F44:
        {
            const uint32_t actual_len = vm_read32(ctx->gpr[1] + 0x8C);
            const uint32_t actual_cap = vm_read32(ctx->gpr[1] + 0x90);
            const uint32_t actual_addr = actual_cap < 0x10
                ? (uint32_t)(ctx->gpr[1] + 0x7C)
                : vm_read32(ctx->gpr[1] + 0x7C);
            const uint32_t expected_addr = (uint32_t)ctx->gpr[31];
            const uint32_t expected_len = (uint32_t)ctx->gpr[28];
            fprintf(stderr,
                    "[boost-archive] invalid signature actual_len=%u "
                    "actual_addr=%08X expected_len=%u expected_addr=%08X "
                    "actual='",
                    actual_len, actual_addr, expected_len, expected_addr);
            for (uint32_t i = 0; i < actual_len && i < 64; ++i) {
                const unsigned char ch = vm_read8(actual_addr + i);
                fputc(ch >= 0x20 && ch < 0x7F ? ch : '.', stderr);
            }
            fprintf(stderr, "' actual_hex=");
            for (uint32_t i = 0; i < actual_len && i < 64; ++i)
                fprintf(stderr, "%02X", vm_read8(actual_addr + i));
            fprintf(stderr, " expected='");
            for (uint32_t i = 0; i < expected_len && i < 64; ++i) {
                const unsigned char ch = vm_read8(expected_addr + i);
                fputc(ch >= 0x20 && ch < 0x7F ? ch : '.', stderr);
            }
            fprintf(stderr, "'\n");
        }
        ctx->gpr[31] = ctx->gpr[1] + (int64_t)(0x94);'''
    return text.replace(anchor, diagnostic, 1), True


def main():
    if not TARGET.exists():
        sys.exit("missing %s -- generate src/recomp/ first" % TARGET)

    text = original = TARGET.read_text()

    if "g_taiko_xml_tokenizer_mutex" not in text:
        sys.exit(
            "g_taiko_xml_tokenizer_mutex not declared in %s.\n"
            "The mutex and the func_005AB5BC / func_005ABE50 body guards are "
            "themselves hand edits from an earlier session and are not "
            "reproduced here; restore them before running this script." % TARGET.name
        )

    text, applied, skipped = patch_tokenizer_locks(text)

    if text != original:
        TARGET.write_text(text)
    print("tokenizer call sites: %d wrapped, %d already-wrapped/not-found"
          % (applied, skipped))

    if not ARCHIVE_TARGET.exists():
        sys.exit("missing %s -- generate src/recomp/ first" % ARCHIVE_TARGET)
    archive_text = archive_original = ARCHIVE_TARGET.read_text()
    archive_text, archive_applied = patch_archive_signature_diagnostic(archive_text)
    if archive_text != archive_original:
        ARCHIVE_TARGET.write_text(archive_text)
    print("archive signature diagnostic: %s"
          % ("applied" if archive_applied else "already present"))


if __name__ == "__main__":
    main()
