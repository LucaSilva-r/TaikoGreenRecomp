# Local TJA songs

Put TJA charts and their audio beneath `USRDIR/custom_songs/TJA`, then restart
TaikoRecomp. Open Taiko+ and choose **CUSTOM TJA**. Subfolders are scanned
recursively. The first directory beneath TJA is the category; song asset
directories beneath it are flattened so a category opens directly to playable
songs. Standard category names use the stock category colors. Search finds
songs across categories.

```text
USRDIR/custom_songs/
  TJA/
    Anime/
      My Song/
        chart.tja
        audio.ogg
  .cache/
```

`WAVE:` resolves relative to each TJA file. Keep the referenced audio in place;
the importer does not copy or transcode it. UTF-8 and Shift-JIS charts are
supported. The browser uses `TITLE`, course levels, and `DEMOSTART` for previews.
Easy, Normal, Hard, Oni and Ura/Edit solo charts are supported. Both players can
select independently from those courses; authored STYLE:Double charts are not
imported in this version.

The first launch converts charts in a background worker using the vendored
MIT-licensed tja2fumen from Zucchini-connector. Python 3.10 or newer is required.
Keep the generated `tools/` directory beside the executable when distributing
a build. It does not need pip packages, the connector server, Wine, or Sony's encoder.
Native FFmpeg decodes WAV, MP3, Ogg/Vorbis, Opus and FLAC; Windows/Pi dependency
bundles must be rebuilt with the updated `scripts/build_ffmpeg_*.sh` recipe.

Charts are cached under `.cache/<id>/<revision>`. Revisions include the TJA,
converter implementation, and HP table. Cache files are checked against their
sizes and SHA-256 hashes before reuse. A broken cache is rebuilt. Restart to
discover additions or changes. `.cache` can be deleted while the game is closed.
The original charts and audio are never modified.

The default custom folder is beside the resolved VFS `data` directory. This
follows a development checkout's `data` symlink back to its USRDIR. To use a
different existing song folder, set these in the config's `[environment]`:

```ini
TAIKO_CUSTOM_SONGS = /path/to/custom_songs
# Optional interpreter and converter overrides:
# TAIKO_PYTHON = /path/to/python3
# TAIKO_CUSTOM_TOOL = /path/to/tools/custom_songs.py
```

The library root must contain `TJA/`. A symlink to an existing TJA directory
works without duplicating its audio. The root needs write access for `.cache`.
Python and converter errors appear in the game log; preparation failure is
reported in the browser.

Green has a fixed 300-measure chart pool. Longer converted charts are rejected
before reaching the guest. Encoded audio and decoded stereo PCM each have a
256 MiB limit. These limits apply to individual songs, not library size.
Custom results are not uploaded using a stock song identity; persistent custom
scores are not implemented yet.

Custom titles use Zucchini’s calibrated native texture renderer with the
embedded game font. HUD/Results and transition textures are injected at the
game’s texture lookup, including `SUBTITLE` on transitions. Private upload
resources and persistent texture copies keep queued draws valid across scenes.

## Runtime integration

Each launch populates one reusable, natively copy-constructed BasicSong slot in
both the session and source catalogs. A narrow metadata lookup hook supplies
that slot's custom identity and ratings. Stock entries and installed files
remain intact. The read-only VFS overlay resolves that song's fumen and small
NUB/NSH descriptors from the cache.

The descriptors identify host-owned decoded PCM; they do not contain an ATRAC
transcode. The existing cellAtrac shim supplies it through the guest decoder
ring and bnusCore mixer. Gameplay start, seek, EOF, gain, and calibrated audio
offset retain the existing timing path. Chart preparation adds enough common
lead-in to place the earliest note at two seconds, padding PCM by exactly the
same number of samples. Browser previews use the unpadded source audio.

## Installed osu!lazer library

**OSU! LAZER** lists installed native osu!taiko maps. Maps are grouped by lazer beatmap-set identity and audio file, so unrelated
sets with the same title stay separate. Opening a song expands its named
difficulties, ordered by rating, with scrolling for larger sets. The selected
osu chart is shared by joined players and plays through Green's Oni slot;
changing it clears both players' readiness. All named difficulties are retained; osu!standard, catch and mania
maps are not converted. The displayed 1–10 level is an approximation from
lazer's stored star rating, not an official Taiko rating.

Discovery checks Linux/XDG, Flatpak and Windows roaming storage and follows
`storage.ini`'s `FullPath`. Set `TAIKO_OSU_LAZER` to a storage directory or its
`client.realm` to override it; set it to `0` to disable discovery. Restart after
changing the installed library. The read-only dynamic Realm helper follows
[osuplayer's reader](https://github.com/Founntain/osuplayer/blob/master/OsuPlayer.IO/DbReader/RealmReader.cs)
and osu!'s file models. It never migrates or edits the library. Chart/audio
paths resolve directly to `files/<first>/<first two>/<hash>`; only indexes and
converted fumen are cached in TaikoRecomp's custom cache.

Build the pinned Realm 20.1.0 helper with a .NET 8+ SDK:

```sh
scripts/build_osu_lazer_reader.sh
# Self-contained distribution (no user .NET installation):
scripts/build_osu_lazer_reader.sh build-linux/tools/osu_lazer_reader linux-x64
```

Keep the complete helper directory under `tools/` beside the executable.
Development DLL builds require a .NET 8 runtime; `TAIKO_DOTNET` overrides its
command, and `TAIKO_OSU_READER` selects a helper executable or DLL. Windows/Pi
releases must publish for their own runtime (`win-x64` / `linux-arm64`). Python
is still needed for chart conversion. If Realm cannot read a library version,
discovery reports an error and leaves the original library untouched.

## Automated validation

```sh
python3 tools/tests/test_custom_songs.py
ctest --test-dir build-linux -R 'taiko_custom_songs_tests|taiko_title_render_tests|taiko_browser_tests|taiko_catalog_identity_tests|taiko_audio_decoder_tests' --output-on-failure
```

The Python tests cover Shift-JIS metadata, big-endian fumen and lead-in,
changed-source rejection, missing audio and the measure limit. The C++ fixture
tests discovery, native audio decoding, chart/audio overlay access, proxy
resolution, exact PCM padding, and reuse of the disk chart cache.

Title-render tests cover Japanese and Latin text, long-title fitting, subtitles,
and visible fill/outline alpha in both native horizontal texture sizes.

Native Linux live validation (2026-09-09): a synthetic TJA/WAV song launched
through the host browser, loaded both native title types (11 and 12), decoded
1,056,000 stereo frames at 48 kHz including the shared two-second lead-in, and
reached native Results without the previous null-material/TOCBAD errors.
This checks integration; musical sync and Windows/Pi playback still require
platform-specific play testing.

Lazer integration validation (2026-09-09): the real redirected library resolved
15,779 native taiko records, with 15,778 playable/nonempty charts indexed. A
selected installed chart converted to big-endian fumen, decoded 3,203,658
preview frames directly from its hashed audio file, and opened through the
guest chart overlay. Full gameplay sync and Windows/ARM helper execution are
not yet live-validated for lazer maps.
