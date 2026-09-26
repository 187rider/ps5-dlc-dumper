# ps5-dlc-dumper

A PS5 payload that copies **decrypted, currently-mounted DLC** straight to a USB
drive — no FTP, no `wget`, no waiting on a flaky socket. Internal SSD → USB, at
drive speed.

Written for dumping DLC you own from your own console.

---

## Quick start — the payload is already built

`dlc_dump.elf` in this folder is **compiled and ready** (98 KB, stripped, built
with ps5-payload-sdk / clang-18). You do not need to install the SDK unless you
want to modify the source.

```bash
brew install socat          # macOS, one time
./send.sh 192.168.1.6       # send it to the console
```

That's the whole thing. Build instructions are further down if you want them.

**Do this first, in this exact order**, or the payload will find nothing:

1. Jailbreak the console; make sure etaHEN is running.
2. Load **kstuff / kstuff-Lite** — *before* launching any game.
3. Plug in an **exFAT** USB drive, confirm the PS5 sees it.
4. Launch Fallout 4 and **enter the DLC content** (fast travel to Far Harbor /
   Nuka-World — not just the main menu).
5. Press the PS button to return to the home screen. **Do not close the game.**
6. Run `./send.sh <ps5-ip>` from your Mac.

---

## Running it FROM the console (no PC needed)

Dropping a bare `.elf` into a folder does **not** run it. Nothing on the PS5
scans for loose ELF files. A payload must be handed to an ELF loader. There are
three ways to do that; pick one.

### Option A — Homebrew Launcher, web UI (recommended)

**The ordering problem this solves:** the Homebrew Launcher runs homebrew by
hijacking a "big app" process, and a PS5 runs **one big app at a time**. So
launching a payload from the launcher while a game is running *closes that
game* — unmounting the DLC you came for. A one-shot dumper launched that way
always finds nothing.

The payload therefore defaults to a **resident web UI**, the same pattern
ps5-app-dumper uses:

1. FTP the **`DLC-Dumper`** folder (or unzip `DLC-Dumper-homebrew.zip`) into
   `/data/homebrew/`, `/mnt/usb0/homebrew/` or `/mnt/ext0/homebrew/`.
2. With **no game running**, open the Homebrew Launcher and pick **DLC Dumper**.
3. It goes resident and shows a notification: `http://<ps5-ip>:8082`
   (walks up to 8089 if busy — 8080 is websrv, 8081 is ps5-app-dumper).
4. **Now** start Fallout 4 and enter the DLC content.
5. Open that URL on your phone or PC and press **Dump**.

The page live-polls the console: whether a game sandbox is alive, which `-ac`
folders are mounted, the USB it found, and per-file progress. You can dump
everything or one DLC at a time.

Package layout (all three files matter):

```
/data/homebrew/DLC-Dumper/
├── eboot.elf            # required
├── homebrew.js          # menu entry + one-shot options
└── sce_sys/icon0.png    # required
```

### Option B — etaHEN Toolbox (one-shot)

etaHEN's payload menu loads the ELF without claiming the big-app slot, so the
running game survives. Good for a fire-and-forget dump.

1. FTP `dlc_dump.elf` to **`/data/etaHEN/payloads/`**
2. Start the game first, enter the DLC, PS button to home.
3. **Settings → etaHEN Toolbox → Plugins / Payload ELFs** → run it.

Because a game is already running, pass no web flag — but note the Toolbox
launches with no arguments, which starts the *web UI*. If you want a pure
one-shot from the Toolbox, rename a copy built with `--now` baked in, or just
use the web UI (it works fine with the game already running).

**Do NOT tick auto-start for this payload.** At boot nothing is mounted, and a
bad autoload entry can wedge the boot chain badly enough to need a USB
`autoload.txt` override to recover.

### Option C — send it over the network

```bash
./send.sh 192.168.1.6        # starts the web UI
```

Nothing to install on the console. Best when iterating.

---

## Two modes

| Invocation | Mode | Game must be running? |
|---|---|---|
| *(no args)* | Resident web UI on :8082 | **No** — start it first |
| `--now` | One-shot, dumps everything | **Yes** |
| `FALLOUT4DLC00003` | One-shot, filtered | **Yes** |
| `--web` | Force web UI | No |

---

## Which option should you use?

**Option A.** It is the only one that sidesteps the big-app ordering problem,
and it gives you live feedback instead of a silent dump.

Whichever you pick, the rule is absolute: **kstuff loaded before the game, game
running with the DLC content actually entered, exFAT USB plugged in.**

---

The command you were trying:

```bash
wget -r -nH --cut-dirs=3 "ftp://192.168.1.6:2121/mnt/sandbox/PPSA09017_000/addcont*"
```

won't work, for three separate reasons:

1. **`wget` can't glob directories over FTP.** Its FTP globbing only expands
   filenames in a single listing; `addcont*` as a directory wildcard combined
   with `-r` is not something it resolves.
2. **Recursive FTP is brutally slow and fragile here.** The PS5 FTP payloads are
   single-threaded and drop connections on long transfers. A 10 GB expansion
   over this path routinely dies halfway.
3. **It's the long way round.** The data is already on the console. Copying it
   SSD → USB locally avoids the network entirely.

---

## What it dumps

It checks two locations, in order:

| Source | What it is |
|---|---|
| `/mnt/sandbox/pfsmnt/<CONTENTID>-ac` | The canonical decrypted DLC mount. Preferred. |
| `/mnt/sandbox/<TITLEID>_000/addcont*` | In-sandbox addcont mount points. Used only if no `-ac` folders exist. |

