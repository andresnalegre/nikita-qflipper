# CLI panel reference

The CLI panel is the terminal built into nikita-qflipper. It's one shell that
reaches two machines at once: **the Flipper Zero over USB**, and **the
computer nikita-qflipper is running on**. Which one a command touches is
decided by the command's *name*, not by anything you configure — that's the
whole design, and it's worth understanding before the table below makes sense.

## Opening it

The panel needs a Flipper connected over USB before it will open at all —
even for commands that never touch the Flipper. If nothing's plugged in
you'll get "Connect a Flipper over USB first." instead of a prompt. USB-only
for now; a BLE-connected Flipper can't host the panel.

Opening it pauses the app's normal RPC session with the device (screen
streaming, the file manager, live diagnostics) for as long as the panel is
open, because the panel and RPC share the same serial link and can't talk at
the same time. Closing the panel hands the link back.

## The prompt

```
Nikita@qflipper ~ %
```

- `Nikita` — the connected Flipper's own name (or the literal word `flipper`
  if it doesn't have one set).
- `~` — where you are **on this computer**, home-relative (`cd`'s bare form
  changes this).
- `[f:~/nfc]` — appears only when the Flipper side isn't sitting at its root.
  No Flipper segment at all means the Flipper is currently at `/ext`.

So `Nikita@qflipper ~/Desktop [f:~/subghz] %` means: this computer is
currently in `~/Desktop`, and the Flipper is currently in `/ext/subghz`.

## The one rule that explains everything

| Prefix | Machine | Example |
|---|---|---|
| **bare** (`ls`, `cat`, `rm`) | **this computer** — the real Unix program of that name | `ls /ext` lists a folder named `ext` on your computer, if one exists — it is **not** the SD card |
| **f-prefixed** (`fls`, `fcat`, `frm`) | **the Flipper** | `fls /ext` lists the SD card root |

That's it. A bare name is never secretly the Flipper, and an f-name is never
secretly this computer. A handful of commands (`cp`, `wget`, `edit`) are
smart enough to look at the *path* you give them and work out which machine
you mean on their own — those are the only ones with no prefix pair, because
they don't need one.

Every command below that exists on both machines is listed once, as
`name` / `fname`.

## History, completion, interrupting

- **`!!`** repeats the last command, **`!12`** re-runs history entry 12,
  **`!ls`** re-runs the most recent command starting with `ls`. `history`
  lists what's there (`history -c` clears it); it survives between sessions.
- **Tab** completes a command name or a path, on whichever machine the
  partial word implies.
- **⌘C / Ctrl-C** (or **Esc**) interrupts whatever's running — a stuck
  transfer, a long-running host command, `pm3`, or a live `python3` session
  (see below). It does **not** ask for confirmation first; it just stops it.
- A pasted block of several lines runs one line at a time, in order.
- Nothing you type here needs approval before it runs. That confirmation
  dialog belongs to Nikita's *own* tool calls (when the AI decides to touch
  your files on its own) — a human typing a command directly at this prompt
  **is** the approval.

## Flipper's own commands pass through unchanged

Anything that's the Flipper firmware's own vocabulary — `device_info`, `info`,
`gpio`, `subghz`, `nfc`, `rfid`, `ir`, `led`, `loader`, `storage`, `power`,
`top`, `log`, `js`, and the rest of the real Flipper CLI — is sent straight to
the device exactly as typed, no prefix, no translation. The table below only
covers the commands this panel *adds* on top of that.

---

## Commands that exist on both machines

