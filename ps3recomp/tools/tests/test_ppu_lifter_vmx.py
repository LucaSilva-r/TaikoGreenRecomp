#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from ppu_disasm import Instruction
from ppu_lifter import LiftedFunction, PPULifter


class VectorElementLoadStoreTests(unittest.TestCase):
    def setUp(self):
        self.lifter = PPULifter()
        self.function = LiftedFunction()

    def translate(self, mnemonic, operands):
        insn = Instruction(mnemonic=mnemonic, operands=operands)
        return self.lifter._translate(insn, self.function)

    def test_lvewx_uses_ea_selected_slot_and_preserves_other_bytes(self):
        code = self.translate("lvewx", "v3, r0, r9")
        self.assertIn("slot = (uint32_t)ea & (16u - 4u)", code)
        self.assertIn("mem = (uint32_t)ea & ~(4u - 1u)", code)
        self.assertIn("(uint8_t*)&ctx->vr[3] + slot", code)
        self.assertNotIn("memset", code)

    def test_element_width_controls_slot_and_memory_alignment(self):
        for mnemonic, size in (("lvebx", 1), ("lvehx", 2), ("lvewx", 4)):
            with self.subTest(mnemonic=mnemonic):
                code = self.translate(mnemonic, "v7, r4, r5")
                self.assertIn(f"(16u - {size}u)", code)
                self.assertIn(f"~({size}u - 1u)", code)

    def test_stvewx_uses_the_matching_source_slot(self):
        code = self.translate("stvewx", "v6, r0, r8")
        self.assertIn("slot = (uint32_t)ea & (16u - 4u)", code)
        self.assertIn("vm_base + mem", code)
        self.assertIn("(uint8_t*)&ctx->vr[6] + slot", code)

    def test_vmrg_element_size_comes_from_the_last_character(self):
        """vmrghw is a WORD merge.

        The size suffix is the last character; the 'h'/'l' in position 4 is the
        high/low selector.  Matching 'h' anywhere in mn[4:] made vmrghw emit a
        halfword merge, which keeps only the high halfword of each word.  That
        silently corrupted the guest's 4x4 matrix inverse (func_0051F8EC) and,
        through it, every skinned bone matrix.
        """
        widths = {
            "vmrghb": "uint8_t", "vmrglb": "uint8_t",
            "vmrghh": "uint16_t", "vmrglh": "uint16_t",
            "vmrghw": "uint32_t", "vmrglw": "uint32_t",
        }
        for mnemonic, ty in widths.items():
            with self.subTest(mnemonic=mnemonic):
                code = self.translate(mnemonic, "v19, v16, v19")
                self.assertIn(f"{ty} tmp[", code)
                self.assertIn(f"{ty}* a=", code)

    def test_vmrg_high_low_selects_the_correct_half(self):
        # High forms take elements from the start of each register, low forms
        # from the middle.  vmrghw/vmrglw both cover 4 word lanes.
        self.assertIn("a[0+i]", self.translate("vmrghw", "v1, v2, v3"))
        self.assertIn("a[2+i]", self.translate("vmrglw", "v1, v2, v3"))
        self.assertIn("a[0+i]", self.translate("vmrghh", "v1, v2, v3"))
        self.assertIn("a[4+i]", self.translate("vmrglh", "v1, v2, v3"))


if __name__ == "__main__":
    unittest.main()
