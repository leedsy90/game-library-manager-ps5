# Game Library Manager 0.3.9 (PS5 payload)

A companion to ShadowMount Plus (SMP). It finds games that SMP can't see because they're in the wrong folder, fixes them, and moves or copies games between drives in a batch queue. It pops up a PS5 notification and (optionally) a phone push on every job.

## Install

1. Add `game-library-manager.elf` to your payload loader's autoload list **after** ShadowMount Plus, so it only ever starts once the jailbreak and SMP are up. For example, in plk-autoloader's `autoload.txt` (`!5000` is a 5-second wait):
   ```
   shadowmountplus.elf
   !3000
   kstuff.elf
   !5000
   game-library-manager.elf
   ```
   You can also just send it to port 9021 by hand.
2. Game Library Manager never starts by itself. If the console reboots, nothing runs until your loader loads it again.
3. A notification shows the address for your phone, e.g. `http://192.168.1.50:8095`.
4. **Home-screen tile:** the first run puts a **Game Library Manager** tile on your home screen, registered by ShadowMount Plus like a game. Pressing it opens the page in the PS5 browser.
   - If Game Library Manager isn't running, the tile only shows a notification saying so. It never starts the payload itself.
   - The tile is a web link (like the Payload Manager and Web File Manager tiles), installed as `/user/app/GLMG00001`. Its status, and a **Reinstall tile** button, are on the page. Put `tile=0` in the config to skip it.
5. **PS5 and phone at the same time:** all the work runs on the console, and any browser is just a window onto it. Both always show the same queue and progress, and closing either stops nothing.
6. Sending the payload again while it's running just opens the page; your queue is untouched.

## First use

The first time the page opens (on the PS5 or your phone), a short guide walks through each part of the page. It checks ShadowMount, runs the first scan live, and shows what was found. Skip it at any time, and reopen it with the **Guide** button at the top.

## What it does

| Section | What you get |
| --- | --- |
| **Needs attention** | Every game SMP won't pick up, with the reason and a suggested spot: too deep, extra wrapper folder, outside every scan folder, name starting with a dot, sitting in `backports`, or duplicate title IDs. It also looks *inside* `.exfat` and `.ffpkg` images (read-only, without mounting them) to catch images that have an extra top-level folder. **Fix** queues an instant rename on the same drive. **Fix all** queues the lot. |
| **Library** | Every game on every drive. With SMP 1.7's API it also shows whether SMP actually sees each one. Tick several games, pick a drive, then **Move** or **Copy**. |
| **Queue** | Runs one job at a time in the background, so you can close the page. It shows live MB/s, files done and time left. You can cancel, reorder or pause. It survives a crash or reboot: an interrupted job is restarted from scratch when you reload the payload. **Stop batch** cancels the job in progress (its partial copy is deleted and the original stays put) plus everything still waiting. Games that already finished are never touched or rolled back. |
| **Save fix** | For backported games that won't save. Finds games whose `fakelib` contains a save library (`libSceSaveData*.sprx`). **Apply save fix** renames it to `.disabled` so the console's own save library is used instead. Nothing is deleted, **Undo** reverses it, and it takes effect the next time the game starts. **Apply to all** does every affected game. Individual fakelib files are under *Advanced*. |

### How a move between drives works

1. The game is copied into a hidden `.glm-stage-N` folder next to the destination. SMP ignores names starting with a dot, so it never sees a half-copied game.
2. The copy is checked file by file (every file must be present with the same size).
3. The original is deleted. If SMP manages that game, SMP's own delete API does this, so it unmounts safely first.
4. The staging folder is renamed into place in a single, instant step, so the game appears all at once.
5. SMP is asked to rescan, and Game Library Manager waits until SMP reports the game at its new path.

If the original can't be removed (for example, the game is running), the copy is thrown away so you never end up with two copies.

Moves on the **same drive** are a plain rename and finish instantly, whatever the size.

### Speed

The copy engine copies 6 files at a time, using 8 MB buffers. Files bigger than 256 MB are split into 64 MB pieces that are copied in parallel. SMP's built-in mover copies one file at a time with 64 KB buffers. Tune `copy_threads` and `copy_buffer_mb` in the config if you like.

## 7z archives

Games still inside a `.7z` (including split `.7z.001` sets) show under **Needs attention**. Press **Extract…** to choose a drive.

