from __future__ import annotations

import bisect
import functools
import math
import re
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any



from tja2fumen.classes import (
    FumenCourse,
    FumenHeader,
    FumenMeasure,
    FumenNote,
)
from tja2fumen.converters import fix_dk_note_types_course


MAX_OSU_BYTES = 16 * 1024 * 1024
MAX_AUDIO_BYTES = 512 * 1024 * 1024
# Green's fumen loader has a fixed pool of 300 measure records.  Writing a
# larger count corrupts adjacent loader state instead of failing gracefully.
MAX_FUMEN_MEASURES = 300
SCORE_TARGET = 1_000_000
CONVERTER_VERSION = 4

COURSES = (
    ("e", "Easy", 2),
    ("n", "Normal", 4),
    ("h", "Hard", 6),
    ("m", "Oni", 8),
    ("x", "Ura", 10),
)


@dataclass(frozen=True)
class OsuChart:
    member: str
    audio_member: str
    title: str
    artist: str
    creator: str
    version: str
    osu_stars: float
    level: int
    course_hint: int | None = None
    display_title: str = ""


@dataclass(frozen=True)
class TimingPoint:
    offset: float
    beat_length: float
    meter: int
    uninherited: bool
    kiai: bool


@dataclass(frozen=True)
class HitEvent:
    start: float
    note_type: str
    duration: float = 0.0
    hits: int = 0


@dataclass
class ParsedBeatmap:
    mode: int = 0
    audio_filename: str = ""
    title: str = ""
    title_unicode: str = ""
    artist: str = ""
    artist_unicode: str = ""
    creator: str = ""
    version: str = ""
    slider_multiplier: float = 1.4
    timing_points: list[TimingPoint] = field(default_factory=list)
    hit_objects: list[list[str]] = field(default_factory=list)


def inspect_osz(path: Path) -> dict[str, Any] | None:
    """Return catalog metadata for the native-taiko charts in an OSZ."""
    try:
        stat = path.stat()
    except OSError:
        return None
    return _inspect_osz_cached(str(path), stat.st_size, stat.st_mtime_ns)


@functools.lru_cache(maxsize=8192)
def _inspect_osz_cached(
    path: str, _size: int, _mtime_ns: int
) -> dict[str, Any] | None:
    charts: list[OsuChart] = []
    try:
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            by_name = {info.filename.casefold(): info for info in members}
            for info in members:
                if not info.filename.lower().endswith(".osu"):
                    continue
                if info.file_size > MAX_OSU_BYTES or info.flag_bits & 0x1:
                    continue
                raw = archive.read(info)
                parsed = parse_osu(raw)
                if parsed.mode != 1 or not parsed.hit_objects:
                    continue
                audio_info = _resolve_member(
                    info.filename, parsed.audio_filename, by_name
                )
                if audio_info is None or audio_info.file_size > MAX_AUDIO_BYTES:
                    continue
                try:
                    import rosu_pp_py as rosu
                    beatmap = rosu.Beatmap(bytes=raw)
                    rating = float(rosu.Difficulty().calculate(beatmap).stars)
                except Exception:
                    continue
                level = max(1, min(10, int(math.floor(rating * 1.5 + 0.5))))
                charts.append(
                    OsuChart(
                        member=info.filename,
                        audio_member=audio_info.filename,
                        # osu!'s Title field is the romanized title intended
                        # for clients that cannot display TitleUnicode. Use it
                        # consistently in both the downloader and song select.
                        title=parsed.title or parsed.title_unicode,
                        artist=parsed.artist_unicode or parsed.artist,
                        creator=parsed.creator,
                        version=parsed.version,
                        osu_stars=rating,
                        level=level,
                        course_hint=_course_hint(parsed.version),
                        display_title=parsed.title or parsed.title_unicode,
                    )
                )
    except (OSError, zipfile.BadZipFile, RuntimeError):
        return None

    selected = _fill_required_courses(_select_charts(charts))
    if not selected:
        return None
    representative = selected[-1][1]
    return {
        "title": representative.title or Path(path).stem,
        # Kept for clients using the split-title library schema. For osu! songs
        # both public title fields intentionally carry the romanized Title.
        "display_title": representative.display_title or representative.title,
        "subtitle": representative.artist or None,
        "creator": representative.creator,
        "audio_member": representative.audio_member,
        "courses": [
            {
                "id": COURSES[course_idx][0],
                "label": COURSES[course_idx][1],
                "stars": chart.level,
                "osu_stars": round(chart.osu_stars, 2),
                "version": chart.version,
                "osu_member": chart.member,
            }
            for course_idx, chart in selected
        ],
    }