| Command / Flipper form | Aliases | Usage | What it does |
|---|---|---|---|
| `cat` / `fcat` | `read`, `type` | `cat <file>` | Prints a file's contents to the screen. |
| `cd` / `fcd` | `chdir`, `lcd` | `cd [path]` | Changes the current folder. `cd -` goes back, `cd ~` goes to `/ext` (Flipper side) or home (computer side). |
| `cp` / *(no prefix — see above)* | `copy`, `pull`, `push` | `cp [-r] <source> <destination>` | Copies a file or folder (`-r`), **including between this computer and the Flipper** — the path tells it the direction. Wildcards (`*.sub`) work as a source. Every transfer is MD5-verified. |
| `df` / `fdf` | `diskfree` | `df [path]` | Free/used space on the Flipper's storage. |
| `diff` / `fdiff` | — | `diff <fileA> <fileB>` | Line-by-line diff of two files. |
| `du` / `fdu` | — | `du [path]` | How much space a folder uses, broken down by contents. |
| `echo` / `fecho` | — | `echo <text> [> file \| >> file]` | Echoes text; with a redirect, writes it to a file on the named machine. |
| `edit` / *(no prefix)* | `nano`, `vi`, `vim`, `emacs`, `pico`, `micro` | `edit <file>` | Opens a file in the built-in editor panel — works on either machine. |
| `file` / `ffile` | — | `file <path>` | Identifies what a file is from its contents. |
| `find` / `ffind` | — | `find <pattern> [path]` | Finds files by name under a folder, e.g. `find *.sub /ext/subghz`. |
| `grep` / `fgrep` | — | `grep <text> <file>` | Prints the lines of a file containing some text. |
| `head` / `fhead` | — | `head [-n lines] <file>` | First lines of a file. |
| `ls` / `fls` | `dir`, `ll`, `la` | `ls [path]` | Lists a folder's contents. |
| `md5` / `fmd5` | `md5sum` | `md5 <file>` | Prints a file's MD5 hash. |
| `mkdir` / `fmkdir` | `md` | `mkdir [-p] <path>` | Creates a folder; `-p` creates missing parents too. |
| `mv` / `fmv` | `move`, `ren` | `mv <source> <destination>` | Moves or renames. |
| `pwd` / `fpwd` | `lpwd` | `pwd` | Prints the current folder. |
| `rm` / `frm` | `del`, `erase` | `rm [-r] [-f] <path>` | Deletes a file, or a folder and everything in it (`-r`). Wildcards work. A whole tree needs `-f` too. |
| `sed` / `fsed` | — | `sed s/old/new/[g] <file>` | Find-and-replace inside a file; overwrites it with the result. |
| `stat` / `fstat` | — | `stat <path>` | Size and type of a file or folder. |
| `tail` / `ftail` | — | `tail [-n lines] <file>` | Last lines of a file. |
| `touch` / `ftouch` | — | `touch <file>` | Creates an empty file (Flipper side) / updates a timestamp (host side). |
| `tree` / `ftree` | — | `tree [path]` | Recursive listing of everything under a folder. |
| `wc` / `fwc` | — | `wc <file>` | Line/word/byte count. |
| `wget` / *(no prefix)* | `curl`, `fetch` | `wget <url> [destination]` | Downloads a URL **and saves it straight onto the Flipper** — the Flipper has no network of its own, so this is how it gets one. |
| `whoami` / `fwhoami` | — | `whoami` | Your user name on this computer / the Flipper's own identity. |

## Flipper-only

| Command | Aliases | Usage | What it does |
|---|---|---|---|
| `fclose` | — | `fclose` | Closes the app currently running on the Flipper (`loader close`). |
| `fname` | `rename` | `name <2-8 chars> \| name reset` | Sets the Flipper's custom device name — the same as Settings → Desktop → Change Flipper Name — then reboots to apply it. `name reset` restores the factory name. |
| `flocate` | — | `flocate <text>` | Searches the whole SD card for a name (a `find` rooted at `/ext`). |
| `fopen` | — | `fopen <app>` | Launches an app **on the Flipper**, e.g. `fopen NFC`. (The bare `open` below is a different command, for this computer.) |
| `freboot` | `restart` | `freboot` | Restarts the Flipper. |
| `fshutdown` | `poweroff`, `halt` | `fshutdown` | Powers the Flipper off. |
| `fvibro` | `buzz`, `vibrate` | `fvibro [0\|1]` | Turns the vibration motor on/off. |

## This computer only

| Command | Aliases | Usage | What it does |
|---|---|---|---|
| `apt` | `apt-get`, `yum`, `dnf`, `brew`, `pacman`, `apk`, `zypper` | `apt [args]` | Runs the package manager for whatever's actually on this machine. The Flipper has no package manager — its apps are `.fap` files copied into `/ext/apps`. |
| `chmod` | `chown`, `chgrp` | `chmod <mode> <path>` | Changes permissions. The Flipper's FatFS has no owners or permission bits, so this only ever makes sense here. |
| `fdisk` | `diskutil` | `fdisk [args]` | Runs `fdisk` on this computer. |
| `flipper install` | — | `flipper install <url to a .fap> [name.fap]` | Downloads a `.fap` from a URL and installs it straight to `/ext/apps` — a package-manager-style install for the Flipper. Same 1 MB cap as `wget`. Check with `loader list`, launch with `fopen "App Name"`. |
| `hostname` | — | `hostname` | This computer's host name. |
| `ifconfig` | `ip` | `ifconfig` | This computer's network interfaces. |
| `kill` | `killall`, `pkill` | `kill <pid>` | Kills a process on this computer. To stop the Flipper's running app use `close`/`fclose`; to stop a command use Ctrl-C. |
| `lsblk` | — | `lsblk` | This computer's block devices. |
| `mount` | — | `mount` | This computer's mounts. (The Flipper's `/int` and `/ext` are always mounted — `df` shows their space.) |
| `ping` | — | `ping <host>` | Pings a host from this computer. |
| `pm3` | `proxmark3`, `proxmark` | `pm3 <proxmark command>` | Runs a command via the Proxmark3 client (the Proxmark hardware must be plugged into *this* computer), e.g. `pm3 hf search`. Batch mode under the hood (`pm3 -c "..."`), so it runs one command and exits rather than opening its own interactive prompt. |
| `ps` | — | `ps` | Processes on this computer. For the Flipper's own threads, use `top` (a native Flipper command, passed through). |
| `python3` | `python` | `python3 [script.py \| /ext/path/to/script.py] [args...]` | See **Python** below — this one has real nuance. |
| `su` | `login`, `passwd`, `useradd` | `su [user]` | Needs a real terminal to ask for a password, which this panel isn't. Refused outright, with a pointer to `sudo` or a real terminal window. |
| `sudo` | `doas` | `sudo <command>` | Runs a command as root on this computer. No terminal here to type a password into — `sudo -v` in a real terminal first to cache credentials, then `sudo <command>` works here. |
| `umount` | `unmount` | `umount <target>` | Unmounts a volume on this computer. |
| `uname` | — | `uname` | This computer's system info. For the Flipper's own, use the native `device_info`. |

