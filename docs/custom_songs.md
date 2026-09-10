# Local TJA songs

Put TJA charts and their audio beneath `USRDIR/custom_songs/TJA`, then restart
TaikoRecomp. Open Taiko+ and choose **CUSTOM TJA**. Subfolders are scanned
recursively. The first directory beneath TJA is the category; song asset
directories beneath it are flattened so a category opens directly to playable
songs. Standard category names use the stock category colors. Search finds
songs across categories.

With the host browser enabled, its library is indexed once on a background
thread during game startup, including installed TJA and osu!lazer charts.
Entering the browser reuses that index; if startup finishes before a very large
scan, entry waits for the remaining work. This does not change the stock game's
library loader. Chart conversion and audio decoding still happen on demand.
The log reports browser preload start and completion time.

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

Charts are converted in a background worker by native C++ code, ported from
Zucchini-connector's MIT-licensed tja2fumen and osu converters. No Python,
.NET runtime, converter scripts, or external helper executable is needed for
custom-song loading. The HP lookup table is embedded in the executable.
Native FFmpeg decodes WAV, MP3, Ogg/Vorbis, Opus and FLAC; Windows/Pi dependency
bundles must be rebuilt with the updated `scripts/build_ffmpeg_*.sh` recipe.

Charts are cached under `.cache/<id>/<revision>`. Revisions include the TJA,
converter implementation, and HP table. Cache files are checked against their
sizes and SHA-256 hashes before reuse. A broken cache is rebuilt. Restart to
discover additions or changes. `.cache` can be deleted while the game is closed.
The original charts and audio are never modified.

The default custom folder is beside the resolved VFS `data` directory. This
follows a development checkout's `data` symlink back to its USRDIR. To use a
different existing song folder, use the config's `[songs]` section:

```ini
[songs]
custom_folder = /path/to/custom_songs
osu_lazer = /path/to/osu/storage
```

The library root must contain `TJA/`. A symlink to an existing TJA directory
works without duplicating its audio. The root needs write access for `.cache`.
Leave either path blank to use automatic discovery. Set `osu_lazer = 0` to
disable osu discovery. `TAIKO_CUSTOM_SONGS` and `TAIKO_OSU_LAZER` environment
variables override these settings for individual runs. Existing config files
receive the new section automatically on launch while retaining known values.
Parser and converter errors appear in the game log; preparation failure is
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
`storage.ini`'s `FullPath`. Set `[songs] osu_lazer` to a storage directory or its
`client.realm` to override it; set it to `0` to disable discovery. Restart after
changing the installed library. The native Realm reader follows
[osuplayer's reader](https://github.com/Founntain/osuplayer/blob/master/OsuPlayer.IO/DbReader/RealmReader.cs)
and osu!'s file models. It holds a Realm read transaction to coordinate with
osu!lazer while reading. Realm may create its normal lock/management files,
but the reader never starts a write transaction, migrates the database, restores
backups, or edits chart/audio files. Chart/audio
paths resolve directly to `files/<first>/<first two>/<hash>`; only
converted fumen are cached in TaikoRecomp's custom cache.

Developers fetch the pinned Realm Core 14.14.0 sources once:

```sh
scripts/setup_realm.sh
# Then configure/build TaikoRecomp normally.
```

CMake 3.22.1 or newer builds the storage engine statically for the target platform. The setup
script applies tracked safeguards and portability fixes to the pinned source.
`TAIKO_REALM_SOURCE` selects another checkout with those same patches. If Realm
cannot read a library version, discovery reports an error without upgrading or
restoring the database. The Python converters remain development references;
they are not packaged or invoked by the game. Python is still used by existing
build-time code generation outside custom-song loading.

## Automated validation

```sh
python3 tools/tests/test_custom_songs.py
python3 tools/tests/test_native_charts.py
ctest --test-dir build-linux -R 'taiko_chart_tests|taiko_custom_songs_tests|taiko_title_render_tests|taiko_browser_tests|taiko_catalog_identity_tests|taiko_audio_decoder_tests' --output-on-failure
```

The Python tests cover Shift-JIS metadata, big-endian fumen and lead-in,
changed-source rejection, missing audio and the measure limit. The C++ fixture
tests discovery, native audio decoding, chart/audio overlay access, proxy
resolution, exact PCM padding, and reuse of the disk chart cache.

`test_native_charts.py` compares serialized native output to the Python reference.
Its optional `--tja-root` and `--osu-manifest` arguments extend the comparison to
installed chart corpora. The native Realm fixture checks filtering, shared-audio
grouping, database byte preservation, and rejection of a future database version
even when an older backup is available. Missing branch measures are copied by
value to avoid the old reference's duplicate-note bug; equally common BPM values
use a deterministic tie-break when assigning Don/Ka syllables.

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
guest chart overlay. This predates the native converter replacement.

Native replacement validation (2026-09-10): Linux and MinGW game builds pass.
The Linux chart/audio integration test runs with unavailable interpreter paths;
discovery with an empty executable search path finds all 2,845 local TJA files.
The complete TJA corpus plus five fixtures produces 2,847 byte-identical
conversions and three matching rejections against the corrected Python reference.
A 500-map osu sample plus those fixtures produces 504 byte-identical conversions
and one matching rejection. Direct installed-lazer discovery finds the same
15,778 playable charts, and the selected song passes native conversion, audio
decoding and the guest file overlay. The original Realm database SHA-256 remains
unchanged. Native chart and Realm tests also pass on Windows under Wine.
Full gameplay and ARM execution of this replacement still need live validation.