def parse_osu(raw: bytes) -> ParsedBeatmap:
    text = raw.decode("utf-8-sig", errors="replace")
    parsed = ParsedBeatmap()
    section = ""
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("//"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section == "TimingPoints":
            fields = [part.strip() for part in line.split(",")]
            if len(fields) >= 2:
                try:
                    parsed.timing_points.append(
                        TimingPoint(
                            offset=float(fields[0]),
                            beat_length=float(fields[1]),
                            meter=max(1, int(fields[2])) if len(fields) > 2 else 4,
                            uninherited=(len(fields) <= 6 or fields[6] != "0"),
                            kiai=(len(fields) > 7 and bool(int(fields[7]) & 1)),
                        )
                    )
                except ValueError:
                    pass
            continue
        if section == "HitObjects":
            fields = [part.strip() for part in line.split(",")]
            if len(fields) >= 5:
                parsed.hit_objects.append(fields)
            continue
        if ":" not in line:
            continue
        key, value = (part.strip() for part in line.split(":", 1))
        try:
            if section == "General" and key == "Mode":
                parsed.mode = int(value)
            elif section == "General" and key == "AudioFilename":
                parsed.audio_filename = value
            elif section == "Metadata" and key == "Title":
                parsed.title = value
            elif section == "Metadata" and key == "TitleUnicode":
                parsed.title_unicode = value
            elif section == "Metadata" and key == "Artist":
                parsed.artist = value
            elif section == "Metadata" and key == "ArtistUnicode":
                parsed.artist_unicode = value
            elif section == "Metadata" and key == "Creator":
                parsed.creator = value
            elif section == "Metadata" and key == "Version":
                parsed.version = value
            elif section == "Difficulty" and key == "SliderMultiplier":
                parsed.slider_multiplier = max(0.01, float(value))
        except ValueError:
            pass
    parsed.timing_points.sort(key=lambda point: point.offset)
    return parsed


def fumen_from_osu(raw: bytes, course_name: str, level: int) -> FumenCourse:
    parsed = parse_osu(raw)
    if parsed.mode != 1:
        raise ValueError("The selected .osu chart is not native osu!taiko data.")
    red_points = [
        point
        for point in parsed.timing_points
        if point.uninherited and point.beat_length > 0
    ]
    if not red_points:
        raise ValueError("The osu!taiko chart has no uninherited timing point.")
    events = _hit_events(parsed, red_points)
    if not events:
        raise ValueError("The osu!taiko chart has no supported hit objects.")

    boundaries, barlines = _measure_boundaries(parsed.timing_points, red_points, events)
    measures: list[FumenMeasure] = []
    for start, end in zip(boundaries, boundaries[1:]):
        red = _active_red(red_points, start)
        scroll, kiai = _timing_effects(parsed.timing_points, start)
        bpm = 60_000.0 / red.beat_length
        # Fumen measure offsets exclude an implicit 4/4 lead measure. The
        # game adds it back at playback time; storing the raw osu! timestamp
        # here would therefore make every note one full measure late.
        fumen_lead = 4 * 60_000.0 / bpm
        measure = FumenMeasure(
            bpm=bpm,
            offset_start=start - fumen_lead,
            offset_end=end - fumen_lead,
            duration=end - start,
            gogo=kiai,
            barline=_time_key(start) in barlines,
        )
        measure.branches["normal"].speed = scroll
        measures.append(measure)

    starts = boundaries[:-1]
    combo_notes = 0
    for event in events:
        idx = max(
            0,
            min(
                len(measures) - 1,
                bisect.bisect_right(starts, event.start) - 1,
            ),
        )
        measure = measures[idx]
        note = FumenNote(
            note_type=event.note_type,
            # Note positions are relative to the logical osu! measure, not
            # the serialized fumen offset (which excludes the lead measure).
            pos=max(0.0, event.start - starts[idx]),
            duration=max(0.0, event.duration),
            hits=event.hits,
        )
        branch = measure.branches["normal"]
        branch.notes.append(note)
        branch.length += 1
        if event.note_type.lower().startswith(("don", "ka")):
            combo_notes += 1

    # osu! files do not carry TJA SCOREINIT/SCOREDIFF metadata.  A zero value
    # makes Green award no points, so use the same Shin-Uchi-style fallback as
    # other Taiko players: a constant per-note value targeting one million.
    score_init = _shinuchi_score_init(combo_notes)
    for measure in measures:
        for note in measure.branches["normal"].notes:
            if note.note_type.lower().startswith(("don", "ka")):
                note.score_init = score_init
                note.score_diff = 0

    header = FumenHeader(order=">")
    header.b512_b515_number_of_measures = len(measures)
    header.set_hp_bytes(combo_notes, course_name, level)
    header.set_timing_windows(course_name)
    fumen = FumenCourse(
        header=header,
        measures=measures,
        score_init=score_init,
        score_diff=0,
    )
    fix_dk_note_types_course(fumen)
    return fumen


def read_member(path: Path, member: str, max_bytes: int) -> bytes:
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo(member)
        if info.file_size > max_bytes:
            raise ValueError(f"OSZ member is too large: {member}")
        if info.flag_bits & 0x1:
            raise ValueError(f"Encrypted OSZ member is unsupported: {member}")
        return archive.read(info)


def _resolve_member(
    chart_member: str,
    audio_filename: str,
    by_name: dict[str, zipfile.ZipInfo],
) -> zipfile.ZipInfo | None:
    if not audio_filename or "\0" in audio_filename:
        return None
    audio = PurePosixPath(audio_filename.replace("\\", "/"))
    if audio.is_absolute() or ".." in audio.parts:
        return None
    relative = PurePosixPath(chart_member).parent / audio
    return by_name.get(relative.as_posix().casefold()) or by_name.get(
        audio.as_posix().casefold()
    )


def _course_hint(version: str) -> int | None:
    value = version.casefold()
    patterns = (
        (4, r"\b(ura|inner|edit)\b|裏"),
        (3, r"\b(oni|insane|extreme|expert)\b|鬼"),
        (2, r"\b(hard|muzukashii)\b|難"),
        (1, r"\b(normal|futsuu)\b|普通"),
        (0, r"\b(easy|kantan|beginner)\b|簡単"),
    )
    for course, pattern in patterns:
        if re.search(pattern, value, flags=re.I):
            return course
    return None


def _select_charts(charts: list[OsuChart]) -> list[tuple[int, OsuChart]]:
    """Ordered sequence alignment between charts and the five Taiko slots."""
    ordered = sorted(
        charts,
        key=lambda chart: (
            chart.level,
            chart.osu_stars,
            chart.version.casefold(),
        ),
    )
    slot_count = (
        5
        if len(ordered) >= 5 or any(chart.course_hint == 4 for chart in ordered)
        else 4
    )
    required = min(slot_count, len(ordered))
    if not required:
        return []
    # (chart index, slot index, mapped count) -> (cost, assignments)
    states: dict[tuple[int, int, int], tuple[float, list[tuple[int, OsuChart]]]] = {
        (0, 0, 0): (0.0, [])
    }
    for chart_idx in range(len(ordered) + 1):
        for slot_idx in range(slot_count + 1):
            for mapped in range(required + 1):
                state = states.get((chart_idx, slot_idx, mapped))
                if state is None:
                    continue
                cost, assignments = state
                if chart_idx < len(ordered):
                    _keep_best(states, (chart_idx + 1, slot_idx, mapped), cost, assignments)
                if slot_idx < slot_count:
                    _keep_best(states, (chart_idx, slot_idx + 1, mapped), cost, assignments)
                if chart_idx < len(ordered) and slot_idx < slot_count and mapped < required:
                    chart = ordered[chart_idx]
                    target = COURSES[slot_idx][2]
                    hint_cost = (
                        0.0
                        if chart.course_hint is None
                        else abs(slot_idx - chart.course_hint) * 3.0
                    )
                    _keep_best(
                        states,
                        (chart_idx + 1, slot_idx + 1, mapped + 1),
                        cost + abs(chart.level - target) + hint_cost,
                        assignments + [(slot_idx, chart)],
                    )
    candidates = [
        value
        for (chart_idx, slot_idx, mapped), value in states.items()
        if chart_idx == len(ordered) and mapped == required
    ]
    return min(
        candidates,
        key=lambda item: (item[0], [(i, c.member) for i, c in item[1]]),
    )[1]


def _fill_required_courses(
    selected: list[tuple[int, OsuChart]],
) -> list[tuple[int, OsuChart]]:
    """Easy through Oni are mandatory; Ura remains an optional fifth slot."""
    if not selected:
        return []
    by_slot = dict(selected)
    available = [chart for _, chart in selected]
    for slot in range(4):
        if slot in by_slot:
            continue
        target = COURSES[slot][2]
        by_slot[slot] = min(
            available,
            key=lambda chart: (
                abs(chart.level - target),
                abs(slot - chart.course_hint) if chart.course_hint is not None else 0,
                chart.osu_stars,
                chart.member.casefold(),
            ),
        )
    return sorted(by_slot.items())


def _keep_best(
    states: dict[tuple[int, int, int], tuple[float, list[tuple[int, OsuChart]]]],
    key: tuple[int, int, int],
    cost: float,
    assignments: list[tuple[int, OsuChart]],
) -> None:
    old = states.get(key)
    if old is None or cost < old[0]:
        states[key] = (cost, assignments)


def _hit_events(parsed: ParsedBeatmap, red_points: list[TimingPoint]) -> list[HitEvent]:
    events: list[HitEvent] = []
    for fields in parsed.hit_objects:
        try:
            start = float(fields[2])
            kind = int(fields[3])
            sound = int(fields[4])
        except ValueError:
            continue
        if kind & 1:
            events.append(HitEvent(start, _note_type(sound)))
        elif kind & 2 and len(fields) >= 8:
            try:
                spans = max(1, int(fields[6]))
                pixel_length = float(fields[7])
            except ValueError:
                continue
            red = _active_red(red_points, start)
            scroll, _ = _timing_effects(parsed.timing_points, start)
            span_duration = (
                pixel_length * red.beat_length /
                (100.0 * parsed.slider_multiplier * max(0.01, scroll))
            )
            # osu!taiko turns a slider into a drum roll unless the *whole*
            # slider (all of its spans) is shorter than two beats, in which
            # case it becomes a short run of hits. Charters write long rolls
            # as a tiny slider repeated dozens of times, so comparing a single
            # span against a beat turned those rolls into unhittable streams.
            duration = span_duration * spans
            if duration < 2 * red.beat_length:
                edge_sounds = [sound] * (spans + 1)
                if len(fields) > 8 and fields[8]:
                    try:
                        edge_sounds = [int(value) for value in fields[8].split("|")]
                    except ValueError:
                        pass
                for edge in range(spans + 1):
                    edge_sound = edge_sounds[edge] if edge < len(edge_sounds) else sound
                    events.append(HitEvent(start + span_duration * edge, _note_type(edge_sound)))
            else:
                events.append(
                    HitEvent(start, "DRUMROLL" if sound & 4 else "Drumroll", duration)
                )
        elif kind & 8 and len(fields) >= 6:
            try:
                end = float(fields[5])
            except ValueError:
                continue
            duration = max(0.0, end - start)
            events.append(HitEvent(start, "Balloon", duration, max(1, int(duration / 122))))
    return sorted(events, key=lambda event: (event.start, event.duration))


def _note_type(sound: int) -> str:
    rim = bool(sound & (2 | 8))
    big = bool(sound & 4)
    if rim:
        return "KA" if big else "Ka"
    return "DON" if big else "Don"


def _active_red(red_points: list[TimingPoint], at: float) -> TimingPoint:
    offsets = [point.offset for point in red_points]
    idx = bisect.bisect_right(offsets, at + 0.001) - 1
    return red_points[max(0, idx)]


def _timing_effects(points: list[TimingPoint], at: float) -> tuple[float, bool]:
    scroll = 1.0
    kiai = False
    for point in points:
        if point.offset > at + 0.001:
            break
        kiai = point.kiai
        if point.uninherited:
            scroll = 1.0
        elif point.beat_length < 0:
            scroll = max(0.01, -100.0 / point.beat_length)
    return scroll, kiai


def _measure_boundaries(
    points: list[TimingPoint],
    red_points: list[TimingPoint],
    events: list[HitEvent],
) -> tuple[list[float], set[int]]:
    # Old maps can place an object just before their first timing point. Use
    # the first object as an implicit timing boundary instead of clamping it.
    start = min(red_points[0].offset, min(event.start for event in events))
    last_event = max(event.start + event.duration for event in events)
    final_red = _active_red(red_points, last_event)
    end = last_event + final_red.beat_length * final_red.meter
    required = {start, end}
    natural_barlines: set[float] = {start}
    barlines = {_time_key(start)}
    for idx, red in enumerate(red_points):
        if red.offset > end:
            break
        segment_end = min(end, red_points[idx + 1].offset if idx + 1 < len(red_points) else end)
        cursor = max(start, red.offset)
        measure_length = red.beat_length * red.meter
        while cursor < segment_end - 0.001:
            natural_barlines.add(cursor)
            barlines.add(_time_key(cursor))
            cursor += measure_length
        required.add(segment_end)
        if idx + 1 < len(red_points) and red_points[idx + 1].offset <= end:
            barlines.add(_time_key(red_points[idx + 1].offset))

    # Kiai can change on inherited timing points.  Those transitions affect
    # scoring and must remain exact even when a map contains thousands of
    # smooth-SV points.
    kiai = _timing_effects(points, start)[1]
    for point in points:
        if point.offset <= start + 0.001:
            continue
        if point.offset >= end - 0.001:
            break
        if point.kiai != kiai:
            required.add(point.offset)
        kiai = point.kiai

    required_boundaries = sorted(required)
    required_measures = len(required_boundaries) - 1
    if required_measures > MAX_FUMEN_MEASURES:
        raise ValueError(
            "osu! chart requires "
            f"{required_measures} BPM/kiai timing sections, but the game "
            f"supports at most {MAX_FUMEN_MEASURES}."
        )

    # Ordinary barlines are musically useful but may be thinned for a song
    # longer than 300 natural measures.  Note timestamps remain exact because
    # their positions are recalculated relative to whichever boundary remains.
    available = MAX_FUMEN_MEASURES + 1 - len(required_boundaries)
    selected_barlines = _evenly_sample(
        [value for value in natural_barlines if value not in required],
        available,
    )
    structural = required | set(selected_barlines)

    # A fumen measure stores only one scroll speed.  Mid-measure inherited
    # timing points are therefore useful approximation boundaries, not hard
    # structural requirements.  Keep all of them when possible; otherwise
    # sample them evenly across the song to stay inside the game's fixed pool.
    scroll_boundaries: list[float] = []
    scroll = _timing_effects(points, start)[0]
    for point in points:
        if point.offset <= start + 0.001:
            continue
        if point.offset >= end - 0.001:
            break
        new_scroll = scroll
        if point.uninherited:
            new_scroll = 1.0
        elif point.beat_length < 0:
            new_scroll = max(0.01, -100.0 / point.beat_length)
        if not math.isclose(new_scroll, scroll, rel_tol=1e-9, abs_tol=1e-9):
            if point.offset not in structural:
                scroll_boundaries.append(point.offset)
            scroll = new_scroll

    available = MAX_FUMEN_MEASURES + 1 - len(structural)
    selected_scroll = _evenly_sample(scroll_boundaries, available)
    boundaries = sorted(structural | set(selected_scroll))
    if len(boundaries) < 2:
        raise ValueError("Could not construct measures for osu!taiko chart.")
    return boundaries, barlines


def _evenly_sample(values: list[float], limit: int) -> list[float]:
    """Return at most ``limit`` ordered values spread over the full input."""
    values = sorted(set(values))
    if limit <= 0:
        return []
    if len(values) <= limit:
        return values
    if limit == 1:
        return [values[len(values) // 2]]
    return [
        values[round(index * (len(values) - 1) / (limit - 1))]
        for index in range(limit)
    ]


def _shinuchi_score_init(combo_notes: int) -> int:
    """Constant per-note score, rounded up to 10, for a ~1M perfect score."""
    if combo_notes <= 0:
        return 0
    return min(65535, math.ceil(SCORE_TARGET / combo_notes / 10) * 10)


def _time_key(value: float) -> int:
    return int(round(value * 1000))
