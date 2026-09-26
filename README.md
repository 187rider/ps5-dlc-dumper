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

## Running the Payload

Dropping a bare `.elf` into a folder does **not** run it. Nothing on the PS5
scans for loose ELF files. A payload must be handed to an ELF loader.
Because **the game must remain running with its DLC mounted** in order for `/mnt/sandbox/pfsmnt` to exist, the payload must be run without claiming the console's foreground "Big App" slot.

Pick one of the two working methods below:

### Option 1 — Send over the network (Recommended)

`ps5-payload-elfldr` (listening on port 9021) runs as a background system daemon. Sending the payload here **never touches or closes your running game**.

1. Start your game (e.g. Fallout 4) and travel into the DLC area.
2. Press the **PS button** once to go to the home screen (**do NOT close the game**).
3. From your Mac or PC, run:
   ```bash
   ./send.sh 192.168.1.6        # replace with your PS5 IP
   ```
4. A notification appears on your TV: `DLC Dumper ready http://<ps5-ip>:8082`.
5. Open that URL on your phone or PC browser and click **Dump**.

The web UI live-polls the console: whether a game sandbox is alive, which `-ac`
folders are mounted, the USB it found, and per-file copy progress. You can dump
everything or one DLC at a time.

### Option 2 — etaHEN Toolbox (No PC needed)

etaHEN's payload menu loads the ELF inside the etaHEN system daemon rather than claiming the Big App slot, so your running game survives.

1. FTP `dlc_dump.elf` to **`/data/etaHEN/payloads/`**.
2. Start the game first, enter the DLC content, press **PS button** to home.
3. Open **Settings → etaHEN Toolbox → Plugins / Payload ELFs** → run `dlc_dump.elf`.
4. It starts the web UI on port 8082 without interrupting the game. Open `http://<ps5-ip>:8082` on your phone to trigger the dump.

> [!WARNING]
> **Do NOT tick auto-start for this payload in etaHEN.** At boot nothing is mounted, and a bad autoload entry can wedge the boot chain.

---

### Why NOT the Homebrew Launcher?

The PS5 operating system strictly enforces **one Big App process at a time**.

* The **Homebrew Launcher (HBL) is itself a Big App** (running inside a hijacked game/app process).
* Opening the Homebrew Launcher while a game is running forces the PS5 OS to **immediately terminate the game**, instantly unmounting `/mnt/sandbox/pfsmnt`.
* Launching HBL first and then launching the game forces the OS to terminate HBL (killing any dumper payload launched by it).

Therefore, **do not use the Homebrew Launcher** for DLC dumping. Always use **Option 1 (`./send.sh`)** or **Option 2 (etaHEN Toolbox)**.

---

## Two modes

| Invocation | Mode | Game must be running? |
|---|---|---|
| *(no args)* | Resident web UI on :8082 | **Yes** (suspended in background) |
| `--now` | One-shot, dumps immediately | **Yes** |
| `FALLOUT4DLC00003` | One-shot, filtered | **Yes** |
| `--web` | Force web UI | **Yes** |

---

## Which option should you use?

**Option 1 (`./send.sh`).** It leaves the game running cleanly, requires no installation on the console, and gives you live web UI feedback. If you don't have a computer nearby, use **Option 2 (etaHEN Toolbox)**.

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
