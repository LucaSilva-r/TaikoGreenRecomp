#!/usr/bin/env python3
"""Build Player Entry with an independent TAIKO+ carousel item.

Green already ships the artwork/timeline for a dormant Campaign item. This
patch gives that unused timeline a unique ``SetEntryPCMode`` label, appends it
to the carousel independently of AI Battle, and clones the normal-play board.
Stock GetMode's default is normal Play. Only the existing final native
SetNextScene call carries a host-only fourth argument for the new label.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ENTRY_RECORD = 97
PLAYER_ENTRY_RECORD = 98
CONNECTION_RECORD = 65
PC_MODE_STRING = 691          # Serialized string index: "SetEntryPCMode"
CAMPAIGN_STRING = 289         # Serialized string index: "キャンペーン"
PC_MODE_SENTINEL = 99
PC_MODE_BOARD_SPRITES = {716, 731, 797}  # normal, focused, unavailable


def parse_header(data: bytes) -> tuple[list[dict], list[dict], dict]:
    if data[:12] != b"LM_NUT_TYPE1":
        raise ValueError(f"bad DDP magic: {data[:12]!r}")
    pos = 16
    skip_size = struct.unpack_from(">I", data, pos)[0]
    pos += 4 + skip_size + 0x14
    lm_count = struct.unpack_from(">I", data, pos)[0]
    pos += 13

    lm_entries = []
    for index in range(lm_count):
        name_size = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        name = data[pos:pos + name_size].decode("utf-8")
        pos += name_size
        if index == 0:
            pos += 5
        offset_field = pos
        offset, size, begin_id, end_id = struct.unpack_from(">IIII", data, pos)
        pos += 16
        lm_entries.append({
            "name": name,
            "relative_offset": offset,
            "size": size,
            "begin": begin_id,
            "end": end_id,
            "offset_field": offset_field,
            "size_field": offset_field + 4,
        })

    pos += 5
    nut_count = struct.unpack_from(">I", data, pos)[0]
    pos += 13
    nut_table = data[pos:pos + nut_count * 8]
    pos += nut_count * 8
    lm_size_field = pos
    lm_size = struct.unpack_from(">I", data, pos)[0]
    pos += 16
    lm_base = pos
    nut_base = lm_base + lm_size

    for entry in lm_entries:
        entry["offset"] = lm_base + entry["relative_offset"]

    nut_entries = []
    for index in range(nut_count):
        offset, size = struct.unpack_from(">II", nut_table, index * 8)
        nut_entries.append({"offset": nut_base + offset, "size": size})
    return lm_entries, nut_entries, {
        "lm_base": lm_base,
        "lm_size": lm_size,
        "lm_size_field": lm_size_field,
        "nut_base": nut_base,
    }


def parse_packlist(path: Path) -> list[tuple[str, list[str]]]:
    lines = [line for line in path.read_text(encoding="utf-8").splitlines()
             if line.strip()]
    sections: list[tuple[str, list[str]]] = []
    current: tuple[str, list[str]] | None = None
    for line in lines[1:]:
        if line[0].isspace():
            if current is None:
                raise ValueError("NUT appeared before the first LM")
            current[1].append(line.strip())
        else:
            current = (line.strip(), [])
            sections.append(current)
    return sections


def action_instructions(code: bytes) -> list[tuple[int, int, int]]:
    result = []
    offset = 0
    while offset < len(code):
        opcode = code[offset]
        size = 1
        if opcode >= 0x80:
            if offset + 3 > len(code):
                raise ValueError(f"truncated action at record offset {offset:#x}")
            size = 3 + struct.unpack_from("<H", code, offset + 1)[0]
            if offset + size > len(code):
                raise ValueError(f"truncated action payload at {offset:#x}")
        result.append((offset, opcode, size))
        offset += size
    return result


def insert_action_code(code: bytes, at: int, extra: bytes) -> bytes:
    """Insert AVM1 actions and repair branches and enclosing body lengths."""
    original = action_instructions(code)
    starts = {offset for offset, _, _ in original}
    if at not in starts:
        raise ValueError(f"action insertion {at:#x} is not an instruction boundary")

    delta = len(extra)
    patched = bytearray(code[:at] + extra + code[at:])

    def moved_target(position: int) -> int:
        return position if position <= at else position + delta

    def moved_instruction(position: int) -> int:
        return position if position < at else position + delta

    for offset, opcode, size in original:
        new_offset = moved_instruction(offset)
        if opcode in (0x99, 0x9D):  # Jump / If
            displacement = struct.unpack_from("<h", code, offset + 3)[0]
            target = offset + size + displacement
            if target not in starts and target != len(code):
                raise ValueError(f"branch at {offset:#x} has bad target {target:#x}")
            new_target = moved_target(target)
            struct.pack_into("<h", patched, new_offset + 3,
                             new_target - (new_offset + size))
        elif opcode == 0x8E:  # DefineFunction2
            body_data = offset + 3
            parameter_count = struct.unpack_from("<H", code, body_data + 2)[0]
            size_field = body_data + 7 + 3 * parameter_count
            body_size = struct.unpack_from("<H", code, size_field)[0]
            body = offset + size
            if body <= at < body + body_size:
                struct.pack_into("<H", patched, moved_instruction(size_field),
                                 body_size + delta)
        elif opcode == 0x9B:  # DefineFunction
            body_data = offset + 3
            parameter_count = struct.unpack_from("<H", code, body_data + 2)[0]
            size_field = body_data + 4 + 2 * parameter_count
            body_size = struct.unpack_from("<H", code, size_field)[0]
            body = offset + size
            if body <= at < body + body_size:
                struct.pack_into("<H", patched, moved_instruction(size_field),
                                 body_size + delta)
        elif opcode == 0x94:  # With
            size_field = offset + 3
            body_size = struct.unpack_from("<H", code, size_field)[0]
            body = offset + size
            if body <= at < body + body_size:
                struct.pack_into("<H", patched, moved_instruction(size_field),
                                 body_size + delta)
    return bytes(patched)


def parse_action_records(payload: bytes) -> list[bytes]:
    count = struct.unpack_from(">I", payload, 0)[0]
    records = []
    offset = 4
    for _ in range(count):
        size = struct.unpack_from(">I", payload, offset)[0]
        offset += 4
        records.append(payload[offset:offset + size])
        offset += (size + 3) & ~3
    if offset != len(payload):
        raise ValueError("action-record pool has trailing or truncated data")
    return records


def serialize_action_records(records: list[bytes]) -> bytes:
    payload = bytearray(struct.pack(">I", len(records)))
    for record in records:
        payload.extend(struct.pack(">I", len(record)))
        payload.extend(record)
        payload.extend(b"\0" * (-len(record) % 4))
    return bytes(payload)


def parse_strings(payload: bytes) -> list[str]:
    """Read F001 indices verbatim, including the valid empty string at zero.

    Length excludes the NUL terminator; padding includes it. Dropping empty
    entries shifts every symbol and makes plausible-looking disassembly wrong.
    """
    if len(payload) < 4:
        raise ValueError("truncated Lumen string pool")
    declared = struct.unpack_from(">I", payload, 0)[0]
    result = []
    offset = 4
    for _ in range(declared):
        if offset + 4 > len(payload):
            raise ValueError("truncated Lumen string length")
        size = struct.unpack_from(">I", payload, offset)[0]
        end = offset + 4 + ((size + 4) & ~3)
        if end > len(payload) or payload[offset + 4 + size] != 0:
            raise ValueError("truncated or unterminated Lumen string")
        raw = payload[offset + 4:offset + 4 + size]
        result.append(raw.decode("utf-8"))
        offset = end
    if offset != len(payload):
        raise ValueError("Lumen string pool has trailing data")
    return result


def patch_entry_actions(record: bytes, debug_y_probe: bool = False) -> bytes:
    if len(record) != 9976:
        raise ValueError("Entry action record signature mismatch")

    # Keep the destination/index and count increment from the stock Battle
    # append, but use a direct, unique string as the new item's value.
    if record[0x474:0x4B9] != bytes.fromhex(
        "9603000943011c960300099d054e9605000401099c054e"
        "9603000943011c9603000986054e9603000943011c960300098705"
        "4e4e4f960a000401099c050401099c054e504f"
    ):
        raise ValueError("stock AI Battle append block signature mismatch")
    pc_append = (
        record[0x474:0x48B]
        + bytes.fromhex("96030009b302")  # Push string[691:"SetEntryPCMode"]
        + record[0x4A8:0x4B9]
    )

    # Unknown labels already return the numeric ModeSelect.MODE_GAME. Keep
    # GetMode byte-for-byte stock; do not return LABEL_NAME_SRC[MODE_GAME],
    # which is a display string, or dispatch a native event from this getter.
    if record[0x1FB2:0x1FC1] != bytes.fromhex(
            "9603000943011c960300098c054e3e"):
        raise ValueError("Entry GetMode normal-play fallback mismatch")

    # Unlike GetMode, FinishDecide switches on DISPLAY LABELS and its default
    # returns without installing EnterFrame_Fadeout. Alias only its temporary
    # switch value to Play's label; retain the actual board label for the final
    # native completion marker. This runs after the authored confirmation.
    if record[0xD3E:0xD5F] != bytes.fromhex(
            "870100009603000943011c9603000986054e"
            "9603000943011c960300098c054e4e"):
        raise ValueError("Entry FinishDecide label switch signature mismatch")
    play_label = record[0xD42:0xD5F]
    substitute_label = b"\x17" + play_label + bytes.fromhex("87010000")
    # A private static boolean survives until CppConnection.SendResultInfo.
    # Do not depend on Proc_Mode's NotifyModeSelectEnd: the live route reaches
    # SetNextScene without invoking that native callback.
    remember_selection = bytes.fromhex(
        "960300091e021c96030009b302"  # CppConnection.SetEntryPCMode
        "960200040096030009b302664f"  # = (selected label == custom label)
    )
    finish_decide_alias = (remember_selection
                           + bytes.fromhex("960200040096030009b30266129d0200")
                           + struct.pack("<h", len(substitute_label))
                           + substitute_label)

    # A fifth board clip is cloned below. Let the stock layout and
    # navigation code initialize all five.
    if record[0x2664:0x2672] != bytes.fromhex(
            "960a00040109a40507040000004f"):
        raise ValueError("Entry BOARD_MAX signature mismatch")
    record = bytearray(record)
    struct.pack_into("<i", record, 0x2664 + 3 + 2 + 3 + 1, 5)
    record = bytes(record)

    # Clone board0 as board4 ONCE per Init, before the loop's induction-variable
    # initialization. Inserting at the loop header recreates the clip on every
    # backedge, leaving mcBoard[4] pointing to a removed instance at loop exit.
    # The authored carousel occupies
    # display-list depths 924 through 987, so put the dynamic clone at the first
    # round unused depth above that range.
    clone_body = bytes.fromhex(
        "960a00040409a6050700000000"  # Push reg(4), string[1446], int(0)
        "474e"                    # Add2, GetMember -> source board0
        "96080009a6050704000000"  # Push string[1446], int(4)
        "47"                      # Add2 -> target name "board4"
        "96050007e8030000"        # Push depth 1000
        "24"                      # CloneSprite
    )
    fifth_controller = bytes.fromhex(
        "960a00040409a6050704000000"  # Push reg(4), "board", int(4)
        "474e"                    # Add2, GetMember -> board4
    )
    remove_existing = fifth_controller + b"\x25"  # RemoveSprite
    clone_fifth_controller = remove_existing + clone_body

    first_controller = bytes.fromhex(
        "960a00040409a6050700000000"  # Push reg(4), "board", int(0)
        "474e"                    # Add2, GetMember -> board0
    )
    align_fifth_controller = (
        # board4._y = board0._y after Init's final Tween_Move().
        fifth_controller
        + bytes.fromhex("96030009bc02")  # Push string[700:"_y"]
        + first_controller
        + bytes.fromhex(
            "96030009bc02"        # Push string[700:"_y"]
            "4e"                  # GetMember -> board0._y
        )
        + (bytes.fromhex(
            "9605000764000000"    # Push integer 100
            "0b"                  # Subtract: unmistakable visual probe
        ) if debug_y_probe else b"")
        + b"\x4f"                 # SetMember -> board4._y
    )

    # Insert from the highest original offset down so every following offset
    # is still expressed against the clean action record.
    # Original 0x743 is the End immediately after Init's final Tween_Move(). Insert
    # before it while its offset is still unchanged by the earlier edits below.
    patched = insert_action_code(record, 0xD42, finish_decide_alias)
    patched = insert_action_code(patched, 0x743, align_fifth_controller)
    patched = insert_action_code(patched, 0x585, clone_fifth_controller)
    patched = insert_action_code(patched, 0x4B9, pc_append)
    # Reset on every actual carousel initialization, including card refresh.
    patched = insert_action_code(patched, 0x1FC, bytes.fromhex(
        "960300091e021c96050009b30205004f"))
    return patched


def patch_player_entry_actions(record: bytes) -> bytes:
    """Leave the stock parent dispatcher and its callbacks unchanged."""
    if len(record) != 10599:
        raise ValueError("PlayerEntry action record signature mismatch")

    # GetMode defaults to numeric normal Play, so dispatch remains unchanged.
    if record[0x6B8:0x6D1] != bytes.fromhex(
            "960a00070000000004010934034e96030009bc0552"
            "87010000"):
        raise ValueError("PlayerEntry post-decision dispatch signature mismatch")

    return record


def patch_connection_actions(record: bytes) -> bytes:
    """Tag the live-proved SetNextScene call without changing stock arguments."""
    if record[0x5B76:0x5B8B] != bytes.fromhex(
            "960a0007030000000401091f024e960300095c0352"):
        raise ValueError("CppConnection SetNextScene signature mismatch")
    original_call = record[0x5B37:0x5B8B]
    tagged_call = bytearray(original_call)
    struct.pack_into('<i', tagged_call, 0x5B76 - 0x5B37 + 4, 4)
    custom = (bytes.fromhex("9605000763000000")  # fourth arg below stock args
              + tagged_call + b"\x99\x02\x00"
              + struct.pack('<h',len(original_call)))
    condition = bytes.fromhex("960300091e021c96030009b3024e129d0200")
    return insert_action_code(record, 0x5B37,
                              condition + struct.pack('<h',len(custom)) + custom)


def patch_entry_lm(data: bytes, debug_y_probe: bool = False) -> bytes:
    if data[:4] != b"LMB\0":
        raise ValueError("entry.lm has bad magic")
    output = bytearray(data[:0x40])
    offset = 0x40
    active_sprite: int | None = None
    label_patches = 0
    actions_patched = False
    strings_checked = False

    while offset < len(data):
        tag, word_count = struct.unpack_from(">II", data, offset)
        end = offset + 8 + word_count * 4
        if end > len(data):
            raise ValueError(f"truncated LMB tag {tag:#x}")
        payload = data[offset + 8:end]

        if tag == 0xF001:
            strings = parse_strings(payload)
            if (strings[CAMPAIGN_STRING] != "キャンペーン" or
                    strings[PC_MODE_STRING] != "SetEntryPCMode"):
                raise ValueError("Entry string-pool signature mismatch")
            strings_checked = True
        elif tag == 0xF005:
            records = parse_action_records(payload)
            if len(records) <= PLAYER_ENTRY_RECORD:
                raise ValueError("Entry action records 97/98 are missing")
            records[ENTRY_RECORD] = patch_entry_actions(
                records[ENTRY_RECORD], debug_y_probe)
            records[PLAYER_ENTRY_RECORD] = patch_player_entry_actions(
                records[PLAYER_ENTRY_RECORD])
            records[CONNECTION_RECORD] = patch_connection_actions(
                records[CONNECTION_RECORD])
            payload = serialize_action_records(records)
            actions_patched = True
        elif tag == 0x0027:
            active_sprite = struct.unpack_from(">I", payload, 0)[0]
        elif tag == 0x002B and active_sprite in PC_MODE_BOARD_SPRITES:
            label, frame = struct.unpack_from(">II", payload, 0)
            # Campaign is index 289 at frame 30. Training (288/frame 20) and
            # Shop (290/frame 40) must retain their authored labels.
            if label == CAMPAIGN_STRING and frame == 30:
                patched_payload = bytearray(payload)
                struct.pack_into(">I", patched_payload, 0, PC_MODE_STRING)
                struct.pack_into(">I", patched_payload, 4, 30)
                payload = bytes(patched_payload)
                label_patches += 1

        if len(payload) % 4:
            raise ValueError(f"LMB tag {tag:#x} became unaligned")
        output.extend(struct.pack(">II", tag, len(payload) // 4))
        output.extend(payload)
        offset = end

    if not strings_checked or not actions_patched or label_patches != 3:
        raise ValueError("Entry PC Mode patch did not match the clean movie")
    return bytes(output)


def build(source: Path, packlist: Path, font: Path, output: Path,
          debug_y_probe: bool = False) -> None:
    from dxt5_encoder import create_ntp3_nut_bytes
    from generate_pc_mode_textures import make_pc_mode_images

    data = bytearray(source.read_bytes())
    lm_entries, nut_entries, layout = parse_header(data)
    sections = parse_packlist(packlist)
    if len(sections) != len(lm_entries):
        raise ValueError("packlist and DDP LM counts differ")

    entry_index = next((index for index, item in enumerate(lm_entries)
                        if Path(item["name"]).name == "entry.lm"), None)
    if entry_index is None:
        raise ValueError("entry.lm not found")

    named_nuts: dict[str, dict] = {}
    for lm, (_, names) in zip(lm_entries, sections):
        if len(names) != lm["end"] - lm["begin"]:
            raise ValueError(f"NUT count mismatch for {lm['name']}")
        for relative, name in enumerate(names):
            named_nuts[Path(name).name] = nut_entries[lm["begin"] + relative]

    for name, image in make_pc_mode_images(font).items():
        replacement = create_ntp3_nut_bytes(image)
        target = named_nuts.get(name)
        if target is None:
            raise ValueError(f"{name} not found")
        if len(replacement) != target["size"]:
            raise ValueError(f"{name} size changed: {len(replacement)} != "
                             f"{target['size']}")
        begin = target["offset"]
        data[begin:begin + target["size"]] = replacement

    lm_blobs = [bytes(data[item["offset"]:item["offset"] + item["size"]])
                for item in lm_entries]
    lm_blobs[entry_index] = patch_entry_lm(
        lm_blobs[entry_index], debug_y_probe)

    header = bytearray(data[:layout["lm_base"]])
    relative_offset = 0
    for item, blob in zip(lm_entries, lm_blobs):
        struct.pack_into(">I", header, item["offset_field"], relative_offset)
        struct.pack_into(">I", header, item["size_field"], len(blob))
        relative_offset += len(blob)
    struct.pack_into(">I", header, layout["lm_size_field"], relative_offset)

    rebuilt = bytes(header) + b"".join(lm_blobs) + bytes(data[layout["nut_base"]:])
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_bytes(rebuilt)
    temporary.replace(output)
    print(f"Wrote {output} ({len(rebuilt)} bytes); added independent TAIKO+ "
          "item using img00277/img00285 and a cloned fifth board; "
          f"AI Battle left stock; Y probe "
          f"{'enabled (-100)' if debug_y_probe else 'disabled'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="unmodified entry packeddata.ddp")
    parser.add_argument("--packlist", type=Path, required=True)
    parser.add_argument("--font", type=Path, default=Path("fonts/font.ttf"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--debug-y-probe", action="store_true",
        help="move board4._y 100 units above board0 for diagnosis")
    args = parser.parse_args()
    build(args.source, args.packlist, args.font, args.output,
          args.debug_y_probe)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
