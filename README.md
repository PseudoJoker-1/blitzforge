# BlitzForge

An in-game mod portal for World of Tanks Blitz: browse a reviewed catalogue
from inside the hangar and install with one click, in the style of Geode for
Geometry Dash.

The project is in early development. What works today is the client-side
catalogue screen and the registry API; installation is not wired up yet.

## What is here

| Path | What it is |
| --- | --- |
| `_mod_tools/build_catalog.py` | Generates the in-game catalogue screen from mod metadata and splices it into the hangar |
| `_mod_tools/patch_dvpl.py` | Edits `.dvpl` client resources with a one-time backup, so any change can be rolled back |
| `_mod_tools/dvpl.py` | `.dvpl` container reader/writer (LZ4 payload plus a 20-byte footer) |
| `_mod_tools/registry.py` | Registry client, with a disk cache so an outage cannot empty the hangar |
| `_mod_tools/modpack.py` | The artifact format: build, inspect, apply |
| `_mod_tools/install.py` | Install and remove mods from the registry |
| `_mod_tools/agent.py` | Carries out install and remove requests made from inside the game |
| `_mod_tools/mod_api/` | C ABI and runtime core for native mods: lifecycle, per-mod config, resource mounting, crash isolation |
| `_mod_tools/proxy_dll/` | `version.dll` proxy that injects the loader into the game |
| `backend/` | The mod registry API, deployed on Vercel |

## The catalogue screen

The screen is built entirely from the game's own declarative UI: DAVA YAML
plus its `.actions` scripting language. No native code is involved in drawing
it, so it matches the stock screens exactly and survives without a DLL.

```
python _mod_tools/build_catalog.py            # rebuild and install
python _mod_tools/build_catalog.py --dry-run  # print the generated markup
```

Every run starts from the pristine `Hangar.yaml` in the backup directory, so a
patch is never applied on top of a previous patch. Generated markup is checked
for structural damage before installation — the client gives no parse
diagnostics, a malformed screen is simply a crash on load.

## The artifact format

An artifact is a zip holding `blitzforge.json`, unified diffs under `patches/`,
and whole files under `files/`.

Two rules shape it.

**Patches carry no original content.** Diffs are generated with zero context
lines, so a patch holds only the lines the author adds and removes. A mod that
edits a shader ships its own thirty lines, not a copy of Wargaming's file. The
two sample artifacts quote zero original lines between them.

**A patch names the file it was built against.** Each one records the sha256 of
the pristine original and of the expected result. Applying to a client whose
file has changed — a game update, another mod — fails with both hashes rather
than producing something that is neither.

That hash is load-bearing rather than advisory. A diff that only inserts names
no existing line, so it has nothing to compare and will apply anywhere,
including at the wrong offset if the file has moved. Removals and replacements
are checked line by line; insertions are covered by the hash alone.

```
python _mod_tools/modpack.py build   mods/night-mode dist/night-mode-1.2.0.zip
python _mod_tools/modpack.py inspect dist/night-mode-1.2.0.zip
python _mod_tools/install.py install night-mode
python _mod_tools/install.py remove  night-mode
python _mod_tools/test_modpack.py    # round-trips the diff engine on real client files
```

Installing records which mod owns which file. Patches are built against the
pristine original, so two mods editing the same resource cannot both apply —
the second would be diffed against stock and would quietly erase the first.
That case is refused, not resolved.

## How a button in the hangar installs a mod

The actions language the catalogue is written in has 47 statement functions and
every one of them is UI or animation. There is no file access, no network call,
no way to open a URL. A button therefore cannot run the installer itself.

What it can do is write to the client log, which `agent.py` tails:

```
button  ->  Log("BLITZFORGE:install:" + str(index))
agent   ->  reads the line, maps the index to a mod id, runs the installer
```

The index is what the button has; `cache/catalog_index.json` maps it back to an
id and is written by the same `build_catalog` run that laid the cards out, so
the two cannot disagree about which card is which mod.

```
python _mod_tools/agent.py           # follow the log and act on requests
python _mod_tools/agent.py --probe   # check whether the log channel works
```

Resource patches change files the client reads at startup, so a press cannot
take effect in the running session. The detail page says so rather than
implying the mod is live.

## Review policy

A mod reaches the catalogue only after review. The registry returns an entry
only when `review.status == "approved"`, and that filter lives in the shared
layer rather than in each endpoint, so a new endpoint cannot forget it. A mod
still in review answers `404` rather than `403`: response codes should not
leak the queue.

Not published, regardless of how it is packaged:

- anything showing information the player should not have — enemy positions,
  hit points, reload state
- anything removing what obscures vision — vegetation, fog, darkness
- anything claiming to be a different platform or client than it is

**This is enforced by people, not by the API.** A native mod loader hands
third-party code `resolve_rva`, `find_pattern` and `hook_create`, because it
cannot function without them, and those are enough to build anything on the
list above. The review process is the actual control. Nothing in this
repository changes that, and the code does not pretend otherwise.

## What is deliberately absent

**Client resources.** `patch_dvpl.py` extracts originals from the user's own
installation into `_mod_tools/backup/`. Those are Wargaming's files and are
git-ignored. Mods that alter game resources are distributed as patches applied
to the user's copy, never as copies of the originals.

**A reverse-engineering address table.** An earlier build of this tooling
included aimbot, ESP, wallhack and vegetation removal, together with a curated
table of camera, entity and reload-timer addresses. All of it was removed, and
the address table is git-ignored so it cannot come back through a stray commit.

## Backend

```
cd backend
npm test          # handler tests: the approved-only filter, 404 for hidden mods
vercel deploy     # requires the Vercel CLI
```

| Endpoint | Returns |
| --- | --- |
| `GET /api/mods` | Approved mods |
| `GET /api/mods?type=resource` | Filtered by type |
| `GET /api/mods?q=rain` | Search over name, description and author |
| `GET /api/mods/:id` | One mod |

## Licence

MIT. See `LICENSE`.

`_mod_tools/proxy_dll/third_party/minhook` is vendored and carries its own BSD
2-clause licence.