## Panel-only (never touch the wire)

| Command | Aliases | Usage | What it does |
|---|---|---|---|
| `clear` | `cls` | `clear` | Clears the terminal view. |
| `colors` | `color`, `colours` | `colors on \| off` | Colours folders, prompt and log lines, `ls --color` style. |
| `help` | `?` | `help [command]` | Lists every command, or explains one. Three groups: **Flipper** (the `f`-prefixed set), **Computer** (everything that runs here), **Firmware** (the connected Flipper's own commands). |
| `history` | — | `history [-c]` | Lists commands typed this session; `-c` clears it. |
| `host` | `local`, `run` | `host <command> [args]` | Explicit escape hatch: runs *anything* on this computer, even a name that would otherwise be ambiguous or unknown — `host whoami`, `host git status`, `host bash myscript.sh`. |
| `tgz` | — | `tgz <folder> [archive.tgz]` | Packs a folder on this computer into a `.tgz`, the same way the app's own Backup feature does. |
| `verbose` | — | `verbose on \| off` | Shows or hides the wire-level log of every command sent to the device. |

## Other real programs on this computer, no dedicated row

These have no Flipper meaning at all, so there's nothing to disambiguate —
they're just run directly, bare, on this computer: `awk`, `base64`, `dig`,
`docker`, `env`, `git`, `gzip`, `hexdump`, `hostname`, `id`, `ifdown`, `ifup`,
`lsof`, `man`, `netstat`, `nmap`, `nslookup`, `openssl`, `sha256sum`, `ssh`,
`tar`, `traceroute`, `unzip`, `which`, `xxd`, `zip`.

Being listed is not the same as working: each one is forwarded to a program of
the same name, so it only runs if that program is installed here. `help` says
which are missing rather than letting you find out from a spawn error:

```
  (not installed on this computer: docker ifdown ifup nmap)
```

---

## Worked examples

**Which machine, at a glance:**
```
Nikita@qflipper ~ % ls
Applications  Desktop  Documents  Downloads          <- this Mac

Nikita@qflipper ~ % fls /ext
apps  badusb  dolphin  nfc  subghz                    <- the SD card
```

**Copying between the two machines** (direction comes from the paths, no prefix):
```
Nikita@qflipper ~ % cp ~/Desktop/payload.sub /ext/subghz/
[ copying payload.sub -> Flipper:/ext/subghz/payload.sub ]
[ verified (md5 match) ]
```

**The escape hatch, for anything without its own row:**
```
Nikita@qflipper ~ % host git status
[ host git status -- running on this computer, not the Flipper ]
On branch main
nothing to commit, working tree clean
```

**`sudo` without a cached credential:**
```
Nikita@qflipper ~ % sudo systemctl restart something
[ sudo needs a real terminal to ask for a password, and this panel is not one.
  Run 'sudo -v' in a terminal window to cache your credentials, then 'sudo <command>' works here. ]
```

**History shorthand:**
```
Nikita@qflipper ~ % fls /ext/badusb
hello.txt  test.txt
Nikita@qflipper ~ % !!
fls /ext/badusb
hello.txt  test.txt
```

## Python — the one command worth reading closely

`python3` behaves differently depending on what you give it, because the
Flipper has no Python interpreter of its own — everything runs on this
computer.

- **With a script:** `python3 myscript.py` or `python3 /ext/badusb/script.py`
  runs it and prints the output, same as any other command here. A device
  path is fetched off the SD card first, so a script saved on the card runs
  the same way no matter which computer the Flipper is plugged into.
- **Bare, no script:** `python3` alone opens a **live interactive session** —
  a real REPL, not a one-shot command. Once it's open, every line you type
  goes straight to Python instead of being read as a panel command:
  ```
  Nikita@qflipper ~ % python3
  [ python3 -- running on this computer, not the Flipper. Type exit() or quit() to leave the session. ]
  >>> print(2 + 2)
  4
  >>> exit()
  [ python3 session ended ]
  Nikita@qflipper ~ %
  ```
  Leave it with `exit()`, `quit()`, or Ctrl-C/Esc. Until you do, panel
  commands (`ls`, `cd`, anything else in this document) aren't being parsed —
  they'd just be sent to Python as text and fail as Python syntax, exactly
  like typing a shell command inside a real Python REPL.