It explicitly **skips** `-nest`, `-union`, `-app0` and `-patch0` (PFS internals
and base-game/patch data), and ignores all `NPXS4xxxx` system-app sandboxes.

Output goes to `<usb>/PS5_DLC_DUMP/<source-folder-name>/`, plus a `dump.log`
written alongside it. Folder names are preserved because **the folder name is
the content ID**, which you need if you ever repack to PKG.

---

## Prerequisites on the PS5

These are not optional — miss any one and you will dump nothing:

1. **kstuff / kstuff-Lite loaded BEFORE the game launches.** Without it the DLC
   entitlement check fails and nothing mounts. Loading it after the game is
   already running does not work; you must close the game and relaunch.
2. **The game is running right now.** PS5 storage is encrypted at rest — the
   mounted view is the only place decrypted DLC data exists. Closing the game
   destroys the mount.
3. **You entered the DLC content in-game at least once.** Many titles mount
   add-ons lazily. Fallout 4 is one of them: standing in Sanctuary is not enough
   for Far Harbor to be mounted.
4. **`elfldr` running** (port 9021) to receive the payload.
5. **An exFAT USB drive** plugged in and mounted by the system.
   Use exFAT, not FAT32 — FAT32's 4 GB per-file limit will truncate large
   archives. The payload detects this failure and says so in the log.
6. **Rest mode disabled**: Settings → System → Power Saving → Set Time Until
   PS5 Enters Rest Mode → *Don't Put in Rest Mode*.

---

## Building

### macOS

```bash
brew install llvm@18 wget socat
export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config

cd /tmp
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip

export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd ~/ps5-dlc-dumper
make
```

### Debian / Ubuntu

```bash
sudo apt-get install bash clang-18 lld-18 wget socat
cd /tmp
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd ~/ps5-dlc-dumper && make
```

Produces `dlc_dump.elf`.

---

## Running

With the game running and DLC mounted, any of these work:

```bash
./send.sh 192.168.1.6                                   # easiest
make test PS5_HOST=192.168.1.6                          # via the SDK helper
socat -t 99999999 - TCP:192.168.1.6:9021 < dlc_dump.elf # manual
```

> On macOS, do **not** use `nc -q0` — BSD netcat has no `-q` flag. Use `socat`.

> `send.sh` deliberately does **not** probe whether port 9021 is open first.
> Any TCP connection to the ELF loader is treated by it as a payload upload, so
> a "is it listening?" check would hand it a zero-byte ELF. It just sends and
> reports what happened.

You'll get a toast notification on the console when it starts and when it
finishes (with file count, size and error count).

### Dumping one specific DLC

Pass a substring filter as an argument. This needs an ELF loader that forwards
argv, such as `shsrv`:

```bash
# deploy the shell server once
socat -t 99999999 - TCP:192.168.1.6:9021 < shsrv-ps5.elf
telnet 192.168.1.6 2323

# then, inside the PS5 shell:
/data/dlc_dump.elf FALLOUT4DLC00003
```

With no argument it dumps every mounted DLC.

### Watching progress

Three ways, use whichever suits:

- The **toast notifications** on the console (start / finish).
- `<usb>/PS5_DLC_DUMP/dump.log`, flushed after every line.
- `klogsrv.elf` on port 3232 for a live kernel-log stream.

---

## Fallout 4 (PPSA09017) content IDs

The folder names you should expect to see, EU prefix `EP1003`:

| Label | DLC |
|---|---|
| `FALLOUT4DLC00001` | Automatron |
| `FALLOUT4DLC00002` | Wasteland Workshop |
| `FALLOUT4DLC00003` | Far Harbor |
| `FALLOUT4DLC00004` | Contraptions Workshop |
| `FALLOUT4DLC00005` | Vault-Tec Workshop |
| `FALLOUT4DLC00006` | Nuka-World |
| `ADDONSAEBUNDLE00` | Anniversary Edition Upgrade (bundle) |
| `FO4ANNIVERSARYED` | Anniversary Edition (bundle) |

So Far Harbor mounts as `EP1003-PPSA09017_00-FALLOUT4DLC00003-ac`.

**Note:** `ADDONSAEBUNDLE00` and `FO4ANNIVERSARYED` are license/entitlement
bundles that grant the six numbered items — they may produce no `-ac` folder or
an almost-empty one. That's expected, not a failure. The real payloads are the
numbered `FALLOUT4DLC0000X` entries.

---

## Using the dumps

Copy the **contents** of an `-ac` folder (not the folder itself) into the
matching game dump folder on your install target. The `sce_sys/param.sfo` inside
carries the metadata; `icon0.png` for repacking lives separately in
`/user/appmeta/addcont/<TITLEID>/<LABEL>/`.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `Cannot open /mnt/sandbox/pfsmnt` | No game running. |
| `NOTHING DUMPED` | kstuff not loaded before launch, or DLC never mounted in-game. |
| `no writable USB found` | Drive not mounted, or not writable. Re-seat it and check the PS5 sees it. |
| `write failed ... file >4GB` | USB is FAT32. Reformat as exFAT. |
| `USB IS FULL` | Self-explanatory — check free space against the DLC size first. |
| Dump completes but files are unusable | Copied from an at-rest location rather than a live mount, i.e. the game wasn't actually running. |

---

## Verifying the logic without a console

The scan/filter/copy code can be exercised against a fake directory tree:

```bash
make sim
# build a fake /tmp/ps5sim/pfsmnt tree, then:
./sim
```

Paths are overridable at compile time via `-DPFSMNT=`, `-DSANDBOX=`, `-DUSBBASE=`.
