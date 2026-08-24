<p align="center">
  <img src="assets/flipper2zero.png" alt="Flipper 2.Zero" width="560">
</p>

# nikita-qflipper

An unofficial fork of [qFlipper](https://github.com/flipperdevices/qFlipper) — the desktop companion app for the [Flipper Zero](https://flipperzero.one) — that turns it into an agentic workstation for the device.

On top of everything upstream qFlipper does (firmware updates over DFU, screen streaming, device recovery), this fork adds five things that share the same link to the device:

| | What it is |
|---|---|
| **Nikita** | A local LLM agent with tool-calling, running `qwen3:4b` on your own machine. Off until you enable it. It reads and writes the SD card, runs Flipper CLI commands and presses the device's buttons — and, within the access you grant it, carries out real work on your own computer: reading and writing files, running builds, driving shell commands. |
| **File manager** | Upstream lets you move files on and off the card. Here you also **edit them in place** — double-click opens an editor that saves straight back over RPC — **create** new files on the device, and select many at once for batch operations. |
| **CLI panel** | A unified shell where the same command name reaches either the Flipper or your computer, chosen by the path you give it. Transfers between the two are MD5-verified. |
| **Firmware store** | Six firmware sources (official and community) with release/dev channels, side by side, installed through qFlipper's existing update path. |
| **Bluetooth (BLE)** | Cable-free connect — scan, pair and run the same RPC session over Bluetooth Low Energy as an alternative to USB. |

Everything runs on your machine. The agent talks to [Ollama](https://ollama.com) on `localhost`; there is no API key, no account, and no request leaves the computer except firmware downloads and whatever you explicitly ask the agent to fetch.

> **Not affiliated with Flipper Devices.** "Flipper Zero" and "qFlipper" are their trademarks. This is an independent fork, licensed GPLv3 like the original.

---

## Screenshots

<table>
<tr>
<td width="50%" align="center">
<img src="assets/connect.png" alt="Connect screen: USB or Bluetooth" width="100%"><br>
<sub><b>Two ways in.</b> No Flipper connected yet — plug in over USB, or hit the Bluetooth icon next to the cable to scan and connect wirelessly instead.</sub>
</td>
<td width="50%" align="center">
<img src="assets/main.png" alt="Main window: connected over Bluetooth, device info, live screen, Nikita chat" width="100%"><br>
<sub><b>Connected — over Bluetooth.</b> Live device info, the Flipper's own screen mirrored in real time, firmware status, and Nikita ready to help, no cable involved.</sub>
</td>
</tr>
</table>

---

## Compared to upstream qFlipper

| | Upstream qFlipper | nikita-qflipper |
|---|---|---|
| **Firmware sources** | Official only | Official plus five community firmwares — Momentum, Unleashed, RogueMaster, ARF, Xero — listed side by side, each with its own release/dev channel |
| **Editing a file on the device** | Not possible. Download it, edit it locally, upload it back | Double-click it in the file manager. An editor opens in-app and Save writes straight back over RPC |
| **Creating a file** | Folders only | **New File**, created directly on the card |
| **Selecting files** | One at a time | Multi-select — modifier-click, rubber-band drag, select-all — with batch delete that names the count in the confirmation |
| **Backup / restore** | The internal storage (`/int`): settings and pairing data | The whole SD card, with live progress and `.tgz` packing. Upstream's `/int` tarball could never bring back a capture or a script |
| **Connecting to the device** | USB only | USB, or scan-and-connect over Bluetooth Low Energy — same RPC session either way |
| **Shell access** | None | A CLI panel that reaches the Flipper *and* your computer, with MD5-verified transfers between them |
| **AI assistant** | None | Nikita: one local model (`qwen3:4b`), tool-calling, installed from inside the app. Off until you switch it on, and erasable in one click |
| **What the assistant may touch** | — | Nine access switches, per group, enforced at execution — not just hidden from the model |
| **Work on your own computer** | None | Opt-in agent mode — read/write files, run builds and shell commands, with the results fed back into the conversation |
| **App self-update** | Enabled, pointed at Flipper Devices' server | Disabled on purpose — see [why](#why-the-app-self-updater-is-off) |

Everything upstream already does — DFU firmware installs, screen streaming, device info, recovery/repair — is untouched and still works the same way.

---

## How it fits together

qFlipper already owns the hard part: a USB CDC serial link to the device, a protobuf/RPC layer on top of it, and a state machine that tracks whether the Flipper is connected, streaming, updating, or in DFU. This fork does not replace any of that — it adds consumers of it.

```mermaid
flowchart TB
    subgraph qml["QML layer (application/components/)"]
        MW["MainWindow.qml"]
        NT["NikitaTalk.qml<br/>chat + live screen"]
        HO["HomeOverlay.qml<br/>firmware, backup, format"]
        FM["FileManager.qml"]
    end

    subgraph singles["C++ singletons (registered into QML)"]
        NB["Nikita<br/>NikitaBackend"]
        FS["Firmware<br/>FirmwareStore"]
        CLI["Cli<br/>FlipperCli"]
        BLE["Ble<br/>BleSpike"]
    end

    subgraph base["Upstream qFlipper (unchanged)"]
        AB["ApplicationBackend<br/>device state machine"]
        RPC["protobuf RPC<br/>storage, GUI, system"]
        DFU["DFU / update path"]
    end

    OLL["Ollama<br/>localhost:11434"]
    DEV["Flipper Zero<br/>USB CDC"]
    HOST["Your computer<br/>shell + filesystem"]

    qml --> singles
    NB --> OLL
    NB --> RPC
    NB --> HOST
    CLI --> DEV
    CLI --> HOST
    FS --> DFU
    singles --> AB
    AB --> RPC
    RPC --> DEV
```

The four singletons are registered in `application/application.cpp` as `Nikita`, `Firmware`, `Cli` and `Ble`, and are reachable from any QML file via `import QFlipper 1.0`.

---

## Nikita — the agent

### The loop

A turn is: build the prompt → stream from Ollama → if the model asked for tools, run them and feed the results back → repeat until it answers in prose.

- **Endpoint:** `POST http://localhost:11434/api/chat`, streamed.
- **Context:** `num_ctx` is set to 16384. The system prompt alone measures ~7.7k tokens, and 8192 left so little headroom that Ollama silently truncated *from the front* mid-turn — dropping the instructions that say which machine is which. Raising the window fixed a whole class of "hallucination" that was really amnesia.
- **No step cap.** A job that genuinely needs forty tool calls gets forty. What is bounded instead is *going in circles*: three consecutive rounds that produce no new call ends the turn, with a far-off ceiling as a runaway stop.

### The tool surface

Tools are assembled per turn, not fixed. Tool **count** is the single biggest lever on whether a small model calls anything at all — at 22 entries a 3B model stopped calling tools entirely and just described the work in prose. So the list is pruned to what the message is actually about.

**Always offered — memory:**
`remember` · `list_memory` · `forget`

**The Flipper** (dropped when the message is plainly about the computer):

| Tool | Does |
|---|---|
| `list_files` | List a path on the device (`/ext` is the SD root, `/int` internal) |
| `read_file` | Read a device file, up to ~8 KB |
| `save_file` | Write a file to the SD card |
| `make_dir` / `delete_file` / `rename_file` / `file_info` | The rest of the storage verbs |
| `press_button` | Tap up/down/left/right/ok/back — real D-pad input over RPC |
| `run_cli` | Any Flipper CLI command: `device_info`, `subghz`, `nfc`, `gpio`, `ir`, `led`, `vibro`, `power`, `js`, … |

**Your computer** — only offered when agent mode is on. This is the part with no equivalent upstream: Nikita stops being a chat window about the Flipper and becomes something that does work on your machine.

| Tool | Does |
|---|---|
| `host_read` / `host_write` | Read and overwrite text files anywhere you can |
| `host_run` | Run a shell command and get back the exit code plus combined stdout/stderr |
| `host_cd` | Move around, and report where it is and what is there |
| `host_list` / `host_find` | Browse and search the filesystem |
| `host_mkdir` / `host_move` / `host_copy` / `host_delete` | The rest of the file verbs |

In practice that means asking for the outcome instead of the steps: *"read the crash log on my Desktop and tell me what broke"*, *"build this project and fix the first error"*, *"pull every `.sub` off the card into a folder in Documents, grouped by frequency."* It runs the commands, reads the real output, and reports what actually happened rather than what it intended.

`host_cd` is stateful: the agent walks a working directory the way a person at a shell does, and `host_run` starts wherever it last landed — so a task can move somewhere and stay there across a dozen calls. Paths accept `~`, and well-known folder names ("Desktop", "Downloads", and their pt-BR spellings) resolve through `QStandardPaths` rather than being hardcoded, so they land in the right place on a localised install or a relocated home.

The two machines never blur together: the Flipper tools and the host tools are separate families with separate names, and a call aimed at the wrong one is rerouted rather than silently run in the wrong place.

### Not lying about what it did

A local 7B model will cheerfully report a file it never wrote. Most of the agent code is about making that impossible rather than unlikely:

- **Writes are read back.** `host_write` checks the file's size on disk against what it meant to write, and reports `verified: true/false` — the return value of `QFile::write` is not evidence.
- **Deletes distinguish "gone" from "was never there."** A delete of a missing file returns `deleted: false, existed: false` with an explicit instruction not to claim a deletion. Recursive removes are re-checked on disk afterwards, because `removeRecursively()` can return false having emptied most of a folder.
- **Claimed-but-unrun actions are caught.** The final text is scanned for claims ("saved to…", "created…") that no tool call backs up; when one is found the turn is sent back for correction, up to six times.
- **Destructive paths are refused.** Deleting `/`, a home directory, or anything at depth ≤ 1 is rejected with a message asking for something inside it instead.

### Memory

Two files, and they are **not** the same thing.

`memory.txt` holds everything Nikita learns about **you** — who you are and how you work, the machine and tools you use, what you are building and why, opinions and preferences, things you explain, decisions and the reason behind them. Not a log of tasks.

`actions-memory.txt` holds what it has **done**: which tool actually solved which kind of request, recorded only after the artifact was confirmed on disk. Read back into the prompt it reads as a sentence, not a stack trace — *"When asked to 'create rel2.txt in Desktop', I created rel2.txt and saved it to Desktop"* — because a small model copies the shape of what it reads.

Both live in `/ext/nikita` on the Flipper so they travel with the device, mirrored to a local cache so the agent still knows you when it is unplugged.

**Remembering is automatic.** You will almost never say "remember this" — you will just mention something. Nikita calls `remember()` silently in the same turn and carries on with what you actually asked. There is **no cap** on facts: the old build kept the most recent 40, which meant it quietly forgot the oldest thing it knew about you every time it learned a new one.

If you tell Nikita an action didn't work, the lesson is unlearned **and** the correction is written to `mistakes.txt`, which is read back on every turn as *"you were asked X, you did Y, that was wrong — do not repeat it"*. Forgetting a bad lesson stops it being recommended; nothing else stops the model reaching the same wrong conclusion again.

### Feedback — a second opinion, never a gate

After an action, a strip appears: **Did that do what you asked?** with `YES` / `NO` and an optional box to say what went wrong.

It opens **after** the turn has already closed and the lesson has already been filed automatically. Ignoring it, clearing the chat or quitting changes nothing — silence is not a verdict. It only ever adds.

The optional note is the valuable half: *"I asked it to open Chrome and it made a txt file"* is something no automatic check can produce. The tool reported success, the file exists, and the request was still not met. Only the person who asked can tell the difference.

### While it is thinking

A local 4B takes minutes, so the window says what is happening rather than sitting still:

```
●  1m 53s · 2.1k tokens · writing the file...
· wrote /Users/you/Desktop/note.txt (17 bytes)
· done in 2m 11s · 4.5k tokens
```

Every phrase is set at a real transition — `thinking`, `getting to work`, `writing the file`, `running the command`, `wrapping up` — never a decorative cycle. Tokens are prompt plus generated, summed across every round the turn takes.

The input **stays editable** while a turn runs. Anything typed is queued, shown as `↳ queued (1): …` with a `cancel`, and delivered as one message the moment the turn ends. The send button becomes `Queue` when there is text and `Stop` when there is not: while a turn runs, stopping is almost certainly not what that text was for.

### The model — one, local, yours

Nikita runs on **one** model: `qwen3:4b`, pulled and run by Ollama on your own machine. Nothing is sent anywhere.

The model manager (gear icon in the chat header) lists it, shows whether it is installed, and installs or removes it for you — the app drives `ollama` so you never touch a terminal. Ollama itself can be installed from the setup wizard if you don't have it.

| Tag | Size | Notes |
|---|---|---|
| `qwen3:4b` | 2.5 GB | The brain. Runs locally, reasons before it answers. |

Earlier versions shipped a catalog of six models plus an optional path to the real Claude CLI. Both are gone. One model that is understood and tuned beats six that are merely listed, and the settings that matter — context size, sampling, output ceiling — can only be tuned honestly for a model you actually measured.

Two things worth knowing about this particular build:

`qwen3:4b` on Ollama is the **Thinking-2507** variant. It always reasons before answering; there is no way to switch that off (`/no_think` is not honoured, `"think": false` only hides the reasoning, and a pre-closed `<think>` template does not work on it). Every request pays a reasoning pass.

Context is set to **8192**, not higher, and that is deliberate. Measured on an M1 with 8 GB: at 16384 the Ollama runner reaches 3.9 GB resident, the machine swaps, and generation collapses from ~18 tokens/s to 2.2 — an eight-fold slowdown that turns "create this file" into an eight-minute wait. At 8192 the model fits.

### Access filters — what it is allowed to touch

Under the model in the same panel, nine switches decide what Nikita can reach. `NO ACCESS` and `FULL ACCESS` are shortcuts for all of them at once; anything in between reads as `custom`.

| Group | Tools |
|---|---|
| Memory | `remember` `list_memory` `forget` |
| Flipper: read | `list_files` `read_file` `file_info` |
| Flipper: create and change | `save_file` `make_dir` `rename_file` |
| Flipper: delete | `delete_file` |
| Flipper: control | `press_button` `run_cli` |
| Computer: read | `host_list` `host_read` `host_find` `host_cd` |
| Computer: create and change | `host_write` `host_mkdir` `host_move` `host_copy` |
| Computer: delete | `host_delete` |
| Computer: run commands | `host_run` |

A disabled group is not merely hidden from the model — the call is refused at execution time as well. Hiding a tool from the list is a smaller menu, not a gate: a small model invents tool names, and an older conversation still in history carries calls from when the access was allowed.

The state is stored as the set that is **off**, so a group added in a later version arrives switched on rather than appearing disabled without anyone having disabled it.

### Personality

Five presets — Default (Nikita), Chill helper, Chaos gremlin, Deadpan pro, Sweet companion — or "build one from the name." The default is terse and low-key: short answers, no emoji, no mascot voice. The choice is persisted in `QSettings` and layered over the built-in system prompt.

---

## Off by default

A fresh install opens with Nikita **switched off**. The chat is replaced by a single control:

```
        (  ●    )
      ENABLE NIKITA
```

Plenty of people want a Flipper companion without any AI in it, and an assistant that reads files and keeps notes about you should be something you switch on — not something you discover already running. Off is not cosmetic: with the switch off the assistant cannot start a turn, and nothing touches `/ext/nikita` on the card. Five separate code paths used to write there on startup; all five now refuse.

**Turning it on** asks first, and says what starts happening: conversations kept on this computer, facts saved here and on the SD card, which actions worked, and files read or changed once you allow that in ACCESS. Nothing is sent anywhere — the model runs locally.

**Turning it off** warns that it erases everything Nikita stored about you, and then does it: the conversation, the facts, the learned actions, the permissions you granted it, the local files, and the whole `/ext/nikita` folder on the card — the folder itself, not just its contents, because an empty folder left behind reads as "something is still installed".

Both dialogs have Cancel and a confirm, and expand to full size if the text does not fit.

## Agent mode: read this before turning it on

Agent mode is **off by default** and gated behind an explicit opt-in in the setup wizard, with a warning. Here is the same warning at more length, because it matters:

**With agent mode on, a local language model can run arbitrary shell commands as you, and read, overwrite or delete any file you can.** The workspace folder you pick is *where it starts*, not a fence around it — `host_run` takes a full command line, so a boundary on the typed tools was never real, and pretending otherwise only pushed the model off tools that report failures honestly and onto `sh -c`, where mistakes are invisible.

Concretely:

- `host_run` executes via `/bin/sh -c` (or `cmd /c` on Windows), with a 15-minute timeout and captured output. There is no allowlist and no per-command confirmation.
- `host_write` and `host_delete` reach anywhere on the filesystem, minus the root/home guards described above.
- The agent reads files off your SD card and can download URLs. **That content enters the model's context.** A text file planted on a Flipper you didn't prepare yourself is a plausible prompt-injection vector.

Sensible use: keep the workspace under version control, leave agent mode off for day-to-day device work, and turn it on deliberately for a session where you want it. Per-action confirmation is on the roadmap below and is not implemented yet.

Without agent mode, Nikita still has the full device tool set — the SD card, the Flipper CLI, and the buttons. It just has no reach onto your computer.

---

## The CLI panel

A terminal that speaks to **two machines at once**, and is explicit about which one it means.

Most commands exist in three spellings:

```
ls  /ext/nfc      # a Flipper path -> runs on the Flipper
ls  ~/Downloads   # a host path    -> runs here
fls /ext/nfc      # the f-prefix always means the Flipper
```

Commands that route by path: `ls` `cat` `stat` `du` `wc` `grep` `head` `tail` `find` `diff` `mkdir` `touch` `rm` `mv` `file`.

Flipper-only: `fopen` (launch an app) · `fclose` · `freboot` · `fshutdown` · `fvibro` · `flocate` · `fdf` · **`fname <2-8 letters/numbers>`** — sets the Flipper's custom device name (same file, `/ext/dolphin/name.settings`, that Settings → Desktop → Change Flipper Name writes on the device itself) and reboots to apply it; `fname reset` goes back to the factory name.

Host-only, with a real explanation when you reach for the wrong machine — `ping`, `ifconfig`, `apt`, `ps`, `kill`, `mount`, `lsblk`, `uname`, `sudo`, `pm3` (Proxmark3), plus pass-through for `git`, `docker`, `ssh`, `nmap`, `openssl`, `tar` and friends. Asking the Flipper to `ping` doesn't fail silently; it tells you the Flipper has no network.

- **`python3 [script] [args...]`** — runs Python on this computer (the Flipper has none of its own). Point it at a device path — `python3 /ext/scripts/thing.py` — and it fetches the script off the SD card first and runs *that*, so a script you save on the card behaves the same on whatever computer the Flipper is plugged into next. A host path or a bare filename still just runs locally, same as any terminal.
- **`flipper install <url to a .fap> [Category/name.fap]`** — downloads a `.fap` and installs it straight to `/ext/apps`, the same 1 MB cap `wget` has. Files in `/ext/apps` only show up in the on-device app menu when they're filed under a category folder (`Tools`, `NFC`, `Games`, ...), so this defaults to `Tools` unless you say otherwise.

Commands that straddle both by design:

- **`cp [-r] <src> <dst>`** — copies in either direction, including host ↔ device, with wildcards. **Every transfer is MD5-verified.**
- **`wget <url> [dest]`** — downloads on the computer and writes the result straight onto the Flipper, which has no network of its own.
- **`edit <file>`** — opens an editor panel, for device files and host files alike.

Plus the shell affordances you expect: Tab completion (host and device paths), `history` with `!12` and `!!`, `colors on|off`, and `verbose on|off` for a wire-level log of everything a command actually sends.

`tgz <folder>` packs a folder the same way Backup does.

A command the panel doesn't recognize is sent to the Flipper as-is, so the firmware's own CLI is always reachable underneath — including commands the firmware itself drops you into a nested prompt for (`nfc`, `subghz`'s `chat`, `subshell_demo`...). Type `exit` to leave one, or just close and reopen the panel.

**Reconnects on its own after anything that reboots the device** — `freboot`, `fname`, a factory reset done from the Flipper's own menu, an update. The panel used to go dead until closed and reopened; now it waits for the same physical Flipper to reappear (under whatever port it comes back on, which a rename changes) and picks the session back up.

---

## File manager

Upstream's file manager is a transfer window: browse the card, drag files on, download them off, rename, delete, make a folder. This fork keeps all of that and makes the card something you can actually *work in*.

**Edit files in place.** Double-click any file and an editor opens inside the app, holding the file's real contents read over RPC. Save writes it straight back to the same path — no extension forcing, no download-edit-reupload round trip. Editing a `.txt` BadUSB script, a `.sub` capture's metadata or an app's config is now a two-click operation.

It is deliberately **one editor, shared**. The CLI panel's `edit <path>` opens the same panel, and the panel records which side opened it so Save routes back to the right backend. Two editors would eventually have looked and behaved differently; one cannot.

**Create files on the device.** A **New File** action makes a file directly on the card, so you can start a BadUSB script or a config from the app instead of creating it on your computer and copying it across.

**Work on several files at once.** Selection is a real selection: modifier-click to add, drag a rubber band across the list, or select all. Delete then acts on the whole set, and the confirmation tells you how many items it is about to remove rather than naming just one. Navigating to another folder clears the selection, so a stale pick from a previous directory can never be acted on by mistake.

Drag-and-drop upload from your desktop works as before, and the view refreshes itself after the agent or the editor writes to the card, so what is on screen is what is on the device.

---

## Firmware store

Upstream qFlipper installs the official firmware. This fork lists six sources side by side and installs any of them through the same DFU/update path:

| Source | Channels | |
|---|---|---|
| **Official** | release | Flipper Devices' own build |
| **Momentum** | release | Feature-rich community firmware |
| **Unleashed** | release, dev | Popular community firmware |
| **RogueMaster** | release, dev | Feature-packed community firmware |
| **ARF** | dev | Automotive / Sub-GHz research |
| **Xero** | release | Lightweight, based on the official one |

Official and Momentum are read from a `directory.json` update server; the rest from GitHub releases. Details worth knowing:

- Your channel choice is remembered **per source**.
- Payloads are cached, so switching channels doesn't re-fetch.
- What was learned last run is loaded at startup, so the panel shows correct versions before — and regardless of — what the network says this time.
- The GitHub sources share the unauthenticated **60-requests-per-hour** budget for your whole machine, and four are checked per refresh, so refreshes are throttled rather than fired on every panel open.

The channel a firmware was actually flashed from is recorded, so the app can tell "an update exists" apart from "you are on a different firmware than you were."

---

## Other device operations

Exposed in the home overlay, each behind a confirmation dialog:

- **Backup SD card** — walks the card and copies everything to your computer with live progress, packing it into a `.tgz`. This replaced upstream's Backup, which saved the *internal* storage (`/int`): that is settings and pairing data, so it could never bring back a capture, a script or an app's data.
- **Restore SD card** — from a `.tgz` or from a plain backup folder, so archives from either generation still restore.
- **Format SD card**
- **Reboot device**

The chat panel also carries a **live mirror of the Flipper's 128×64 screen**, driven by qFlipper's existing screen streaming — useful when the agent is navigating menus with `press_button`, since it is otherwise pressing blind.

---

## Bluetooth

Connect without the cable: a `BleTransport` implements the same `FlipperTransport` interface the USB link uses, so the RPC session on top — storage, device info, the live screen mirror — doesn't know or care which one it's running over. It's behind the `HZUI_BLE` compile flag, enabled on **Windows, Linux, and macOS**, each on Qt Bluetooth's native backend for that platform (WinRT, BlueZ/D-Bus, CoreBluetooth via Homebrew's `qtconnectivity`).

**Getting there:** the "no device" screen shows the USB cable and, right beside it, a Bluetooth icon (see the [screenshots](#screenshots) above) — click it to open the scan/connect panel. A found Flipper is identified by its GATT serial service UUID, not by name, so a device renamed away from the factory default still shows up correctly.

A few things worth knowing:

- **The CLI panel is still USB-only.** It talks to the device's raw USB serial port directly, which has no BLE equivalent — opening it over a Bluetooth session shows *"CLI is USB only."* instead of a blank terminal. Everything else (storage, file editing, device info, the screen mirror, firmware operations) works the same over either transport.
- **A stuck connection times out and fails cleanly**, rather than hanging forever. macOS in particular can hold onto a stale CoreBluetooth-side connection record from an earlier session, which otherwise leaves `connectToDevice()` with no callback ever firing — no error, no spinner giving up on its own. If a connection attempt does time out repeatedly, macOS's Bluetooth Settings → *Forget This Device* clears it.
- On macOS, the app's `Info.plist` carries `NSBluetoothAlwaysUsageDescription` — required for CoreBluetooth to scan or connect at all; without it the OS silently denies access with no dialog explaining why.

---

## Why the app self-updater is off

The comparison table above is all additions bar one. This is the removal, and it is deliberate.

**The application self-updater is disabled** (`DISABLE_APPLICATION_UPDATES` in `qflipper_common.pri`). Upstream's updater points at Flipper Devices' own server, so leaving it on would let a user "update" straight into vanilla qFlipper — silently uninstalling this fork. Flipper *firmware* updates use a separate registry and are unaffected. Re-enabling it means dropping the define **and** pointing `applicationupdateregistry` at a feed of this fork's own releases.

The base project structure is otherwise unchanged from [upstream](https://github.com/flipperdevices/qFlipper#project-structure).

---

## Requirements

**Runtime**

- A Flipper Zero — a USB cable that carries data, or Bluetooth on a build with `HZUI_BLE` enabled (the CLI panel still needs the cable; see [Bluetooth](#bluetooth)).
- [Ollama](https://ollama.com) running (`ollama serve`) with at least one model from the catalog above.
- ~6 GB of VRAM is comfortable for a 7B model. CPU-only works and is slower.
- Nikita is optional: without Ollama the app is still a working qFlipper with the CLI panel and firmware store.

**Build** — Qt **6.4.2** or newer (builds on 6.7 / 6.8), modules `qtserialport`, `qt5compat`, `qtsvg`, `qtimageformats`, `qtconnectivity`, plus libusb-1.0 and zlib.

---

## Build

A plain `git clone` is enough — `nanopb` and `libwdi` are vendored in the tree, not fetched as submodules.

### Linux

```bash
sudo apt update && sudo apt install -y \
    build-essential git \
    qt6-base-dev qt6-base-dev-tools qt6-declarative-dev \
    qt6-serialport-dev qt6-5compat-dev qt6-svg-dev qt6-connectivity-dev \
    libgl1-mesa-dev libusb-1.0-0-dev zlib1g-dev

git clone https://github.com/andresnalegre/nikita-qflipper
cd nikita-qflipper
mkdir build && cd build
qmake6 ../qFlipper.pro CONFIG+=qtquickcompiler
make -j"$(nproc)"
```

Binary: `build/application/qFlipper`.

`qt6-5compat-dev`, `qt6-svg-dev` and `qt6-connectivity-dev` are the easy ones to miss — qmake fails with `Unknown module(s) in QT: ...` if any is absent.

**USB permissions:**

```bash
sudo cp installer-assets/*.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

**AppImage:** `./build_linux.sh`, which needs `linuxdeploy` and `linuxdeploy-plugin-qt` on `PATH`. The reproducible way is the provided container:

```bash
docker build -t nikita-build docker/
docker run --rm --privileged -v "$(pwd)":/project nikita-build /project/build_linux.sh
```

### Windows

Needs Qt 6.4.2 `msvc2019_64`, MSVC 2019 build tools, and [`jom`](https://wiki.qt.io/Jom). Install Qt with [`aqt`](https://github.com/miurahr/aqtinstall), modules `qtdeclarative qttools qtserialport qt5compat qtmultimedia qtspeech qtimageformats svg`.

> Keep Qt on a **different drive** than the source. A qmake quirk makes it generate broken relative paths otherwise, and the build dies in `dfu`.

```bat
git clone https://github.com/andresnalegre/nikita-qflipper
cd nikita-qflipper
:: optional overrides:  set QT_DIR=...   set VS_VCVARS=...   set JOM=...
build_windows_dev.bat
```

Output: `build\qFlipper.exe`. On first run, stage Qt's DLLs and plugins next to it:

```bat
"%QT_DIR%\bin\windeployqt.exe" --qmldir application build\qFlipper.exe
```

After that, `build_windows_dev_inc.bat` is the fast incremental build for code and QML changes. (`build_windows.bat` is the separate full release build -- installer, code signing, the works -- and predates this fork; see the comment at its top.)

### macOS

```bash
./build_mac.sh
```

Builds for the host architecture only — the project-compiled nanopb dependency has no x86_64 slice on Apple Silicon, so a forced universal build fails to link. Override with `BUILD_ARCHS="x86_64 arm64" ./build_mac.sh` if you have universal dependencies.

---

## First run

1. Launch the app. Nikita is **off** — you get the Flipper side of qFlipper and nothing else. If that is all you wanted, you are done.
2. To use the assistant, click **ENABLE NIKITA** and read what it tells you before confirming.
3. Install Ollama and pull the model: `ollama pull qwen3:4b` — or let the app do both from the model manager (gear icon).
4. Make sure `ollama serve` is running.
5. Open the model manager to set **ACCESS**: what Nikita may read, change, delete or run, on the Flipper and on this computer. Everything is on by default once enabled; `NO ACCESS` turns it all off in one click.
6. **Agent mode** is separate and off by default — if you turn it on, pick the workspace folder it should start in. Read the section above first.

---

## Project layout

```
application/
  nikitabackend.{h,cpp}      NikitaBackend, FirmwareStore, FlipperCli
  application.cpp            registers the QML singletons
  blespike.cpp               BLE scan/connect panel
  bletransport.cpp           BLE FlipperTransport implementation
  components/
    NikitaTalk.qml           chat panel + live Flipper screen
    MainWindow.qml           shell, setup wizard, file editor, CLI and BLE overlays
    HomeOverlay.qml          firmware store, backup/restore/format
    FileManager.qml          card browser: new file, multi-select, drag-drop
    FileManagerDelegate.qml  per-row actions, batch delete, open-to-edit
backend/                     upstream: device state machine, RPC, operations
dfu/                         upstream: DFU/update transport
docker/                      Linux build image (Qt 6.4.2 via aqtinstall)
```

Everything outside `application/` is upstream qFlipper.

---

## Known limitations

Stated plainly, because they are the honest state of the tree:

- **`nikitabackend.cpp` is ~12k lines** and holds three unrelated classes. A split into three translation units is the next structural change.
- **No automated tests.** The pure functions worth covering first are path resolution, the tool-call parser, and the claimed-but-unrun detector. Two bugs found by hand would have been caught by any of them: a learning check that compared the raw argument path instead of the resolved one, so every relative-path write was judged a failure; and a de-duplication that recorded its state but never read it, so the same write went over USB on every cycle.
- **A turn takes minutes on a 4B.** Roughly 60–120 s for a simple action on an M1 with 8 GB, and 100 % of that is the model — the tool itself executes in milliseconds. The largest single win available is a non-reasoning model; the largest one already taken was fitting the context in RAM.
- **The Flipper's name cannot be changed on Official firmware.** `hardware_name` comes from the factory OTP block, read-only, and Official reads no `name.settings` from anywhere. The `name` command detects this and refuses instead of rebooting your device for nothing. Custom firmware (Momentum, Unleashed, RogueMaster) supports it.
- **Erasing the SD-card half is best effort.** With no Flipper attached, the local data is erased anyway and the two files on the card are not — they are recreated from scratch when the assistant is next enabled.
- **CI builds the Linux AppImage only, and only on release.** Nothing compiles on push, and Windows/macOS aren't covered at all.

---

## Credits

- **[lotei-qflipper](https://github.com/DUNKINKKD/lotei-qflipper)** — by [DUNKINKKD](https://github.com/DUNKINKKD). The visual language of this fork comes from Lotei, and Lotei is what made me want to build this architecture in the first place. The look, the terminal feel, the idea that a Flipper companion could have a character instead of a settings panel — that starting point is theirs.
- **[qFlipper](https://github.com/flipperdevices/qFlipper)** — Flipper Devices, the base this forks.
- **[Ollama](https://ollama.com)** and **[Qwen3](https://github.com/QwenLM/Qwen3)** — the local model stack.
- Firmware sources: Flipper Devices, [Momentum](https://momentum-fw.dev), [Unleashed](https://github.com/DarkFlippers/unleashed-firmware), [RogueMaster](https://github.com/RogueMaster/flipperzero-firmware-wPlugins), [ARF](https://github.com/D4C1-Labs/Flipper-ARF), [Xero](https://github.com/noproto/xero-firmware).

## License

**GPLv3**, inherited from qFlipper — see [LICENSE](LICENSE).
