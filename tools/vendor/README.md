# tja2fumen

Copied from `Zucchini-connector/app/tja2fumen` on 2026-09-09, including the
connector's chart conversion fixes and `hp_values.csv`. Upstream copyright and
MIT license are retained in `tja2fumen/LICENSE.txt`.

`../custom_songs.py` adapts the connector's `_convert_charts`, `_first_note_ms`
and lead-in flow for local files. It does not import the connector's service,
database, catalog, or Sony ATRAC encoding dependencies.

These Python modules now serve as development references for the native C++
converter in `src/taiko_chart*.cpp`; they are not runtime dependencies.
The native port and reference both break equally common BPM ties deterministically
and deep-copy missing branch measures, preventing shared measures from duplicating
notes or consuming another branch's commands.

# osu!taiko converter

`osu.py` is copied from `Zucchini-connector/app/osu.py` (2026-09-09).
The optional rosu import is deferred to OSZ inspection; direct lazer conversion
uses the installed database rating and needs no additional Python packages.
