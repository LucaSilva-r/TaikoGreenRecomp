#!/usr/bin/env python3
"""Build Player Entry with an independent TAIKO+ carousel item.

Green already ships the artwork/timeline for a dormant Campaign item. This
patch gives that unused timeline a unique ``SetEntryPCMode`` label, appends it
to the carousel independently of AI Battle, clones the fourth authored board
as a fifth carousel controller, and makes ``GetMode`` return a host-only
sentinel for it. AI Battle and its card-login predicate stay stock.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from dxt5_encoder import create_ntp3_nut_bytes
from generate_pc_mode_textures import make_pc_mode_images


ENTRY_RECORD = 97
PC_MODE_STRING = 690          # Existing compact string: "SetEntryPCMode"
CAMPAIGN_STRING = 288         # Existing compact string: "キャンペーン"
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


def compact_strings(payload: bytes) -> list[str]:
    declared = struct.unpack_from(">I", payload, 0)[0]
    serialized = []
    offset = 12
    while offset < len(payload):
        size = struct.unpack_from(">I", payload, offset)[0]
        raw = payload[offset + 4:offset + 4 + size]
        serialized.append(raw.rstrip(b"\0").decode("utf-8"))
        offset += 4 + ((size + 3) & ~3)
    result = [value for value in serialized if value]
    result.append("")
    if len(result) != declared:
        raise ValueError("compact Lumen string pool count mismatch")
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
        + bytes.fromhex("96030009b202")  # Push string[690:"SetEntryPCMode"]
        + record[0x4A8:0x4B9]
    )

    # GetMode has just stored the selected board label in register 0. Return
    # host sentinel 99 for our unique label; all stock labels continue through
    # the original comparisons unchanged.
    if record[0x1E3A:0x1E44] != bytes.fromhex("87010000960300094301"):
        raise ValueError("Entry GetMode signature mismatch")
    get_mode_match = (bytes.fromhex(
        "9602000400"          # Push reg(0)
        "96030009b202"        # Push string[690:"SetEntryPCMode"]
        "66"                  # StrictEquals
        "12"                  # Not
        "9d02000900"          # If not equal, continue original GetMode
        "96050007"            # Push integer
    ) + struct.pack("<i", PC_MODE_SENTINEL) + b"\x3e")  # Return

    # A fifth Tween_Move clip is cloned below. Let the stock layout and
    # navigation code initialize all five.
    if record[0x2664:0x2672] != bytes.fromhex(
            "960a00040109a40507040000004f"):
        raise ValueError("Entry mcBoard signature mismatch")
    record = bytearray(record)
    struct.pack_into("<i", record, 0x2664 + 3 + 2 + 3 + 1, 5)
    record = bytes(record)

    # Clone the fourth controller as Tween_Move4 before the initialization loop
    # reads this["Tween_Move" + i]. The stock controller rebuilds its label list
    # when card availability changes, so this block can execute more than once.
    # Replace the dynamic clip on every rebuild: retaining the first clone lets
    # the card refresh leave its nested board timeline in a hidden state. The
    # loop below immediately replaces board[4] with the fresh controller.
    #
    # Lumen serializes multi-value ActionPush operands in reverse VM stack
    # order. ActionCloneSprite pops depth, target, source, so serialize those
    # values as source, target, depth. The authored carousel occupies
    # display-list depths 924 through 987, so put the dynamic clone at the first
    # round unused depth above that range.
    clone_body = bytes.fromhex(
        "960a00040409a6050703000000"  # Push reg(4), string[1446], int(3)
        "474e"                    # Add2, GetMember -> source Tween_Move3
        "96080009a6050704000000"  # Push string[1446], int(4)
        "47"                      # Add2 -> target name "Tween_Move4"
        "96050007e8030000"        # Push depth 1000
        "24"                      # CloneSprite
    )
    fifth_controller = bytes.fromhex(
        "960a00040409a6050704000000"  # Push reg(4), "Tween_Move", int(4)
        "474e"                    # Add2, GetMember -> Tween_Move4
    )
    remove_existing = fifth_controller + b"\x25"  # RemoveSprite
    clone_fifth_controller = remove_existing + clone_body

    first_controller = bytes.fromhex(
        "960a00040409a6050700000000"  # Push reg(4), "Tween_Move", int(0)
        "474e"                    # Add2, GetMember -> Tween_Move0
    )
    align_fifth_controller = (
        # Tween_Move4.posY = Tween_Move0.posY. Lumen's carousel drives its
        # controller through posX/posY rather than the MovieClip wrapper's
        # _x/_y properties. This must run after Left(0), because Init and Left
        # are free to replace the transform installed at clone time. Keep the
        # stack in the same value/name/object form used by the stock SetMember
        # sequences in this function.
        fifth_controller
        + bytes.fromhex("96030009bc02")  # Push string[700:"posY"]
        + first_controller
        + bytes.fromhex(
            "96030009bc02"        # Push string[700:"posY"]
            "4e"                  # GetMember -> Tween_Move0.posY
        )
        + (bytes.fromhex(
            "9605000764000000"    # Push integer 100
            "0b"                  # Subtract: unmistakable visual probe
        ) if debug_y_probe else b"")
        + b"\x4f"                 # SetMember -> Tween_Move4.posY
    )

    # GetMode is after both Init edits.  The PC append shifts the original
    # controller-loop boundary from 0x592 to 0x5B3; insert the clone there.
    patched = insert_action_code(record, 0x1E3E, get_mode_match)
    # Original 0x743 is the End immediately after Init's final Left(0). Insert
    # before it while its offset is still unchanged by the earlier edits below.
    patched = insert_action_code(patched, 0x743, align_fifth_controller)
    patched = insert_action_code(patched, 0x4B9, pc_append)
    patched = insert_action_code(patched, 0x5B3, clone_fifth_controller)
    return patched


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
            strings = compact_strings(payload)
            if (strings[CAMPAIGN_STRING] != "キャンペーン" or
                    strings[PC_MODE_STRING] != "SetEntryPCMode"):
                raise ValueError("Entry string-pool signature mismatch")
            strings_checked = True
        elif tag == 0xF005:
            records = parse_action_records(payload)
            if len(records) <= ENTRY_RECORD:
                raise ValueError("Entry action record 97 is missing")
            records[ENTRY_RECORD] = patch_entry_actions(
                records[ENTRY_RECORD], debug_y_probe)
            payload = serialize_action_records(records)
            actions_patched = True
        elif tag == 0x0027:
            active_sprite = struct.unpack_from(">I", payload, 0)[0]
        elif tag == 0x002B and active_sprite in PC_MODE_BOARD_SPRITES:
            label, frame = struct.unpack_from(">II", payload, 0)
            # Campaign's stock label starts at frame 20, whose held visual is
            # Training. Move our replacement label to frame 30, where Campaign
            # is actually placed. Shop's own frame-30 label remains untouched.
            if label == CAMPAIGN_STRING and frame == 20:
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
        help="move Tween_Move4.posY 100 units above Tween_Move0 for diagnosis")
    args = parser.parse_args()
    build(args.source, args.packlist, args.font, args.output,
          args.debug_y_probe)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
