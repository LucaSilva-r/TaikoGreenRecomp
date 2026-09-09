# tja2fumen

Copied from `Zucchini-connector/app/tja2fumen` on 2026-09-09, including the
connector's chart conversion fixes and `hp_values.csv`. Upstream copyright and
MIT license are retained in `tja2fumen/LICENSE.txt`.

`../custom_songs.py` adapts the connector's `_convert_charts`, `_first_note_ms`
and lead-in flow for local files. It does not import the connector's service,
database, catalog, or Sony ATRAC encoding dependencies.