- **Game part:** the game folder or image goes to `<drive>/homebrew/<TITLE_ID>`, with wrapper folders dropped so ShadowMount sees it straight away.
- **Everything else** in the archive (Fix, Backport, DLC, readme…) goes to `<drive>/game-extras/<TITLE_ID>/` with its original folder names, outside ShadowMount's scan folders. You can untick this.
- **Release-folder extras:** when the archive sits in a release folder next to separate Fix, Backport or DLC folders, those can be copied into the same `game-extras` folder too.
- **Recommendations:**
  - folder games: internal SSD first (fastest small-file loading), then M.2
  - images: internal SSD first, since ShadowMount's optimised image mode only applies there
  - drives without enough space are disabled
- **Verification:** every file's CRC32 is checked while it's written. Nothing appears in `homebrew` unless everything passes, and the archive is only deleted afterwards if you tick that option.
- **Speed:** LZMA2 is decoded on several cores when the archive was made with multi-threaded 7-Zip. Reading, decoding and writing overlap, and small files are written by 6 threads at once. Stored archives extract at disk speed.
- **Limits:** RAR/ZIP, PPMd, BCJ2 filter chains and encrypted archives aren't supported yet.

## PS5 packages (.pkg)

Fake packages (`.pkg`) show under **Needs attention**. **Check if convertible** reads the package header on the console (a few blocks, nothing is changed) and tells you:

- whether it's a fake (debug) or retail package
- whether the standard fake-package passcode unlocks it
- its internal layout and how the game data is compressed

Only packages locked with the standard passcode can ever be converted. Packages made with Sony's own publishing tools wrap their key differently, so the check reports them as **not convertible**; get those games as a folder, exFAT image or 7z instead. Converting is not built yet.

## Phone notifications

Edit `/data/game-library-manager/config.ini`, which is created on first run, then reload the payload.

```ini
# free ntfy app: subscribe to a hard-to-guess topic name
ntfy_url=https://ntfy.sh/game-library-change-me-1234
# and/or a Home Assistant webhook (plain http on your LAN)
webhook_url=http://192.168.1.10:8123/api/webhook/game-library-manager
```

Then press **Send test push** on the page. You'll get one push each time a job finishes, saying what's next, plus one when the whole queue is done.

## Other settings (`/data/game-library-manager/config.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `http_port` | 8095 | Web page port |
| `toasts` | 1 | PS5 pop-ups |
| `copy_threads` | 6 | Files copied in parallel (1 to 16) |
| `copy_buffer_mb` | 8 | Buffer per thread, in MB |
| `verify_timeout_s` | 120 | How long to wait for SMP to report a moved game |
| `walk_depth` | 5 | How deep to search each drive for misplaced games |

Game Library Manager reads `scan_depth` and `scanpath=` from SMP's `/data/shadowmount/config.ini`, so its "is this in the right place?" checks follow your SMP settings.

## ShadowMount Plus version

Game Library Manager checks SMP every scan and shows a banner at the top of the page if anything's off:

| Banner | Meaning |
| --- | --- |
| SMP not installed | No `/data/shadowmount` folder |
| SMP API switched off | `api_enabled=0` in SMP's config |
| SMP not found / older than 1.7 | Nothing answering on SMP's API port: SMP isn't loaded yet, or it's a version before the API existed |
| SMP x.y too old | SMP answers but is older than 1.7 |
| SMP needs updating | SMP 1.7 build that's missing an API feature Game Library Manager uses |

In all of these, finding, fixing and moving games still work; only the "does SMP see it?" checks are switched off.

## Limits in 0.3.9

- It can't look inside `.ffpfs` / `.ffpfsc` (PFS) images. It checks where they sit, not what's inside.
- Verification needs SMP 1.7 with its API enabled (`api_enabled=1`, the default). Without it, everything else still works.
- An image with a wrapper folder inside needs rebuilding on the PC. Game Library Manager tells you which ones, but can't repack them.
- Conversion to `.ffpkg` / `.ffpfsc` isn't included yet, and `.pkg` files can be checked but not converted.
- **Copy** keeps the original, so SMP will see two sources with the same title ID. Use **Move** unless you want a backup.

## Home tile notes

- The tile needs ShadowMount Plus 1.7 or newer, which can register `FAKExxxxx` title IDs. If SMP is older or not loaded, the page says the tile is waiting for ShadowMount.
- The tile is a tiny fake-signed launcher with no libraries (`tile/eboot.c`). Like your backups, it runs because Kstuff is loaded.
- Updating Game Library Manager refreshes the tile's files automatically the next time the new version runs.

## Building

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make            # game-library-manager.elf
make host       # Linux build for testing: ./glm-host --data DIR --drive DIR [--drive DIR...]
make tile-test  # Linux build of the tile launcher, to test its logic
```

GPL-3.0-or-later.
