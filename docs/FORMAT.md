# Trekker-NG track format (v1)

A Trekker-NG track is **either a folder or a `.zip` archive** with the same content:

```
mytrack/
  meta.json
  drums.flac
  bass.flac
  melody.flac
  other.flac
```

In a zip, files may live in an enclosing folder (`mytrack/meta.json`); files are
matched by filename, so the folder name does not matter.

## Audio files

- **1 to 4 stems.** Missing stems are simply silent (the UI always shows 4 slots).
- **Stereo**, all with the **same sample rate**, all about the **same length**
  (shorter stems are padded with silence to the longest one).
- Formats: whatever miniaudio decodes - **WAV, FLAC, MP3**.
- **Recommendation: export WAV or FLAC.** MP3 encoder delay/padding shifts the
  start of the file by a few milliseconds and can misalign stems against each
  other; the shared playhead cannot fix a file that starts late.

## meta.json

```json
{
  "format_version": 1,
  "title": "My Song",
  "artist": "Me",
  "bpm": 128.0,
  "first_beat_offset_ms": 42.5,
  "stems": [
    { "name": "Drums",  "file": "drums.flac",  "color": "#ff5555" },
    { "name": "Bass",   "file": "bass.flac",   "color": "#ffaa00" },
    { "name": "Melody", "file": "melody.flac", "color": "#55aaff" },
    { "name": "Other",  "file": "other.flac",  "color": "#66dd88" }
  ],
  "cues": [
    { "name": "Intro", "position_ms": 0 },
    { "name": "Drop",  "position_ms": 61250 }
  ],
  "loops": []
}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `format_version` | number | no | 1 | Bumped only on breaking changes |
| `title` | string | no | track file/folder name | Display title |
| `artist` | string | no | `""` | Display artist |
| `bpm` | number | no | 0 (unknown) | Constant tempo of the track |
| `first_beat_offset_ms` | number | no | 0 | Time of the first beat from track start |
| `stems` | array | **yes** | - | 1..4 entries, played in order |
| `stems[].name` | string | no | `Stem N` | Display name |
| `stems[].file` | string | **yes** | - | File name inside the folder/zip |
| `stems[].color` | string | no | palette | `#rrggbb` |
| `cues` | array | no | `[]` | `{ "name", "position_ms" }` hot cues |
| `loops` | array | no | `[]` | reserved |

- **Unknown fields are ignored** - new optional fields can be added without
  breaking older builds (forward compatibility).
- The beat grid in v1 is `bpm` + `first_beat_offset_ms` (the producer knows
  both from the DAW); no freeform beat markers yet.

## Cue persistence

When the user edits cue points in the app:

- **Folder track** - `meta.json` is rewritten in place (written to
  `meta.json.tmp` first, then atomically renamed; every other field is kept).
- **Zip track** - the archive is **never modified**. Cues are written to a
  sidecar file right next to it: `mytrack.zip.cues.json`, containing the same
  object shape as `meta.json` (at minimum a `"cues"` array).

On load, `cues` come from `meta.json`; a valid sidecar with a `"cues"` array
overrides them. A missing or corrupt sidecar is ignored (the `meta.json` cues
stand). Loop points are session-only and are never persisted.

## Example: zipping a track

```
cd mytrack
zip -r ../mytrack.zip .
```

Any zip tool works as long as `meta.json` and the stem files are inside.
