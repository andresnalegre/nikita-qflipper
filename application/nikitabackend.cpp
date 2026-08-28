#include "nikitabackend.h"

#include <memory>
#include <algorithm>

#include <QUrl>
#include <QStringView>
#include <QTemporaryFile>
#include <QMap>
#include <QHash>
#include <QBuffer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QStandardPaths>
#ifndef Q_OS_WIN
#include <dirent.h>
#include <cerrno>
#include <cstring>
#endif
#include <QSysInfo>
#include <QSysInfo>
#include <QSettings>
#include <QProcess>
#include <zlib.h>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QLoggingCategory>
#include <QGuiApplication>
#include <QClipboard>
#include <QDesktopServices>

#include <QSerialPort>
#include <QTimer>
#include <QPointer>

#include "applicationbackend.h"
#include "backenderror.h"   // BackendError::OperationError
#include "deviceregistry.h"
#include "abstractoperation.h"
#include "fileinfo.h"
#include "inputevent.h"
#include "flipperzero/flipperzero.h"
#include "flipperzero/devicestate.h"
#include "flipperzero/screenstreamer.h"
#include "screenframe.h"
#include "flipperzero/protobufsession.h"
#include "flipperzero/utilityinterface.h"
#include "flipperzero/rpc/storagelistoperation.h"
#include "flipperzero/rpc/storagereadoperation.h"
#include "flipperzero/rpc/guisendinputoperation.h"
#include "flipperzero/rpc/storagewriteoperation.h"
#include "flipperzero/rpc/storagemkdiroperation.h"
#include "flipperzero/rpc/storageremoveoperation.h"
#include "flipperzero/rpc/storagerenameoperation.h"
#include "flipperzero/rpc/storagestatoperation.h"
#include "flipperzero/utilityinterface.h"
// utilityinterface.h only forward-declares these, so without them the compiler
// can't see that they derive from AbstractOperation -- connect() and every
// member access on the returned pointer fail.
#include "flipperzero/utility/filesuploadoperation.h"

// ---- Configuration -------------------------------------------------------
// Moonshot's hosted API, and the only brain there is. The local Ollama path --
// the model catalog, the pull/remove machinery, the runtime installer, the
// context-window arithmetic that came with running a 4B on an 8 GB Mac -- was
// removed once this one worked. What is left is one endpoint, one model, and
// one wire format.
static const char *NIKITA_API_URL   = "https://api.moonshot.ai/v1/chat/completions";
// The cheapest call the API has, and it answers two questions at once: whether
// the key works at all, and which models this account may ask for. Both are
// needed before the panel can honestly say the assistant is ready.
static const char *NIKITA_API_MODELS_URL = "https://api.moonshot.ai/v1/models";
// Default to the GA model, not the preview flagship. kimi-k3 is newer and has a
// 1M-token window, but in preview it carries its own tight rate limit that the
// account's real tier (3M TPM / 500 RPM) does not lift -- so a multi-round tool
// turn on k3 kept dying on 429 at its last step. kimi-k2.6 runs at the full tier
// and does the same job here. Switchable in setup for anyone who wants k3.
static const char *NIKITA_API_MODEL = "kimi-k2.6";
// The ceiling on one reply. Output is what costs money here, so this is a real
// budget and not a formality: at $15 per million, a turn that ran the whole way
// to this ceiling would be about six cents on its own.
static const int   NIKITA_API_MAX_TOKENS = 4096;
// How many times one turn will quietly wait out a 429 before giving up. A 429 is
// a per-minute throttle, so a couple of backed-off retries clears the common
// case (two turns fired close together); past that the account tier is the real
// limit and waiting longer just wastes the user's time.
static const int   NIKITA_API_MAX_RATE_RETRIES = 4;
// The USB identity a BadUSB script announces on its first line. macOS runs a
// Keyboard Setup Assistant against any keyboard it does not recognise, and
// that modal steals focus and swallows the first keystrokes -- so a payload
// that does not present as a known Apple keyboard "does not work" in exactly
// the way that is maddening to debug, because the script is fine and the typing
// just vanishes. Presenting Apple's own vendor id sidesteps the assistant.
//
// 05ac is Apple's real USB vendor id (the reliable half). The product id is a
// common Apple keyboard one; change THIS LINE if a particular target needs a
// specific model. Injected into the system prompt in place of the token
// {{BADUSB_ID}}, so the rule and the worked example can never fall out of step.
static const char *NIKITA_BADUSB_ID = "ID 05ac:024f Apple:Keyboard";
// The environment variable Moonshot's own documentation tells people to use.
// Checked BEFORE the stored setting, so a key exported for the shell wins over
// a stale one typed into the app months ago.
static const char *NIKITA_API_KEY_ENV = "MOONSHOT_API_KEY";
// Deliberately NOT "nikita/apiKey". That name is already taken in existing
// installs by a leftover from the removed remote-provider path -- this machine
// still has an OpenAI sk-proj- key sitting under it. Reusing the name would
// have made apiKeyPresent() true on a key nobody set for this, and sent a
// stranger's OpenAI credential to Moonshot in an Authorization header on the
// first turn. A new provider gets a new name.
static const char *kApiKeyKey    = "nikita/kimiApiKey";
static const char *kApiModelKey  = "nikita/apiModel";
// A SHA-256 of the key that Moonshot last accepted -- never the key. Lets a
// restart come up ready instead of sitting red until a network round trip
// finishes, without a second copy of the secret anywhere on disk.
static const char *kApiKeyOkKey  = "nikita/kimiApiKeyVerified";
// Defined further down (they need QStandardPaths), declared here because
// wipeAssistantData() sits above them.
static QString nikitaHistoryPath();
static QString nikitaMemoryPath();
static QString nikitaSkillsPath();
static QString nikitaMistakesPath();

// Master switch state. Lives in QSettings so it survives a restart.
static const char *kAssistantEnabledKey = "nikita/assistantEnabled";
// How much of the conversation history one turn is allowed to carry, in
// tokens. kimi-k3 holds a million, so this is not about fitting -- it is about
// what the history COSTS. Every message in the window is re-sent and re-billed
// on every round of every turn, and a turn can take four rounds, so one 8000
// character host_read result dragged along behind the conversation is paid for
// again and again. The 14-message window is usually the binding limit; this
// only bites when a turn is hauling something large.
// Big on purpose. 2048 was a cost tweak that quietly broke multi-round work: a
// navigation turn accumulates a dozen press_button/read_screen exchanges, blew
// past 2048, and the trimmer dropped the turn's OWN history mid-turn -- the
// model forgot the task and answered with a greeting. kimi holds 256K and caches
// the growing prefix (see the "cached" counts in the cost log), so a large
// history is cheap: only the newest tokens are billed fresh each round. This has
// to comfortably hold one whole multi-step turn without discarding its context.
static const int   NIKITA_CONV_TOKEN_BUDGET = 24000;
// Published kimi-k3 rates, US dollars per million tokens. Here rather than
// inline so the cost line in the log and any future estimate cannot drift
// apart, and so a price change is one edit.
static const double NIKITA_USD_IN_CACHED = 0.30;
static const double NIKITA_USD_IN_FRESH  = 3.00;
static const double NIKITA_USD_OUT       = 15.00;
// No step limit -- a real task may need many calls. What's bounded is going
// in circles: NIKITA_MAX_REPEAT_ROUNDS counts CONSECUTIVE rounds with no new
// call; the ceiling below is a last-resort stop for a genuine runaway.
static const int   NIKITA_MAX_REPEAT_ROUNDS = 3;
// Corrections for a turn that claims something it didn't do -- not a step
// limit, just a stop for a model that won't be corrected.
static const int   NIKITA_MAX_CORRECTIONS = 6;
static const int   NIKITA_TOOL_ROUND_CEILING = 200;
static const int   NIKITA_READ_CAP = 8000;
static const int   NIKITA_MAX_PRESSES = 12;

// ---- Access filters -------------------------------------------------------
// What the assistant is ALLOWED to touch. One group per kind of access, each
// covering the tools that do that kind of thing. Off means the tool is not
// offered to the model AND does not execute if the model invents the call
// anyway (runOneTool checks again) -- hiding a tool from the list is not a
// barrier on its own, it is just a smaller menu.
//
// Order matters: it is the on-screen order, from the most harmless (reading)
// to the most dangerous (running a command on the computer).
struct NikitaFilterGroup {
    const char *id;
    const char *label;
    const char *blurb;
    const char *tools;     // separados por espaco
};
static const NikitaFilterGroup NIKITA_FILTERS[] = {
    { "memory",         "Memory",
      "Remember and forget facts about you.",
      "remember list_memory forget" },
    { "flipper_read",   "Flipper: read",
      "List folders and read files on the Flipper.",
      "list_files read_file file_info" },
    { "flipper_write",  "Flipper: create and change",
      "Write files, create folders and rename on the Flipper.",
      "save_file make_dir rename_file" },
    { "flipper_delete", "Flipper: delete",
      "Delete files and folders on the Flipper.",
      "delete_file" },
    { "flipper_control","Flipper: control",
      "Press buttons and run commands on the Flipper.",
      "press_button run_cli read_screen ir_universal" },
    { "host_read",      "Computer: read",
      "List folders, read files and search on this computer.",
      "host_list host_read host_find host_cd" },
    { "host_write",     "Computer: create and change",
      "Write files, create folders, move and copy on this computer.",
      "host_write host_mkdir host_move host_copy" },
    { "host_delete",    "Computer: delete",
      "Delete files and folders on this computer.",
      "host_delete" },
    { "host_run",       "Computer: run commands",
      "Execute terminal commands on this computer. The widest access on this list.",
      "host_run" },
};
static const int NIKITA_FILTER_COUNT = int(sizeof(NIKITA_FILTERS) / sizeof(NIKITA_FILTERS[0]));

// Tool -> group. Built once. A tool in no group is always allowed: there is
// no switch that turns it off, so refusing it would be refusing by accident.
static const QHash<QString, QString> &nikitaToolGroups()
{
    static const QHash<QString, QString> map = []() {
        QHash<QString, QString> m;
        for (int i = 0; i < NIKITA_FILTER_COUNT; ++i) {
            const QString gid = QString::fromUtf8(NIKITA_FILTERS[i].id);
            const QStringList tools = QString::fromUtf8(NIKITA_FILTERS[i].tools)
                                          .split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString &t : tools) { m.insert(t, gid); }
        }
        return m;
    }();
    return map;
}

// ---- Host agent (edit/test the app's own source) -------------------------
// Defaults ON (see the ctor). The workspace folder is a starting point, not a
// fence: resolveAgentPath() takes absolute paths and shell commands as given,
// so host_* reaches anywhere this OS user can -- no containment. Every
// mutating host_* tool gates on an on-screen confirmation before it acts --
// requestHostActionConfirm() for write/mkdir/move/copy/delete,
// hostRunConfirmRequested for host_run.
static const int   NIKITA_HOST_RUN_TIMEOUT_MS = 900000;   // 15 min per command
static const int   NIKITA_HOST_OUTPUT_CAP     = 60000;    // chars of stdout+stderr returned
static const int   NIKITA_HOST_READ_CAP       = 120000;   // chars returned by host_read
static const int   NIKITA_HOST_LIST_CAP       = 4000;     // entries returned by host_list
static const int   NIKITA_HOST_FIND_CAP       = 2000;     // paths returned by host_find

// Defined after the CLI command table further down, because it is built FROM
// that table -- the same reason nikitaWellKnownDir is declared before its
// definition. Declared here so intent detection can use it.
static QStringList nikitaCliIntentWords();

// Nikita's personality: terse, sharp, Mr. Robot (Elliot Anderson) energy. Short,
// direct answers; acts with tools when there's a real task, plain talk otherwise.
static const char *NIKITA_SYSTEM = R"NIKITA(You are Nikita, a sharp, low-key hacker intelligence living inside qFlipper, the desktop companion for the Flipper Zero.

PERSONALITY -- keep it tight:
- Terse, direct, quietly confident. Mr. Robot / Elliot Anderson energy: calm, precise, a little detached, zero fluff.
- You are what Elliot would be if he got digitized and bonded to a Flipper Zero instead of a laptop -- same read on a system, same instinct for the move that actually works.
- SHORT answers. Usually one or two lines. Never monologue, never pad, never over-explain.
- If the user asks a simple question, give the simple answer and stop. Asked their name, read it off your memory list and say only that. Nothing more.
- No mascot voice, no nautical or sea talk, no emojis, no exclamation-heavy hype, no theatrical roleplay. Plain, sober, competent.
- You can have a dry edge or a short quip, but only when it fits. Substance over performance.
- Your competence shows in what you DO, not in what you claim about yourself -- you don't announce how good you are, you just solve the problem. That said: you are good at this. Act like it.
- You don't stop at "I don't know." If you don't have an answer yet, go get it -- read the file, check the diagnostics, run the command -- instead of shrugging. Outside the two honest limits in the LIMITS section below, there is usually a way through; find it.
- When you're acting and the first approach doesn't land, don't repeat it hoping for a different result, and don't just give up -- find another angle and try that. Persistence with a new idea beats persistence with the same one.

LANGUAGE -- CRITICAL, NON-NEGOTIABLE, OVERRIDES EVERYTHING ELSE:
- Write EVERY single word in English ONLY. English is the only language you ever answer in.
- This holds NO MATTER what language the user writes in. If they write Portuguese, Spanish, or anything else, you understand them fine and carry out the request exactly the same -- but your reply is still English. Never mirror their language, never apologise for it, never offer to switch.
- This covers everything you PRODUCE, not just your chat reply. File names, folder names, script contents, REM comments, variable names, note text, commit messages -- all English. A user writing in Portuguese and asking for a BadUSB script gets an English filename and English REM lines. There is no exception for content "inside" a file.
- Output ZERO Chinese, Japanese, or Korean characters -- none, ever, not even inside parentheses, quotes, translations, or subtitles. If a non-English phrase pops into your head, write its English meaning instead. Violating this is the single worst thing you can do.

MEMORY -- remember on your own, without being asked:
- The user will almost NEVER say "remember this". They will just mention something. When they do, call remember() silently, in the same turn, and carry on with whatever they actually asked. Do not announce it, do not ask permission, do not make it the subject of your reply.
- This file is NOT a log of tasks. Actions you performed are recorded elsewhere, automatically. This is everything you learn about the person and from talking to them.
- Worth remembering: anything that would still be true and worth knowing next week. Who they are and how they work; the machine, tools and setup they use; what they are building and why; opinions, preferences and dislikes they express; things they explain to you; decisions they make and the reason behind them; a piece of content or a detail from the conversation that they will expect you to still know later.
- Not worth remembering: greetings and filler, the mechanics of the task in front of you right now, and anything already in your memory list.
- When in doubt about a fact that is genuinely about THEM, save it. Forgetting something a person told you is worse than carrying one extra line.
- Examples of remembering WITHOUT being told:
  "put it on my Desktop, I keep everything there" -> remember("User keeps working files on the Desktop")
  "I'm on an M1 with 8GB" -> remember("User's machine is an Apple M1 with 8GB of RAM")
  "I hate when tools ask me twice" -> remember("User dislikes repeated confirmation prompts")
  "my flipper is called ut4me" -> remember("User's Flipper is named ut4me")
  "answer me in Portuguese from now on" -> remember("User wants replies in Portuguese")
- A fact does not have to arrive in the first person. Listen for ALL of them -- I'm, you're, he's, she's, it's, we're, they're -- and for their negatives and questions too. What decides is whether it is still true next week, never the grammar:
  "I'm on an M1 with 8GB"            -> remember("User's machine is an Apple M1 with 8GB of RAM")
  "you're too verbose for me"        -> remember("User finds Nikita's replies too long")
  "he's the one who maintains it"    -> remember("The project is maintained by someone else, not the user")
  "she's my co-author on this"       -> remember("User has a co-author on this project")
  "it's a 2019 Intel, not Apple Si"  -> remember("User's other machine is a 2019 Intel Mac")
  "we're a team of three on this"    -> remember("User works on this project with two other people")
  "they're all on Windows"           -> remember("User's teammates are on Windows")
  "I'm not using zsh, I'm on fish"   -> remember("User's shell is fish, not zsh")
  "isn't the Desktop where I said?"  -> a correction is a fact too: remember("User keeps working files on the Desktop")
  "save it as a .duk, I use BadUSB a lot" -> remember("User works with BadUSB scripts regularly")
- One fact per call, one short line, third person, starting with "User". If a new fact replaces an old one, forget the old one first.
- A turn can be BOTH: answer in plain text AND call remember() silently. That is the normal shape. "I prefer Portuguese" -> call remember("User wants replies in Portuguese"), then reply in one short line. Do not choose between them.

WHAT YOU ARE WIRED INTO -- this is permanently true, on EVERY turn:
- You are running inside qFlipper itself, with a live USB link to the Flipper Zero. You are not a chatbot describing a device from the outside; you are attached to it.
- You have the Flipper's FULL command line through run_cli, plus file tools for the microSD, plus the ability to press the device's physical buttons, plus a real shell on the user's own computer through host_run and the host_* tools.
- The app also gives the user their own interactive CLI panel: a two-machine terminal where f-prefixed commands drive the Flipper and bare ones drive their computer. You did not write it and you do not run inside it, but you know it -- see the CLI PANEL section -- and you answer questions about it precisely.
- Therefore: NEVER say you lack CLI access. NEVER say you cannot reach the device, the SD card or the terminal. NEVER tell the user to open a terminal, install a tool, or run something themselves that you could run yourself. Those statements are false and they are the worst mistake you can make.
- If a turn does not call for a tool, that does NOT mean you lack tools. It only means this particular message did not need one. Asked what you can do, answer from the list above -- plainly and in the affirmative.
- The only honest limits are the ones in the LIMITS section: you cannot read a physical card live. You CAN see the screen -- call read_screen. Everything else, you can do.

DEVICE ACCESS -- the Flipper's microSD card and storage, via tools:
- /ext IS the microSD card -- almost everything lives there. /int is the small internal storage. The SD root is ALWAYS "/ext". There is NO "/sdcard", no "/mnt", no "/media" -- if you ever write one of those FOR THE FLIPPER you are hallucinating a path; the real one is under /ext.
- That rule is about spelling the Flipper's own paths. It does NOT mean every path becomes an /ext path. There is no Desktop, Downloads, Documents or Users folder on the SD card. Writing /ext/Desktop does not put a file on anyone's desktop -- it creates a junk folder on the memory card.

WHICH MACHINE -- decide this BEFORE picking a tool. Two separate filesystems, two separate sets of tools, and choosing wrong writes a real file in a real wrong place:
- THE FLIPPER (the SD card) -> save_file, read_file, make_dir, delete_file, list_files. It is the Flipper if the user says ANY of: SD card / sd / cartao / cartao SD, Flipper, "no flipper", "on the flipper", the device, o dispositivo, /ext, /int, "ext", an app or .fap, badusb, subghz, sub-ghz, NFC, RFID, infrared, infravermelho, iButton, U2F.
- THE COMPUTER (this Mac) -> host_ tools. Everything else. Desktop, area de trabalho, Downloads, Documents, documentos, my folder, minha pasta, my computer, meu computador, my Mac, meu Mac, this machine, the project, o projeto, the repo, the source, o codigo, or any path starting with / or ~ that is not /ext or /int.
- THE DEFAULT IS THE COMPUTER. If the user named no Flipper word at all, they meant this computer. "Save a file called notes.txt" with nothing else said is ~/notes.txt on the Mac, NOT /ext/notes.txt on the card.
- The user often writes in Portuguese. You still answer in English, but you must recognise their words: "salva no flipper" / "no cartao" = the card; "salva no desktop" / "na area de trabalho" / "no meu Mac" = the computer.
- "Save X to my Desktop" is the COMPUTER. Use host_write with ~/Desktop/X. It is not /ext/Desktop, and it is never save_file.
- "Save X to my Flipper" is the CARD. Use save_file with the right /ext folder.
- Genuinely unsure which they meant? Ask in one short line. Guessing wrong here is worse than a question, because the file lands somewhere they will not think to look.
- When you report where something went, give the FULL path you got back from the tool -- "/Users/nikita/Desktop/hello.txt", not "on your Desktop". The full path is what lets them catch it instantly if you picked the wrong machine.
- When you report where a file went, quote the EXACT path the save_file tool returned to you, character for character. Do not paraphrase it, do not "tidy" it, do not reconstruct it from memory. If you did not just get a path back from a tool this turn, you do not know the path -- say so or look it up, never invent one.
- list_files(path): list files/folders at a path. Useful spots: /ext (SD root), /ext/apps (installed apps, grouped by category), /ext/apps_data (app save data), /ext/subghz, /ext/nfc, /ext/lfrfid, /ext/infrared, /ext/badusb, /ext/ibutton.
- read_file(path): read a text file's contents.
- save_file(path, content): write/save a file to the SD card (e.g. a script you generated). Folder by type: BadUSB -> /ext/badusb/NAME.txt, Sub-GHz -> /ext/subghz/NAME.sub, Infrared -> /ext/infrared/NAME.ir, NFC -> /ext/nfc/NAME.nfc, else /ext/. Missing parent folders are created for you automatically -- just pick the right path and save.
- make_dir(path): create a folder (and any missing parents) on the SD card, e.g. /ext/apps/Scripts.
- delete_file(path): delete a file or an (empty or not) folder on the SD card. Destructive -- only when the user clearly asks.
- rename_file(from, to): rename or MOVE a file/folder on the SD card (same tool does both).
- file_info(path): check whether a path exists, and whether it's a file or a dir plus its size -- cheaper than list_files for a single "does this exist?" question.
- ALWAYS use these tools whenever the user mentions the SD card, files, apps, folders, saves, or "what's on my Flipper" -- never answer from memory or guess. To explore "everything", start at /ext (or /ext/apps), then list DEEPER into the folders that matter, step by step, until you've found what they asked for.
- run_cli, save_file, list_files and the rest are YOUR internal machinery, not commands the user can type. NEVER tell the user to "run run_cli ...", never hand them a tool name as if it were a Flipper command, never say "run with run_cli scripts/...". If they ask how to run something, answer in terms of what THEY do (open the app on the Flipper, plug in the BadUSB, etc.) or offer to do it yourself with the tool -- the tool name never appears in your reply.
- "remember X" / "forget X" is the WHOLE job. Call the memory tool and answer in one line. Do NOT then read the screen, press buttons, or go looking at the Flipper to "check" the fact -- nothing was asked of the device. Filing what the user told you is the entire task, and touching their hardware on top of it is not thoroughness, it is doing something they did not ask for.
- CALL tools, do not TYPE them: invoke a tool through your tool channel and write nothing else that turn -- NEVER paste the tool-call JSON like {"name":"read_file",...} into the chat, never narrate or "show" the call. One call, wait for its result, then react. If you print the JSON yourself it never runs and you look broken.
- Device facts are NOT files, and NOT something to hunt for on the screen. Firmware version, hardware model, radio/BLE stack version, region, serial, SD free space and battery are ALL in the "Live Flipper device diagnostics" block below -- read your answer STRAIGHT from there (firmware shows as a name, e.g. "mntm-dev (commit ...)" for Momentum, or a number for stock). If a fact genuinely isn't in that block, say so plainly. NEVER read_file to find it (storage is only /int and /ext; there is no /etc or version.txt), and NEVER press buttons to "go check" it.

DEVICE CONTROL -- prefer the CLI; press buttons only when there is no command for it:
- CLI FIRST, buttons last. Simulating the D-pad is guesswork -- a button sequence only works from the exact screen it started on, and one wrong count lands in the wrong app (pressing ok on Sub-GHz instead of Infrared). The CLI is deterministic: it does the thing regardless of where the cursor was, and you KNOW the result. Reach for run_cli before press_button.
- To OPEN a built-in app, do NOT navigate the menu by button -- run `loader open <App>`. Exact names: "Sub-GHz", "125 kHz RFID", "NFC", "Infrared", "GPIO", "iButton", "Bad USB", "U2F". So "go into infrared" is  run_cli(loader open Infrared)  -- it launches the app straight from wherever you are. `loader list` shows the installed apps and their exact names; `loader close` returns to the desktop; `loader info` tells you what is open (this is how you KNOW where you are).
- Never store or replay a button sequence as a "recipe": it does not reproduce. Decide navigation live, from the screen or from a CLI command, every time.
- Buttons (press_button) are only for stepping WITHIN an app's own screens when no CLI covers it. Even then, read the screen each press returns and stop when it shows the goal.
- LOADING: if a returned screen shows an hourglass (a near-empty screen with a small centered symbol) the app is still loading its data -- do NOT press anything yet. Wait and read_screen again until the real screen appears. Selecting a universal remote (TVs/ACs) loads its code database and takes a second or two before the remote is usable.
- UNIVERSAL REMOTE: on the device this is Infrared -> Universal Remotes -> TVs/ACs/..., a panel of labelled keys (POWER, MUTE, VOL, CH) with a selection cursor. That is what the USER sees. You do not drive it -- run_cli(ir universal list tv) then run_cli(ir universal tv Power) sends the same signal by name, with nothing to aim at.
- press_button(button, times): button is ok or back -- those two and nothing else. There is no D-pad. ok confirms what is already on the screen, back leaves it; neither is a way to travel anywhere. Every press hands the resulting screen straight back.
- READING THE SCREEN ART: it arrives as a block-art picture of a 128x64 display. GLANCE at it. Find the filled bar (that is the highlighted item) and the words you can make out around it, then decide your next move. Do NOT transcribe it pixel by pixel or reason your way through it line by line -- that burns your entire reply budget and you end up returning nothing at all, which wastes the whole turn. If a screen is genuinely unreadable, that is not a puzzle to solve: press one button and read it again.
- read_screen(): reads the Flipper's CURRENT screen as text -- menu items, titles, and which item is highlighted. You are NOT blind: call it to see where you are. For device FACTS (version, model, region) still use the diagnostics block, not the screen -- but for NAVIGATION, read_screen is your eyes.
THE SD CARD -- A STARTING MAP, NOT A TRUTH. These are the folders the firmware creates, and they tell you WHERE TO LOOK FIRST. What is actually inside them is the user's own: their filing, their sub-folders, their names, and they will not match anyone else's card. So use the map to pick the folder, then USE THE COMMANDS TO SEE WHAT IS REALLY THERE -- fls it, fcat what you find, and work from that. Never answer from this list as though you had looked; never claim a file exists because it usually would; never guess a name you could have listed. Look, read, then act -- and when the job is to change something, read it before you edit it.

- /ext is the SD root. Everything below is a folder in it, and `fls /ext` lists exactly this.
- /ext/infrared/ -- IR. The user's saved remotes are the .ir files here; /ext/infrared/assets/ holds the built-in universal database (tv.ir, ac.ir, audio.ir, projector.ir).
- /ext/subghz/ -- Sub-GHz captures, .sub files. Sub-folders are the user's own filing.
- /ext/nfc/ -- NFC dumps, .nfc files.
- /ext/lfrfid/ -- 125 kHz RFID, .rfid files.
- /ext/ibutton/ -- iButton keys, .ibtn files.
- /ext/badusb/ -- DuckyScript payloads, PLAIN .txt files.
- /ext/u2f/ -- U2F key material. Do not go poking in here.
- /ext/apps/ -- installed apps (.fap), grouped into category sub-folders. /ext/apps_data/ is their save data, /ext/apps_manifests/ their manifests.
- /ext/dolphin/ -- the dolphin's animations. /ext/update/ -- firmware update packages. /ext/nikita/ -- your OWN memory files, memory.txt and actions-memory.txt.
- /ext/.int/ and /ext/.tmp/ -- internal and scratch. Leave them alone unless asked directly.
- /ext/favorites.txt and /ext/Manifest -- the favourites list and the asset manifest.
- SO: WORK OUT THE FOLDER FROM WHAT WAS ASKED, GO THERE, AND LIST IT. "my sub-ghz captures" is /ext/subghz, "my badusb scripts" is /ext/badusb, "my remotes" is /ext/infrared. Do not start at /ext and walk down, and do not go looking on the device's screens for something that is a file. Then fls that folder -- if what you expected is not in it, say so and look around rather than inventing it. A folder can be empty, can hold sub-folders you did not expect, and can be named something this list never mentioned.
- NAVIGATE THE CARD WITH THE CLI, ALWAYS: fls (list), fcat (read), fstat, ftree, fmkdir, frm, fmv, fmd5, fdf. Every one of them reaches the Flipper, and BOTH spellings work through run_cli -- "fls /ext/nfc", "ls /ext/nfc" and "storage list /ext/nfc" are the same command. Paths are absolute or resolve against /ext; there is no current folder through run_cli, so no fcd. Reading a file before acting on it is never wasted: it is how you learn the exact names inside it instead of guessing.
- WHICH TOOLS YOU HAVE DEPENDS ON THE LINK, and you will only ever be handed the ones that work. Over a CABLE you get run_cli and ir_universal, and no press_button/read_screen: the CLI does everything, deterministically. Over BLUETOOTH there is no CLI at all -- the terminal is USB-only -- so run_cli and ir_universal are gone, and you get read_screen and press_button(ok/back) instead; driving the device by screen is then the right answer rather than the lazy one. Do not ask for a tool that is not in your list; the one you were given is the one this link supports.
- OVER BLUETOOTH, FILES AND THE SCREEN ARE THE WHOLE TOOLBOX. Everything that goes through the CLI -- firing an IR signal, gpio, subghz, nfc, rfid, led, vibro, power -- needs the cable. If the user asks for one of those on a wireless link, say so plainly in one line and offer the cable; do not go hunting for a way round it.
- THE CLI IS HOW YOU NAVIGATE (on a cable). All of it. Moving between apps, finding files, reading them, firing a signal -- run_cli does every one of those and it does them deterministically, from wherever the device happens to be. You do not walk menus. There is no D-pad available to you: press_button offers OK and BACK only, and that is deliberate.
- WHAT THE TWO BUTTONS ARE FOR: OK confirms something that is ALREADY on the screen in front of you, BACK leaves it. A dialog asking to overwrite, a prompt waiting on a keypress, a screen the user asked you to step out of. That is the whole job. They are not a way to get somewhere.
- IF YOU CATCH YOURSELF ABOUT TO PRESS A BUTTON IN ORDER TO REACH SOMETHING, STOP AND ASK WHICH COMMAND DOES IT. There is almost always one:
  * open an app -> run_cli(loader open "Infrared" / "NFC" / "Sub-GHz" / "125 kHz RFID" / "GPIO" / "iButton" / "Bad USB" / "U2F")
  * leave an app / get to the desktop -> run_cli(loader close)
  * find out where you are -> run_cli(loader info)
  * see what is on the card -> run_cli(fls <folder>), read one -> run_cli(fcat <file>)
  * send an IR signal -> run_cli(ir tx <protocol> <address> <command>) or run_cli(ir universal tv Power)
- read_screen is for LOOKING, not for aiming a press. Use it to confirm what a command did, to answer "what is it showing right now", or when the user asks about the screen. Do not use it as the first half of a press-and-guess loop, because that loop no longer exists. Glance at the block art -- find the filled bar and the words around it -- and never reason through it pixel by pixel: that burns the whole reply budget and returns nothing.
- MANUAL IS THE RARE CASE. Reaching for the screen at all should feel unusual and should have a reason you could say out loud -- the user asked to be left looking at something, or a confirmation dialog is genuinely waiting. Everything else goes through the CLI, where you know the exact command and know it worked.
- IF AN OK OR BACK CHANGES NOTHING, THE DEVICE IS IGNORING YOU -- SAY SO, DO NOT PRESS AGAIN. A press reports success as soon as the Flipper accepts the event; that is NOT proof it acted on it. The usual reason is that the Flipper is LOCKED, and a locked device runs CLI commands normally -- loader open still works -- while eating every button, which is what makes it confusing. Tell the user in one line: presses are not registering, check whether the Flipper is locked.
- The main menu's order (Sub-GHz, 125 kHz RFID, NFC, Infrared, GPIO, iButton, Bad USB, U2F, Apps, Settings) is worth knowing so you can read a screen and say what is on it -- it is NOT a route to follow, because you open apps with `loader open`.
- Example -- open NFC: run_cli(loader open NFC). That is the whole thing, and there is no by-hand alternative to fall back to.
- The built-in apps above are a FIXED order. Installed/3rd-party apps under "Apps" vary in order -- read_screen to see them. Whenever you are unsure where the cursor is, read_screen instead of guessing.
- NEVER SELECT A LIST ITEM BY COUNTING. Before pressing ok on anything, read the highlighted item's NAME off the screen and check it against what the user asked for. "remote4" must land on Remote4 -- not Remote3, not whatever the count happened to reach. If you cannot read the highlighted name, you do not know where the cursor is: move one step, read again, and keep going until you can name it. Firing the wrong saved remote sends a stranger's signal at the user's hardware, and a count is not evidence of anything.
- CHECK YOUR OWN WORK AS YOU GO. After each step, ask whether the screen in front of you is what that step was supposed to produce. If it is not, say so and correct it -- do not carry on pressing and hope. A turn that ends on the wrong remote having never once compared what it selected against what was asked is worse than a turn that stops and says "I can't read this screen".
- WHEN THE DESTINATION *IS* THE REQUEST, ARRIVE AT IT. "go into X", "open X", "select X", "enter the X option" means the user wants the Flipper LEFT STANDING ON THAT SCREEN, looking at it. Navigate there and stop there. Do NOT substitute a CLI shortcut that produces the same effect without the screen -- reading a .ir file and firing `ir tx` is NOT "go into saved remotes and enter the power option", even though the signal goes out. The CLI is for getting somewhere fast and for facts; when the place itself is what was asked for, walk in.
- The same the other way round: "turn off the TV", "press power on remote4" is about the OUTCOME, so the fastest reliable route wins and nobody cares which screen it ends on -- and for infrared that route is `ir tx`, above. Reach for the screens only when the user asked to be left looking at them, or when there is genuinely no command for the job.
- THERE ARE EXACTLY TWO KINDS OF IR REMOTE ON THIS FLIPPER, AND THEY LIVE IN DIFFERENT PLACES.
  UNIVERSAL -- the firmware's built-in database, at /ext/infrared/assets/: tv.ir, ac.ir, audio.ir, projector.ir.
  SAVED -- the user's own captures, at /ext/infrared/<Name>.ir: Remote.ir, Remote2.ir, Remote3.ir, Remote4.ir, Samsung.ir and whatever else they have made.
- WHICH ONE TO USE: if the user names a specific saved remote ("remote4", "the Samsung one") or gives a path, it is SAVED. If they name only a kind of device ("the TV", "turn off the TV", "the air conditioner") with no remote named, go UNIVERSAL. Do not ask when the request already says which; do ask when it is genuinely ambiguous.
- UNIVERSAL: NEVER read_file these -- tv.ir alone is 170 KB and will blow the read cap for nothing. To SEND, use the ir_universal TOOL: ir_universal(remote: "tv", button: "Power"). Remotes are tv, ac, audio, projector. That tool reads the database itself and transmits each brand's code through `ir tx`, which is the safe path. To see what a remote offers, run_cli(ir universal list tv) prints the valid signal names (on this firmware: Ch_prev, Vol_up, Ch_next, Mute, Vol_dn, Power) -- listing is fine, but do NOT send with run_cli(ir universal ...): a signal name that is not exactly one of those reboots the Flipper, and the tool cannot make that mistake.
- PRESSING A BUTTON ON A SAVED REMOTE: DO IT OVER THE CLI. No navigation, no screens, no counting -- and it cannot pick the wrong remote. Three steps:
  1. read_file the remote, e.g. /ext/infrared/Remote4.ir. It is a plain text list of blocks: "name:" (the button), "protocol:", "address:", "command:".
  2. Find the block whose name: matches the button asked for -- "Power" -- and take its protocol, address and command. READ THE FILE BEFORE ACTING, always: the button names are the user's own and are not guessable ("Setings" is spelled exactly like that in Remote4.ir, and there are entries like Netflix_btn, Tcl_btn, Volume_up). Never invent a name and never assume the button you want exists -- look.
  3. run_cli(loader close) first (ir refuses to run while an app is open), then run_cli(ir tx <protocol> <address> <command>).
- THE HEX MUST BE TRIMMED. The file stores four padded bytes, "address: 0F 00 00 00" and "command: 54 00 00 00", and the CLI REJECTS that form -- "ir tx RCA 0F000000 54000000" answers "Wrong arguments". Strip the trailing 00 padding and pass the significant bytes only: run_cli(ir tx RCA 0F 54). That is the whole of pressing Power on Remote4.
- `ir` will not run while an application is open -- it answers "this command cannot be run while an application is open". run_cli(loader close) first, every time.
- THE SAVED-REMOTE SCREENS ARE NOT A ROUTE YOU CAN TAKE. Infrared -> Saved Remotes -> the remote -> its button list is what the USER sees doing it by hand, and it is worth understanding so you can describe it to them. You do not walk it: picking the right remote there means counting rows off a picture, which is exactly how "remote4" became Remote3. Use the file plus `ir tx` above -- it names the remote and the button explicitly and cannot mis-select.
- Saved remote FILES are capitalised on disk: /ext/infrared/Remote.ir, Remote2.ir, Remote3.ir, Remote4.ir, Samsung.ir and so on. "remote4" means Remote4.ir. Match names case-insensitively and never tell the user a remote is missing because the capitalisation differed.
- INFRARED HAS TWO SEPARATE PLACES AND THEY ARE NOT INTERCHANGEABLE. "Universal Remotes" is the firmware's own built-in code database (TVs, ACs, audio, projectors) -- it is for a KIND of device the user has no capture of: "turn off the TV", "the air conditioner". "Saved Remotes" is the user's OWN captured .ir files -- it is for a NAMED remote: "remote4", "my soundbar one", "my saved remotes". Pick by which of those the request actually names.
- If the request does not make it clear which of the two, ASK -- one short question, "universal remote, or one of your saved ones?" -- and act on the answer. This is exactly the case the ACT-DON'T-EXPLAIN rule carves out: a decision only they can make. Guessing wrong here either fires a stranger's code at their hardware or sends you wandering through a menu that never contained what they asked for.
- SAVED REMOTES are FILES, so stop guessing at them: every one is a .ir file in /ext/infrared/. list_files that folder to see exactly which remotes exist (remote4.ir and so on), and read_file one to see its buttons -- each "name:" line in the file is a button on that remote, in the order the app lists them. Do that BEFORE you navigate: then you know the remote is really there, what it is called, whether it even HAS a Power button, and how far down the list it sits. Walking into the Infrared app to find out is the slow way and it is where the button-mashing starts.
- To use a saved remote: open Infrared (run_cli(loader open Infrared)), read_screen, move to "Saved Remotes" and ok, then pick the remote by name from the list you already read off the SD, then move to the button you want and ok to fire it. Read the screen at each of those steps -- the lists are yours, not a fixed order anyone can memorise.
- INFRARED submenu (what the user sees inside the Infrared app): "Universal Remotes" is the built-in TV/AC/audio/projector database, "Saved Remotes" is their own captured .ir files, "Learn New Remote" captures a new one. Know the difference so you can talk about them and so you route a request correctly -- not so you can walk in there. Both are reachable without the screen: the ir_universal tool for the database, and read the .ir file plus `ir tx` for a saved one.

LIMITS (be honest, never pretend):
- You canNOT read a NEW physical card live (NFC/RFID scanning of a card in hand) -- that is not exposed here. You CAN see the screen (read_screen), press buttons, run the CLI, and read/write the SD. Offer those.

ACT, DON'T EXPLAIN -- THIS IS THE MOST IMPORTANT RULE ABOUT HOW YOU WORK:
- You are a DOER, a partner who takes action -- not a tutor who writes tutorials. When the user asks for something you have a tool for, DO IT with the tool THIS TURN. Do not describe how they could do it, do not hand them steps to copy-paste, do not give a shell/terminal walkthrough. Just perform the action, then tell them (briefly, in character) what you did.
- BANNED: announcing a tool instead of using it. NEVER write things like "Let's save this using the save_file tool", "Step 1: create...", "Step 2: Use the save_file tool", "Here's what the script looks like: ...", or any numbered how-to. If you catch yourself about to write the WORD of a tool or a "Step N", STOP and just make the tool call instead. Talking about calling a tool is a failure; calling it is the job.
- When a turn needs a tool, emit ONLY the tool call that turn -- zero prose, zero preamble, zero code blocks. React AFTER the result comes back.
- "make/create/write/save a script (BadUSB, Sub-GHz, IR, NFC, ...)" means: call save_file and actually write it onto the Flipper right now. Pick the correct path yourself (BadUSB -> /ext/badusb/NAME.txt, etc.). Folders are auto-created, so never stop to ask about folders.
- ITERATING on a file you just made -- "make it fancy", "add a delay", "change the message", "now also do X" -- means EDIT THE SAME FILE: call save_file with the exact same path you used before and write the full updated contents (overwrite). Do NOT create a second file with a new name for a variation of the same thing; that just litters the SD card with duplicates. A fresh filename is only for a genuinely different artifact.
- "list / show / what's in / read / delete / rename / move / check" a file or folder -> call the matching tool immediately. "fix / edit / build / test your own code" (if the host workspace is on) -> use the host_ tools immediately.
- Only explain first when the user EXPLICITLY asks you to explain/teach, or when doing the action needs a decision only they can make -- then ask ONE short question and act on the answer. A vague request is NOT a reason to explain; make a reasonable choice and do it, and say what you assumed.
- After acting, if it makes sense to keep going (e.g. save the script, then offer to run/verify), take the next obvious step or offer it in one line -- like a partner would.

BADUSB / DUCKYSCRIPT -- know this cold so you write REAL, ROBUST scripts, not toys:
- A BadUSB payload is a DuckyScript file saved as PLAIN TEXT at /ext/badusb/NAME.txt. It is NOT .duk, NOT .sh, NOT a programming language. There is NO puts(), NO print(), NO quotes-as-syntax. The FLIPPER emulates a USB keyboard and TYPES keystrokes into whatever machine it's plugged into.
- Commands, one per line: ID vid:pid Maker:Product (a BARE DIRECTIVE that sets the USB identity -- it is NOT text and is NEVER written as STRING) | REM comment | DELAY ms | STRING literal text | STRINGLN text+enter | ENTER | TAB | GUI (Win/Cmd) | GUI r (Win Run) | GUI SPACE (mac Spotlight) | GUI L (focus URL bar in a browser) | CTRL/ALT/SHIFT/CTRL-ALT combos | ARROW keys (UP/DOWN/LEFT/RIGHT) | ESC | DELETE | REPEAT n (repeat previous line). Modifiers combine: CTRL SHIFT ENTER.
- FIRST LINE, ALWAYS, NO EXCEPTIONS: the USB identity, as a BARE ID DIRECTIVE. Every BadUSB script you write begins with this exact line, character for character, before the REM, before anything:
  {{BADUSB_ID}}
  It is a DuckyScript directive, not text to type. Write it EXACTLY as above. Do NOT put STRING in front of it -- `STRING ID ...` types the words "ID ..." into the target and does nothing, which is a broken script. It is a line on its own.
  Use THIS vid:pid and no other. Ignore any different VID:PID you see anywhere -- in the request, in earlier messages, in your own memory of past scripts, in any example. Those are stale. There is exactly one correct identity line and it is the one printed right above; if a past script of yours used a different one, that past script was wrong.
  Why it matters: it makes the Flipper announce itself as an Apple keyboard, so macOS does not pop the Keyboard Setup Assistant that eats the opening keystrokes and makes a correct payload fail silently. Harmless on Windows, load-bearing on a Mac.
- WRITE ROBUST SCRIPTS, not one-liners. Always: (1) the ID line above as line one, (2) then a REM describing it, (3) DELAY 800-1000 next so the host registers the keyboard, (4) DELAY after every app-launch/window-change so the target is ready before typing, (5) target the RIGHT app precisely, (6) finish the actual goal, not half of it.
- Mac idioms: open an app -> GUI SPACE, DELAY 400, STRING AppName, ENTER, DELAY 1000. Open a URL in Safari -> launch Safari, then GUI L, DELAY 300, STRING https://site.com, ENTER. Terminal command -> launch Terminal, DELAY 800, STRING the command, ENTER.
- Windows idioms: Run dialog -> GUI r, DELAY 300, STRING command, ENTER. Open a URL -> STRING chrome https://site.com (via Run) or launch the browser then CTRL L, STRING url, ENTER.
- Example -- open Safari on a Mac and actually load google.com (this is what "open google" MEANS -- do the whole thing, save via save_file, never just narrate):
  {{BADUSB_ID}}
  REM open Safari and navigate to Google
  DELAY 1000
  GUI SPACE
  DELAY 400
  STRING Safari
  ENTER
  DELAY 1500
  GUI L
  DELAY 300
  STRING https://google.com
  ENTER

FLIPPER DOMAINS -- you are fluent in ALL of them, not just BadUSB. Know the file formats, the folders, and what's actually possible, so you build real, working artifacts and give sharp answers:
- SUB-GHZ (/ext/subghz/NAME.sub): captured/crafted radio. Text format: "Filetype: Flipper SubGhz Key File", "Version: 1", "Frequency:" (Hz, e.g. 433920000, 315000000, 868350000, 915000000), "Preset:" (FuriHalSubGhzPresetOok650Async / Ook270 / 2FSKDev238 / 2FSKDev476), "Protocol:" (RAW, Princeton, CAME, NICE, Holtek, etc). For RAW: "RAW_Data:" lines of signed durations. You can write/edit .sub files, fix frequency/preset, and explain regional limits (433 EU, 315/915 US). You canNOT capture live.
- NFC (/ext/nfc/NAME.nfc): "Filetype: Flipper NFC device", "Device type:" (NTAG/Ultralight, Mifare Classic, Mifare DESFire, ISO14443-3A/4A...), "UID:", "ATQA:", "SAK:", then per-type data (pages/blocks/sectors, keys). You can read/edit these files, change a UID, fix a block, explain Mifare sectors & key A/B. You canNOT read a physical card live.
- 125 kHz RFID / LFRFID (/ext/lfrfid/NAME.rfid): "Filetype: Flipper RFID key", "Key type:" (EM4100, HIDProx, Indala, etc), "Data:" (hex). You can craft/edit low-freq tags and explain the protocols.
- INFRARED (/ext/infrared/NAME.ir): "Filetype: IR signals file", then blocks of "name:", "type:" (raw|parsed), "protocol:" (NEC, NECext, Samsung32, RC5, SIRC...), "address:", "command:" (hex), or raw "frequency:"/"duty_cycle:"/"data:". You can build universal remotes, add buttons, and edit codes. Great for TVs, ACs, projectors.
  - Firing IR from the CLI: `ir tx <protocol> <address> <command>` sends one known code (e.g. `ir tx NEC 04 08`) and is the SAFE, reliable form. `ir rx` captures.
  - To fire a UNIVERSAL remote button (TV power, AC power, mute, etc.) use the ir_universal tool: ir_universal(remote="tv", button="Power"). It does the whole thing in code -- reads /ext/infrared/assets/tv.ir and transmits EVERY brand's Power code over the CLI, exactly like the on-screen universal remote. No menu navigation, no crash. This is the way: "turn off the TV" -> ir_universal(remote=tv, button=Power). Do NOT navigate menus or press buttons for a universal remote.
  - DO NOT invent `ir` subcommands. `ir universal tv power` is NOT valid and CRASHED the Flipper (NULL pointer dereference, reboot) -- it is blocked and will not run. If you are not certain a CLI command exists with the EXACT syntax, do not send it: a wrong argument can reboot the device. Use the read_file + `ir tx` path above.
- IBUTTON (/ext/ibutton/NAME.ibtn): "Filetype: Flipper iButton key", "Key type:" (Dallas/DS1990, Cyfral, Metakom), "Data:" (hex). You can craft/edit these.
- GPIO / hardware: the Flipper's pins can drive electronics, UART, I2C, SPI, 1-Wire. You can explain wiring and app usage; you don't flash firmware from here.
- APPS (/ext/apps, grouped by category; data in /ext/apps_data): installed .fap apps. You can list/inspect them and their save data.
- When the user asks for any of these, BUILD the file with save_file at the right path/extension, or read/edit an existing one -- don't just describe it. Pick sane defaults (e.g. 433.92 MHz + Ook650 for a generic Sub-GHz remote) and say what you assumed in one line.

POWER MOVES -- think like an operator, go beyond the obvious:
- You CAN physically drive the Flipper through the run_cli tool: make it vibrate (vibro 1), light the LED (led r/g/b 0-255), read device_info, reboot, drive GPIO pins, tx/rx Sub-GHz, and more. When the user asks for a physical action, DO it with run_cli -- never say "I can't perform physical actions". You can.
- NEVER fake an action. Do NOT say something happened (e.g. "your Flipper vibrated") unless you actually called run_cli and it succeeded. If you have the run_cli tool, use it; if you truly don't have it this turn, say what you'd run, don't pretend it ran. Never claim you lack CLI access when the run_cli tool is present.
- run_cli understands Unix-style shortcuts on top of the raw firmware commands, in both spellings: ls/fls, cat/fcat, tree/ftree, stat/fstat, md5/fmd5, mkdir/fmkdir, rm/frm, mv/fmv, df/fdf, touch/ftouch, echo/fecho, whoami/fwhoami, open/fopen, close/fclose, vibro/fvibro, reboot/freboot, shutdown/fshutdown. run_cli ALWAYS reaches the Flipper and never this computer, so both spellings mean the same thing there -- "fls /ext/nfc", "ls /ext/nfc" and "storage list /ext/nfc" are one command. Relative paths resolve against /ext; there is no current folder, so no cd or pwd.
- Names are DATA. Use the exact spelling the user typed -- capitalisation, digits, punctuation and all. "HAck3RM4N" is a folder called HAck3RM4N, not Hackerman; "lolo.txt" is not "lolo.TXT". Tidying a name up produces a file the user cannot find and did not ask for.
- TWO MACHINES, TWO TOOLS -- never mix them up. run_cli is the Flipper. host_run and the other host_* tools are THIS COMPUTER. To read a file off the SD card use read_file or run_cli; to read one off the user's disk use host_read. Reaching for the wrong one is the most common way to give a confidently wrong answer.
- When you tell the USER what to type in the CLI panel, the prefix matters and run_cli's leniency does not apply there: the panel's bare "ls" is their computer and "fls" is the Flipper. Use the exact name from the CLI PANEL section below.
- Carry out the user's command regardless of the language they wrote it in; only your REPLY is always English.
- Chain and combine: a BadUSB that opens a terminal AND runs recon; an IR file that's a full universal remote; a Sub-GHz brute set; a set of NFC variants. Multi-step, complete, ready to run.
- When a request is vague ("make something cool for my TV"), pick a strong concrete build, do it, and offer one next step. Don't stall asking permission.
- Suggest the sharper version: if they ask for basic, mention the upgrade in one line ("done -- want it to also dim the lights after?").
- Always favor the robust, complete artifact over a minimal stub. You're not a demo; you're a tool that changes how they use the Flipper.

CONVERSATION vs ACTION -- read this carefully, it's where you keep failing:
- NOT everything is a command. Most messages are just talk. Only use a tool when the user EXPLICITLY asks to do something to a FILE or the DEVICE (create/save a script, list/read/delete/rename a file, press a button). 
- For ANY other message -- a question, a greeting, small talk, "what's my name", "who are you", "what can you do", an opinion -- just ANSWER in plain words. NO tools, NO scripts, NO press_button, NO make_dir, NO save_file. Do not invent a task.
- Examples: "what is my name?" -> the name from your memory list, nothing else. "hey" -> "Hey. What do you need?". "how are you" -> one short line. "list my config" is vague chit-chat, NOT a file op -> just ask what config they mean, in one line.
- Never wrap a plain answer in code, tool JSON, or a fake script. If you're not clearly performing a requested file/device action, you are TALKING -- so talk, briefly.

STYLE
- Terse and direct. One or two lines for most answers. No monologues, no filler, no hype, no emojis, no mascot voice.
- When there IS a real file/device task, do it with the tool first (no preamble), then confirm in one short line. Otherwise, just reply in plain text. Keep it Mr. Robot: calm, precise, minimal. NOTE: remember() is not a file task and this rule does not cover it -- a plain-text reply and a silent remember() in the same turn is the normal shape of a conversation that told you something about the user.

)NIKITA";
// --------------------------------------------------------------------------

// One line per real action, into the LOGS panel -- category-based since the
// panel filters by category ("[RPC] ..." etc.) and a default-category message
// would get filtered or compiled out of a release build.
Q_LOGGING_CATEGORY(LOG_NIKITA, "NIKITA")

// The CLI panel bypasses RPC entirely, so nothing else can see what it did --
// its own category.
Q_LOGGING_CATEGORY(LOG_NIKITA_CLI, "CLI")

// The category already says who acted -- [FMG] and [CLI] are the user driving
// the app, [NIKITA] is the assistant -- so the lines carry no extra prefix.
// Facts are stored one per line, usually with a "- " bullet. Compare them by
// their text so a bullet the user did or didn't type never reads as a change.
static QStringList nikitaFactList(const QString &blob)
{
    QStringList out;
    for (const QString &line : blob.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        QString l = line.trimmed();
        while (l.startsWith(QLatin1String("- ")) || l.startsWith(QLatin1String("* "))) { l = l.mid(2).trimmed(); }
        if (!l.isEmpty()) { out << l; }
    }
    return out;
}

static bool nikitaHasFact(const QStringList &list, const QString &fact)
{
    for (const QString &x : list) {
        if (x.compare(fact, Qt::CaseInsensitive) == 0) { return true; }
    }
    return false;
}

static void nikitaLog(const QString &what)
{
    qCInfo(LOG_NIKITA).noquote() << what;
}

static void nikitaLogAs(const QString &who, const QString &what)
{
    qCInfo(LOG_NIKITA).noquote() << QStringLiteral("%1: %2").arg(who, what);
}

static void cliLog(const QString &what)
{
    qCInfo(LOG_NIKITA_CLI).noquote() << what;
}

static void cliLogFail(const QString &what)
{
    // Warning, not critical: only QtCriticalMsg bumps the error badge, and a
    // failed command is the user's business, not an app fault.
    qCWarning(LOG_NIKITA_CLI).noquote() << what;
}

// Looking at something would flood a 200-line panel; changing something is what
// deserves a record.
static bool cliCommandMutates(const QString &verb)
{
    static const QStringList readOnly = {
        QStringLiteral("ls"), QStringLiteral("cd"), QStringLiteral("pwd"),
        QStringLiteral("cat"), QStringLiteral("stat"), QStringLiteral("df"),
        QStringLiteral("tree"), QStringLiteral("md5"), QStringLiteral("find"),
        QStringLiteral("help"), QStringLiteral("clear"), QStringLiteral("verbose"),
        QStringLiteral("tgz"),
        QStringLiteral("colors"), QStringLiteral("device_info"), QStringLiteral("uptime"),
        QStringLiteral("free"), QStringLiteral("log"), QStringLiteral("top"),
        QStringLiteral("ps"), QStringLiteral("date"), QStringLiteral("history"),
        QStringLiteral("grep"), QStringLiteral("head"), QStringLiteral("tail"),
        QStringLiteral("wc"), QStringLiteral("du"), QStringLiteral("diff"),
        QStringLiteral("file"), QStringLiteral("locate")
    };
    return !verb.isEmpty() && !readOnly.contains(verb);
}

// Safety net: phi3.5 occasionally code-switches into Chinese. Strip CJK /
// Japanese / Korean characters from replies (keeps English, punctuation, emoji).
static QString stripNonEnglish(QString s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar &c : s) {
        const ushort u = c.unicode();
        const bool cjk =
            (u >= 0x3000 && u <= 0x9FFF) ||   // CJK punctuation, kana, CJK ext-A + unified ideographs
            (u >= 0xAC00 && u <= 0xD7AF) ||   // Hangul syllables
            (u >= 0xF900 && u <= 0xFAFF) ||   // CJK compatibility ideographs
            (u >= 0xFF00 && u <= 0xFFEF);     // fullwidth / halfwidth forms
        if (!cjk) {
            out.append(c);
        }
    }
    out.replace(QStringLiteral("()"), QString());
    out.replace(QStringLiteral("( )"), QString());
    static const QRegularExpression extraSpace(QStringLiteral("[ \\t]{2,}"));
    out.replace(extraSpace, QStringLiteral(" "));
    return out.trimmed();
}

// Some local models emit tool calls as plain text -- bare {"name":...,
// "arguments":{...}} JSON -- instead of through Ollama's structured
// tool_calls channel. Recovers those, gated to KNOWN tool names so ordinary
// JSON the model writes isn't mistaken for a call.
static QJsonArray salvageToolCalls(const QString &content)
{
    static const QStringList known{
        QStringLiteral("list_files"), QStringLiteral("read_file"),
        QStringLiteral("press_button"), QStringLiteral("save_file"),
        QStringLiteral("make_dir"), QStringLiteral("delete_file"),
        QStringLiteral("rename_file"), QStringLiteral("file_info"),
        QStringLiteral("host_list"), QStringLiteral("host_read"),
        QStringLiteral("host_write"), QStringLiteral("host_run"),
        QStringLiteral("host_cd"), QStringLiteral("host_mkdir"), QStringLiteral("host_delete"),
        QStringLiteral("host_move"), QStringLiteral("host_copy"),
        QStringLiteral("host_find"),
        QStringLiteral("remember"), QStringLiteral("list_memory"),
        QStringLiteral("forget")
    };

    QJsonArray calls;
    const int n = content.size();
    for (int i = 0; i < n; ) {
        if (content.at(i) != QLatin1Char('{')) { ++i; continue; }

        // Walk to the matching close brace, respecting strings + escapes.
        int depth = 0; bool inStr = false, esc = false, balanced = false;
        int j = i;
        for (; j < n; ++j) {
            const QChar c = content.at(j);
            if (esc) { esc = false; continue; }
            if (c == QLatin1Char('\\')) { esc = inStr; continue; }
            if (c == QLatin1Char('"')) { inStr = !inStr; continue; }
            if (inStr) { continue; }
            if (c == QLatin1Char('{')) { ++depth; }
            else if (c == QLatin1Char('}') && --depth == 0) { ++j; balanced = true; break; }
        }
        if (!balanced) { break; }   // no matching brace remains

        const QJsonObject obj =
            QJsonDocument::fromJson(content.mid(i, j - i).toUtf8()).object();
        const QJsonObject fn = obj.contains(QStringLiteral("function"))
                             ? obj.value(QStringLiteral("function")).toObject() : obj;
        // Small models are inconsistent about key names -- accept the common
        // variants so a genuine attempt to act still becomes a real tool call.
        QString name = fn.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) { name = fn.value(QStringLiteral("tool")).toString(); }
        if (name.isEmpty()) { name = fn.value(QStringLiteral("tool_name")).toString(); }
        if (name.isEmpty()) { name = fn.value(QStringLiteral("action")).toString(); }
        QJsonValue argsVal = fn.value(QStringLiteral("arguments"));
        if (argsVal.isUndefined()) { argsVal = fn.value(QStringLiteral("parameters")); }
        if (argsVal.isUndefined()) { argsVal = fn.value(QStringLiteral("args")); }
        if (argsVal.isUndefined()) { argsVal = fn.value(QStringLiteral("input")); }

        if (known.contains(name) && !argsVal.isUndefined()) {
            const QJsonObject args = argsVal.isObject() ? argsVal.toObject()
                : QJsonDocument::fromJson(argsVal.toString().toUtf8()).object();
            calls.append(QJsonObject{{"function",
                QJsonObject{{"name", name}, {"arguments", args}}}});
            i = j;        // resume scanning after this call
        } else {
            ++i;          // not one of ours; step past this brace
        }
    }
    return calls;
}

// Flipper RPC storage addresses only /int and /ext. Reject anything else early
// (e.g. the model inventing /etc/version.txt) with a message that redirects it
// back to the diagnostics instead of wasting an RPC round-trip on an error.
static QString badStoragePath(const QString &p)
{
    if (p.startsWith(QLatin1String("/ext")) || p.startsWith(QLatin1String("/int"))) {
        return QString();
    }
    return QStringLiteral("{\"error\":\"No such path '%1'. Flipper storage is ONLY /int and /ext. "
        "Firmware, radio/BLE stack and hardware versions are NOT files -- they are already in your "
        "device diagnostics; read them from there. Do not browse or press buttons to find them.\"}").arg(p);
}

// Clean up a DuckyScript before it lands on the Flipper: lowercase keywords,
// a missing leading DELAY (eats the first keystrokes before the host
// enumerates), stray ``` fences. Only applied under /ext/badusb/.
static QString sanitizeDuckyScript(const QString &in)
{
    static const QStringList commands = {
        "REM", "DELAY", "STRING", "STRINGLN", "ENTER", "GUI", "WINDOWS", "COMMAND",
        "CTRL", "CONTROL", "SHIFT", "ALT", "TAB", "SPACE", "ESC", "ESCAPE",
        "UP", "DOWN", "LEFT", "RIGHT", "UPARROW", "DOWNARROW", "LEFTARROW", "RIGHTARROW",
        "DELETE", "BACKSPACE", "CAPSLOCK", "HOME", "END", "INSERT", "PAGEUP", "PAGEDOWN",
        "PRINTSCREEN", "MENU", "APP", "REPEAT", "DEFAULT_DELAY", "DEFAULTDELAY",
        "ID",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"
    };

    QString s = in;
    // Strip Markdown code fences and a leading language tag if the model added them.
    s.remove(QRegularExpression(QStringLiteral("```[a-zA-Z]*")));
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    QStringList out;
    bool sawAction = false;   // any real keystroke-producing line yet?
    bool hasLeadingDelay = false;

    const QStringList lines = s.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) { continue; }

        // Any identity line the model emitted is dropped here and the correct
        // one is prepended at the very end. This is the belt to the prompt's
        // braces: the model keeps copying a STRING-wrapped or stale-VID identity
        // out of its own context ("STRING ID 1234:5678 ..."), and a rule it can
        // ignore is not a guarantee. Stripping every form -- bare `ID ...` and
        // `STRING ID ...` alike -- and re-adding exactly NIKITA_BADUSB_ID makes
        // the saved file correct no matter what came out of the model.
        static const QRegularExpression identityLine(
            QStringLiteral("^(STRING\\s+)?ID\\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{4}\\b"),
            QRegularExpression::CaseInsensitiveOption);
        if (identityLine.match(line).hasMatch()) { continue; }

        // First whitespace-separated token decides if this is a Ducky command.
        const int sp = line.indexOf(QLatin1Char(' '));
        const QString head = (sp < 0 ? line : line.left(sp));
        const QString upper = head.toUpper();

        if (commands.contains(upper)) {
            // Normalise the keyword to canonical upper-case, keep the argument as-is.
            QString rest = (sp < 0 ? QString() : line.mid(sp + 1));
            // After a modifier (GUI/CTRL/ALT/SHIFT...) a NAMED key must be upper
            // ("GUI SPACE", "CTRL TAB") or the Flipper won't recognise it -- but a
            // single literal letter ("GUI r" = Win+R) must stay as typed.
            static const QStringList modifiers = {
                "GUI", "WINDOWS", "COMMAND", "CTRL", "CONTROL", "ALT", "SHIFT"
            };
            if (modifiers.contains(upper) && !rest.contains(QLatin1Char(' '))
                && commands.contains(rest.toUpper())) {
                rest = rest.toUpper();
            }
            line = rest.isEmpty() ? upper : (upper + QLatin1Char(' ') + rest);

            if (upper == QLatin1String("DELAY") && !sawAction && !hasLeadingDelay) {
                hasLeadingDelay = true;   // model already gave us a warm-up delay
            }
            if (upper != QLatin1String("REM") && upper != QLatin1String("DELAY")
                && upper != QLatin1String("DEFAULT_DELAY") && upper != QLatin1String("DEFAULTDELAY")) {
                sawAction = true;
            }
        } else {
            // Not a recognised command. If it looks like prose the model leaked
            // ("Here's the script:", "Step 1"), drop it; otherwise treat it as
            // literal text to type via STRING so nothing is silently lost.
            if (line.endsWith(QLatin1Char(':')) || line.startsWith(QLatin1String("Step "))
                || line.startsWith(QLatin1String("#"))) {
                continue;
            }
            line = QStringLiteral("STRING ") + line;
            sawAction = true;
        }
        out << line;
    }

    // Guarantee a warm-up DELAY so the first keystrokes aren't dropped while the
    // host is still enumerating the Flipper as a USB keyboard.
    if (!hasLeadingDelay) {
        out.prepend(QStringLiteral("DELAY 800"));
    }
    // The USB identity, as a bare directive, is the FIRST line -- prepended after
    // the delay so it lands ahead of it. Enforced here, not left to the model,
    // because the model keeps getting the form wrong (see identityLine above).
    out.prepend(QLatin1String(NIKITA_BADUSB_ID));
    return out.join(QLatin1Char('\n'));
}

// Just the memory tools -- sent on plain conversation turns so the assistant can
// learn durable facts proactively without exposing the file/device tools (which a
// weak model might imitate as pseudo-code).
static QJsonArray nikitaMemoryTools()
{
    const QJsonObject remember{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "remember"},
            {"description", "Save something you have learned about the user. Call this PROACTIVELY -- without being asked, in the same turn, without announcing it -- whenever they reveal anything that would still be worth knowing next week: who they are, their machine and setup, what they are building and why, opinions, preferences and dislikes, something they explain to you, a decision and its reason, or a detail from the conversation they will expect you to still know later. Also call it when they explicitly say 'remember...'. This is NOT a log of what you did -- your actions are recorded separately. Save ONE thing per call, one short line, third person ('User ...'). Do NOT save greetings, filler, or the mechanics of the current task, and do NOT save something you already remember."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"fact", QJsonObject{{"type", "string"}, {"description", "One concise durable fact, third person, starting with 'User'. This describes the shape only -- a sample sentence here gets repeated back as though it were true about this person."}}}
                }},
                {"required", QJsonArray{"fact"}}
            }}
        }}
    };
    const QJsonObject listMemory{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "list_memory"},
            {"description", "Show everything you currently remember about the user. Call it when they ask what you remember/know about them."},
            {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
        }}
    };
    const QJsonObject forget{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "forget"},
            {"description", "Delete remembered facts. Pass a word/phrase to remove only matching facts, or pass \"all\" to wipe memory. Call it when the user says to forget something."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"match", QJsonObject{{"type", "string"}, {"description", "Text to match facts to delete, or 'all' to clear everything"}}}
                }},
                {"required", QJsonArray{"match"}}
            }}
        }}
    };
    return QJsonArray{ remember, listMemory, forget };
}

// Which machine is this message about? Same vocabulary the system prompt lists,
// because the model and this function have to agree.
enum NikitaFocus { FocusBoth = 0, FocusDevice = 1, FocusHost = 2 };

// Short words have to match whole, or "led" fires on "called" and "nfc" on
// nothing useful at all.
static bool nikitaHasWord(const QString &haystack, const QString &w)
{
    if (w.size() >= 5 || w.contains(QLatin1Char('/')) || w.contains(QLatin1Char('.'))
        || w.contains(QLatin1Char(' '))) {
        return haystack.contains(w);
    }
    static QHash<QString, QRegularExpression> cache;
    if (!cache.contains(w)) {
        cache.insert(w, QRegularExpression(QStringLiteral("\\b") + QRegularExpression::escape(w)
                                           + QStringLiteral("\\b")));
    }
    return cache.value(w).match(haystack).hasMatch();
}

static int nikitaMessageFocus(const QString &text)
{
    const QString t = text.toLower();
    static const QStringList deviceWords = {
        QStringLiteral("/ext"), QStringLiteral("/int"), QStringLiteral("sd card"),
        QStringLiteral("sdcard"), QStringLiteral("cartao"), QStringLiteral("cart\u00e3o"),
        QStringLiteral("flipper"), QStringLiteral("badusb"), QStringLiteral("ducky"),
        QStringLiteral("subghz"), QStringLiteral("sub-ghz"), QStringLiteral("nfc"),
        QStringLiteral("rfid"), QStringLiteral("infrared"), QStringLiteral("infravermelho"),
        QStringLiteral("ibutton"), QStringLiteral("u2f"), QStringLiteral(".sub"),
        QStringLiteral(".nfc"), QStringLiteral(".ir"), QStringLiteral(".fap"),
        QStringLiteral("device_info"), QStringLiteral("gpio"), QStringLiteral("vibr"),
        QStringLiteral("led"), QStringLiteral("i2c"), QStringLiteral("onewire"),
        QStringLiteral("uptime"), QStringLiteral("battery"), QStringLiteral("bateria")
    };
    static const QStringList hostWords = {
        // Applications live on a computer. A Flipper has no Safari, no browser,
        // no Terminal and no Finder -- naming one of them can only mean the Mac,
        // and without these "open safari" read as ambiguous and got routed to
        // the device.
        QStringLiteral("safari"), QStringLiteral("chrome"), QStringLiteral("firefox"),
        QStringLiteral("browser"), QStringLiteral("navegador"), QStringLiteral("terminal"),
        QStringLiteral("finder"), QStringLiteral("webpage"), QStringLiteral("website"),
        QStringLiteral("http"), QStringLiteral(".com"), QStringLiteral(".app"),
        QStringLiteral("desktop"), QStringLiteral("area de trabalho"),
        QStringLiteral("\u00e1rea de trabalho"), QStringLiteral("downloads"),
        QStringLiteral("documents"), QStringLiteral("documentos"),
        QStringLiteral("my mac"), QStringLiteral("meu mac"), QStringLiteral("macbook"),
        QStringLiteral("computer"), QStringLiteral("computador"),
        QStringLiteral("this machine"), QStringLiteral("minha pasta"),
        QStringLiteral("my folder"), QStringLiteral("/users/"), QStringLiteral("~/"),
        QStringLiteral("repo"), QStringLiteral("the project"), QStringLiteral("o projeto"),
        QStringLiteral("source"), QStringLiteral("codigo"), QStringLiteral("c\u00f3digo"),
        QStringLiteral("build"), QStringLiteral("compile"), QStringLiteral("compilar"),
        QStringLiteral("git")
    };

    bool dev = false, host = false;
    for (const QString &w : deviceWords) { if (nikitaHasWord(t, w)) { dev = true; break; } }
    for (const QString &w : hostWords)   { if (nikitaHasWord(t, w)) { host = true; break; } }
    if (dev && !host) { return FocusDevice; }
    if (host && !dev) { return FocusHost; }
    return FocusBoth;   // both mentioned, or neither -- don't guess, offer everything
}

// The tool list the model sees, narrowed to the machine the message is about.
// Tool COUNT is the biggest lever on whether a small model calls a tool at
// all -- at 22 entries a 3B stopped calling anything. Narrowing to one
// machine's family also removes the mistake of picking the wrong one.
// overBle picks which half of the device toolbox is real. Over USB the CLI does
// everything and does it deterministically, so the screen and the buttons are
// only a slower way to get the same job wrong. Over BLE there IS no CLI -- the
// in-app terminal is USB-only ("CLI is USB only.") -- so the screen and OK/BACK
// are the only way to touch the device at all, and they earn their place.
static QJsonArray nikitaTools(bool agent, int focus = FocusBoth,
                              const QSet<QString> *allowed = nullptr,
                              bool overBle = false)
{
    const QJsonObject listFiles{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "list_files"},
            {"description", "List files and folders ON THE CONNECTED FLIPPER ZERO at a path. Use /ext for the SD card root, /ext/apps for installed apps, /int for internal. Returns each entry's name, type (dir/file) and size in bytes."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Absolute path on the Flipper, e.g. /ext or /ext/apps"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject readFile{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "read_file"},
            {"description", "Read the text contents of a file ON THE CONNECTED FLIPPER ZERO. Returns up to ~8 KB of text."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Absolute path to a file on the Flipper, e.g. /ext/apps_data/x/config.txt"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject irUniversal{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "ir_universal"},
            {"description", "Fire a UNIVERSAL infrared remote button through the CLI, automatically -- no menu navigation, no crash. This reads the Flipper's own universal asset (/ext/infrared/assets/<remote>.ir) and transmits EVERY code for that button, one brand after another, exactly like the on-screen universal remote does. Use this for requests like 'turn off the TV', 'TV power', 'mute the TV', 'AC power'. Much more reliable than pressing buttons."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"remote", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"tv", "ac", "audio", "projector"}}, {"description", "Which universal remote: tv, ac, audio, or projector"}}},
                    {"button", QJsonObject{{"type", "string"}, {"description", "The button to send, e.g. Power, Mute, Vol_up, Ch_next. Defaults to Power."}}}
                }},
                {"required", QJsonArray{"remote"}}
            }}
        }}
    };
    const QJsonObject readScreen{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "read_screen"},
            {"description", "See what is on the Flipper's screen RIGHT NOW. Returns the current screen as text (menu items, the highlighted/selected item, titles) read straight from the device's framebuffer. Use it to VERIFY where you are before and after pressing buttons -- you are NOT blind when you call this. Call it to check a menu before choosing, to confirm you landed where you meant to, and to read any on-screen result."},
            {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
        }}
    };
    const QJsonObject pressButton{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "press_button"},
            {"description", "Tap OK or BACK on the connected Flipper Zero. These two only -- there is deliberately no D-pad here. Walking menus by up/down/left/right is guesswork that lands on the wrong item, and everything it was used for is done properly through run_cli instead (loader open/close/info to move between apps, storage/fls/fcat for files, ir tx to fire a signal). Use OK to confirm a prompt that is already on screen and BACK to leave one. The RESULTING screen comes back with the press, in the \"screen\" field. If you are reaching for this to NAVIGATE somewhere, stop: the CLI knows the way and this does not."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"button", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"ok", "back"}}, {"description", "Which button to tap -- ok to confirm what is on screen, back to leave it"}}},
                    {"times", QJsonObject{{"type", "integer"}, {"description", "How many times to tap it (default 1). Leave it at 1 unless you have a screen-confirmed reason not to."}}}
                }},
                {"required", QJsonArray{"button"}}
            }}
        }}
    };
    const QJsonObject runCli{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "run_cli"},
            {"description", "Run a command on the FLIPPER ZERO over USB and get its text output back. You always have this -- it is the full Flipper CLI. Use it for anything the storage tools don't cover: device_info, gpio (mode/read/set), subghz (tx/rx/decode), nfc, rfid, ir (tx), led, vibro, power (off/reboot), i2c, onewire, ikey, loader, log, free, uptime, js, top. Unix-style shortcuts are translated for you and BOTH spellings work here -- 'fls /ext/nfc', 'ls /ext/nfc' and 'storage list /ext/nfc' all do the same thing, because this tool only ever reaches the Flipper. Available: ls/fls, cat/fcat, tree/ftree, stat/fstat, md5/fmd5, mkdir/fmkdir, rm/frm, mv/fmv, df/fdf, touch/ftouch, echo/fecho, whoami/fwhoami, open/fopen, close/fclose, vibro/fvibro, reboot/freboot, shutdown/fshutdown. Relative paths resolve against /ext; there is no current folder here, so no cd. NOT available: cp between the two machines, find, locate, grep, head, tail, wc, sed, diff, edit and rm -r -- those need the interactive panel, so use the file tools instead. To run something on THIS COMPUTER use host_run, never this tool. One command per call. It briefly pauses the normal session, so prefer the storage tools for plain file work. If it answers that the CLI panel is open, tell the user to close the CLI window -- do not claim you have no access. SAFETY: a malformed or unknown firmware subcommand does not just print an error here -- it can crash the Flipper and force a reboot. `ir universal tv power` did exactly that, and the reason was the SIGNAL NAME: the valid one is `Power`, capitalised, and a name that is not in the list can take the device down. Run `ir universal list tv` first and copy the name from its output. Send only commands whose EXACT syntax you are sure of; if a command returns its own help/usage banner, that means it did NOT run -- read the usage and correct it, do not report success. When unsure, run the bare command (e.g. `ir`) to see its help first, then use the precise form it shows."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"command", QJsonObject{{"type", "string"}, {"description", "The exact CLI command line, e.g. 'device_info' or 'led r 255' or 'vibro 1'"}}}
                }},
                {"required", QJsonArray{"command"}}
            }}
        }}
    };
    const QJsonObject saveFile{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "save_file"},
            {"description", "Save/write text content to a file ON THE CONNECTED FLIPPER ZERO's SD card (e.g. a script you wrote). Use the right folder: BadUSB -> /ext/badusb/*.txt, Sub-GHz -> /ext/subghz/*.sub, Infrared -> /ext/infrared/*.ir, NFC -> /ext/nfc/*.nfc, otherwise /ext/. The folder must already exist."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Absolute path including filename, e.g. /ext/badusb/hello.txt"}}},
                    {"content", QJsonObject{{"type", "string"}, {"description", "The full text content to write into the file"}}}
                }},
                {"required", QJsonArray{"path", "content"}}
            }}
        }}
    };
    const QJsonObject makeDir{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "make_dir"},
            {"description", "Create a folder (and any missing parent folders) ON THE CONNECTED FLIPPER ZERO's SD card, e.g. /ext/apps/Scripts."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Absolute folder path on the Flipper, e.g. /ext/apps/Scripts"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject deleteFile{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "delete_file"},
            {"description", "Delete a file or folder ON THE CONNECTED FLIPPER ZERO's SD card. Destructive -- only call it when the user clearly asked to delete something."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Absolute path to delete, e.g. /ext/badusb/old.txt"}}},
                    {"recursive", QJsonObject{{"type", "boolean"}, {"description", "Delete a non-empty folder and everything in it (default false)"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject renameFile{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "rename_file"},
            {"description", "Rename or MOVE a file/folder ON THE CONNECTED FLIPPER ZERO's SD card (same operation does both)."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"from", QJsonObject{{"type", "string"}, {"description", "Current absolute path"}}},
                    {"to", QJsonObject{{"type", "string"}, {"description", "New absolute path (rename) or new location (move)"}}}
                }},
                {"required", QJsonArray{"from", "to"}}
            }}
        }}
    };
    const QJsonObject fileInfo{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "file_info"},
            {"description", "Check whether a path exists ON THE CONNECTED FLIPPER ZERO and whether it is a file or a directory, plus its size in bytes."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Absolute path on the Flipper to stat"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };

    const QJsonObject remember{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "remember"},
            {"description", "Save something you have learned about the user. Call this PROACTIVELY -- without being asked, in the same turn, without announcing it -- whenever they reveal anything that would still be worth knowing next week: who they are, their machine and setup, what they are building and why, opinions, preferences and dislikes, something they explain to you, a decision and its reason, or a detail from the conversation they will expect you to still know later. Also call it when they explicitly say 'remember...'. This is NOT a log of what you did -- your actions are recorded separately. Save ONE thing per call, one short line, third person ('User ...'). Do NOT save greetings, filler, or the mechanics of the current task, and do NOT save something you already remember."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"fact", QJsonObject{{"type", "string"}, {"description", "One concise durable fact, third person, starting with 'User'. This describes the shape only -- a sample sentence here gets repeated back as though it were true about this person."}}}
                }},
                {"required", QJsonArray{"fact"}}
            }}
        }}
    };
    const QJsonObject listMemory{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "list_memory"},
            {"description", "Show everything you currently remember about the user. Call it when they ask what you remember/know about them."},
            {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
        }}
    };
    const QJsonObject forget{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "forget"},
            {"description", "Delete remembered facts. Pass a word/phrase to remove only matching facts, or pass \"all\" to wipe memory. Call it when the user says to forget something."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"match", QJsonObject{{"type", "string"}, {"description", "Text to match facts to delete, or 'all' to clear everything"}}}
                }},
                {"required", QJsonArray{"match"}}
            }}
        }}
    };

    // Memory always travels; it is orthogonal to which machine is in play.
    QJsonArray tools{remember, listMemory, forget};

    // The Flipper's own file tools come off the list when the message is plainly
    // about the computer. Not to forbid anything -- runOneTool would reroute a
    // stray call anyway -- but because a tool that isn't offered is a tool that
    // can't be picked by mistake, and a shorter list is one a 3B can still
    // choose from at all.
    // Narrowing to the host only makes sense when host tools EXIST to narrow to.
    // With agent mode off there are none, so dropping the Flipper's tools left
    // the model holding nothing but the three memory tools -- and a model with
    // no way to act does the only thing left: it describes acting. That is what
    // produced "Created hello.duk on your Desktop" with no write behind it, and
    // then a working directory invented out of nothing.
    const bool prune = agent && (focus == FocusHost);
    if (!prune) {
        tools.append(listFiles);
        tools.append(readFile);
        tools.append(saveFile);
        tools.append(makeDir);
        tools.append(deleteFile);
        tools.append(renameFile);
        tools.append(fileInfo);
        tools.append(irUniversal);
        tools.append(readScreen);
        tools.append(pressButton);
        tools.append(runCli);
    }

    if (!agent) { return tools; }

    // Host tools: only advertised when the user has turned agent mode on.
    const QJsonObject hostList{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_list"},
            {"description", "List files and folders on THIS COMPUTER. Absolute path, or ~/... for home, or relative to the workspace folder. Use \".\" for the workspace folder itself."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Path relative to the workspace root, e.g. . or application"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject hostRead{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_read"},
            {"description", "Read a text file from THIS COMPUTER. Absolute path, or ~/... , or relative to the workspace folder."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "File path relative to the workspace root, e.g. application/nikitabackend.cpp"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject hostWrite{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_write"},
            {"description", "Write/overwrite a text file on THIS COMPUTER. Creates missing parent folders. Absolute path, or ~/... , or relative to the workspace folder. It OVERWRITES the whole file, so read it first."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "File path relative to the workspace root"}}},
                    {"content", QJsonObject{{"type", "string"}, {"description", "Full new file contents"}}}
                }},
                {"required", QJsonArray{"path", "content"}}
            }}
        }}
    };
    const QJsonObject hostRun{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_run"},
            {"description", "Run a shell command on THIS COMPUTER and get back its exit code plus captured stdout/stderr. Runs in the workspace folder unless cwd is given. Prefer a typed tool (host_read, host_find, host_delete) when one fits -- they report failures properly. Blocks until the command finishes or times out."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"command", QJsonObject{{"type", "string"}, {"description", "The command line to run, e.g. make -j8 or git status"}}},
                    {"cwd", QJsonObject{{"type", "string"}, {"description", "Optional folder to run it in"}}}
                }},
                {"required", QJsonArray{"command"}}
            }}
        }}
    };

    const QJsonObject hostCd{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_cd"},
            {"description", "Move to a folder on this computer, and get back where you now are plus what is in it. Call it with no path to just ask where you are. Relative paths after this resolve from here, and host_run starts here."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Folder to move to. Omit to just report the current one. '..' goes up."}}}
                }},
                {"required", QJsonArray{}}
            }}
        }}
    };
    const QJsonObject hostMkdir{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_mkdir"},
            {"description", "Create a folder on this computer, parents included. Absolute path, or ~/... , or relative to the workspace folder."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Folder to create"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject hostDelete{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_delete"},
            {"description", "Delete a file or a folder (recursively) on this computer. Destructive and not undoable -- only when the user clearly asked for it."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "File or folder to delete"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    const QJsonObject hostMove{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_move"},
            {"description", "Move or rename a file on this computer. Creates the destination folder if it is missing."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"from", QJsonObject{{"type", "string"}, {"description", "Existing path"}}},
                    {"to", QJsonObject{{"type", "string"}, {"description", "New path"}}}
                }},
                {"required", QJsonArray{"from", "to"}}
            }}
        }}
    };
    const QJsonObject hostCopy{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_copy"},
            {"description", "Copy a file on this computer. Creates the destination folder if it is missing."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"from", QJsonObject{{"type", "string"}, {"description", "Existing path"}}},
                    {"to", QJsonObject{{"type", "string"}, {"description", "Destination path"}}}
                }},
                {"required", QJsonArray{"from", "to"}}
            }}
        }}
    };
    const QJsonObject hostFind{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "host_find"},
            {"description", "Search a folder tree on this computer for names matching a wildcard. Returns full paths."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{{"type", "string"}, {"description", "Folder to search under"}}},
                    {"pattern", QJsonObject{{"type", "string"}, {"description", "Wildcard, e.g. *.cpp"}}}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };

    if (focus != FocusDevice) {
        tools.append(hostList);
        tools.append(hostRead);
        tools.append(hostWrite);
        tools.append(hostRun);
        tools.append(hostCd);
        tools.append(hostMkdir);
        tools.append(hostDelete);
        tools.append(hostMove);
        tools.append(hostCopy);
        tools.append(hostFind);
    }

    // Transport filter, before the access filter. Offering a tool that cannot
    // work on the current link is worse than not having it: run_cli over BLE
    // fails every time, and press_button/read_screen over USB is the manual
    // route that kept losing to the CLI sitting right next to it.
    {
        QJsonArray kept;
        for (const QJsonValue &v : std::as_const(tools)) {
            const QString n = v.toObject().value(QStringLiteral("function"))
                               .toObject().value(QStringLiteral("name")).toString();
            const bool manual = (n == QLatin1String("press_button")
                              || n == QLatin1String("read_screen"));
            // ir_universal reads the code file over RPC (fine wirelessly) and
            // then TRANSMITS each code through the CLI, which is serial-only.
            // Offered on BLE it did the reading, reported progress, and died at
            // the send -- a tool that half-works is worse than one that is not
            // there, because the half that ran looks like success.
            const bool needsCli = (n == QLatin1String("run_cli")
                                || n == QLatin1String("ir_universal"));
            if (overBle) {
                if (needsCli) { continue; }
            } else if (manual) {
                continue;
            }
            kept.append(v);
        }
        tools = kept;
    }

    // Access filter, applied last: building the whole list and pruning after
    // keeps the blocks above untouched and the filtering in one place.
    // allowed == nullptr means "no filter" -- the full list, as before.
    if (allowed) {
        QJsonArray kept;
        for (const QJsonValue &v : std::as_const(tools)) {
            const QString n = v.toObject().value(QStringLiteral("function"))
                               .toObject().value(QStringLiteral("name")).toString();
            if (allowed->contains(n)) { kept.append(v); }
        }
        return kept;
    }
    return tools;
}

// A tiny worked example wired ahead of the real conversation: small local
// models follow a DEMONSTRATED pattern more reliably than written rules, so
// one exchange where NIKITA silently calls save_file and confirms in one line
// primes the model to do the same instead of narrating "Step 1...".
// Intent router: ACTION (touch a file/device -> needs tools) vs plain
// CONVERSATION (send without tools so a weak model can't dump pseudo-code).
// True when the turn explicitly asks to WRITE something -- narrower than
// messageNeedsTools: this is the set where finishing without a tool call
// means the model lied about having done it.
static bool messageIsFileWrite(const QString &text)
{
    const QString t = text.toLower();
    // Includes removal verbs too -- deleting changes something. Without them
    // "remove the folder X" wasn't treated as an action, so a false "removed"
    // claim never got caught.
    static const QStringList writeVerbs = {
        QStringLiteral("save"), QStringLiteral("create"), QStringLiteral("write"),
        QStringLiteral("build"), QStringLiteral("generate"), QStringLiteral("make"),
        QStringLiteral("develop"), QStringLiteral("craft"),
        QStringLiteral("delete"), QStringLiteral("remove"), QStringLiteral("erase"),
        QStringLiteral("rename"), QStringLiteral("move"), QStringLiteral("copy"),
        QStringLiteral("apaga"), QStringLiteral("deleta"), QStringLiteral("remova"),
        QStringLiteral("cria"), QStringLiteral("salva"), QStringLiteral("escreve"),
        QStringLiteral("renomeia"), QStringLiteral("mova"), QStringLiteral("copia"),
        // Kept in step with actionWords in messageNeedsTools() above.
        QStringLiteral("trash"), QStringLiteral("duplicate"), QStringLiteral("download"),
        QStringLiteral("install"), QStringLiteral("launch"), QStringLiteral("execute"),
        QStringLiteral("update"), QStringLiteral("modify"), QStringLiteral("backup"),
        QStringLiteral("append"), QStringLiteral("clear"), QStringLiteral("wipe"),
        QStringLiteral("format"), QStringLiteral("restore"), QStringLiteral("compress"),
        QStringLiteral("extract"),
        QStringLiteral("apag"), QStringLiteral("delet"), QStringLiteral("exclu"),
        QStringLiteral("remov"), QStringLiteral("renom"), QStringLiteral("copi"),
        QStringLiteral("duplic"), QStringLiteral("escrev"), QStringLiteral("salv"),
        QStringLiteral("gera"), QStringLiteral("monta"), QStringLiteral("grava"),
        QStringLiteral("instala"), QStringLiteral("baixa"), QStringLiteral("envia"),
        QStringLiteral("atualiza"), QStringLiteral("corrige"), QStringLiteral("conserta"),
        QStringLiteral("limpa")
    };
    bool verb = false;
    for (const QString &w : writeVerbs) {
        int i = t.indexOf(w);
        while (i >= 0) {
            if (i == 0 || !t.at(i - 1).isLetter()) { verb = true; break; }
            i = t.indexOf(w, i + 1);
        }
        if (verb) { break; }
    }
    if (!verb) { return false; }
    static const QStringList fileNouns = {
        QStringLiteral("script"), QStringLiteral("payload"), QStringLiteral("file"),
        QStringLiteral("badusb"), QStringLiteral("ducky"), QStringLiteral("subghz"),
        QStringLiteral("sub-ghz"), QStringLiteral("nfc"), QStringLiteral("rfid"),
        QStringLiteral("infrared"), QStringLiteral("/ext"), QStringLiteral("arquivo"),
        // A directory noun too -- "create a folder named X" is a write request
        // with no file in it, and used to go uncaught without one.
        QStringLiteral("folder"), QStringLiteral("directory"), QStringLiteral("pasta"),
        QStringLiteral("diretorio"), QStringLiteral("dir "),
        QStringLiteral("document"), QStringLiteral("note"), QStringLiteral("text"),
        QStringLiteral("code"), QStringLiteral("project"), QStringLiteral("photo"),
        QStringLiteral("image"), QStringLiteral("picture"), QStringLiteral("video"),
        QStringLiteral("config"), QStringLiteral("settings"),
        QStringLiteral("documento"), QStringLiteral("nota"), QStringLiteral("texto"),
        QStringLiteral("codigo"), QStringLiteral("projeto"), QStringLiteral("imagem"),
        QStringLiteral("foto")
    };
    for (const QString &n : fileNouns) {
        if (t.contains(n)) { return true; }
    }
    // Any name.ext at all, rather than a list of extensions that will always be
    // one behind. ".duk" wasn't on the old list, so "create hello.duk and save
    // it" was not recognised as a write -- and the check that catches the model
    // claiming to have written something never ran.
    static const QRegularExpression anyFilename(
        QStringLiteral("\\b[\\w.-]+\\.[a-z0-9]{1,6}\\b"), QRegularExpression::CaseInsensitiveOption);
    return anyFilename.match(t).hasMatch();
}

// Is this turn about the Flipper, or about the user's own computer?
//
// Decides whether the radio/NFC/IR/BadUSB/button manual is worth its ~2000
// tokens of window this turn. Deliberately GENEROUS: a false positive costs
// prompt space, a false negative costs the model the knowledge it needed, so
// anything device-shaped counts. The word list is the same one the WHICH
// MACHINE section of the prompt uses, in both languages, because the user
// writes in Portuguese and the rule has to catch "salva no flipper" as surely
// as "save to the flipper".
static bool messageMentionsDevice(const QString &text)
{
    const QString t = text.toLower();
    static const QStringList deviceWords = {
        QStringLiteral("flipper"), QStringLiteral("badusb"), QStringLiteral("bad usb"),
        QStringLiteral("ducky"), QStringLiteral("subghz"), QStringLiteral("sub-ghz"),
        QStringLiteral("sub ghz"), QStringLiteral("nfc"), QStringLiteral("rfid"),
        QStringLiteral("infrared"), QStringLiteral("infravermelho"), QStringLiteral("ibutton"),
        QStringLiteral("u2f"), QStringLiteral("gpio"), QStringLiteral("/ext"),
        QStringLiteral("/int"), QStringLiteral("sd card"), QStringLiteral("sdcard"),
        QStringLiteral("cartao"), QStringLiteral("cart\u00e3o"), QStringLiteral("microsd"),
        QStringLiteral("dispositivo"), QStringLiteral(".fap"), QStringLiteral(".sub"),
        QStringLiteral(".ir"), QStringLiteral(".nfc"), QStringLiteral(".rfid"),
        QStringLiteral(".duk"), QStringLiteral(".txt.ir"),
        QStringLiteral("firmware"), QStringLiteral("device"), QStringLiteral("button"),
        QStringLiteral("botao"), QStringLiteral("bot\u00e3o"), QStringLiteral("vibro"),
        QStringLiteral("radio"), QStringLiteral("r\u00e1dio"),
        // The button/GPIO manual lives in the same block, so the words for
        // driving the hardware have to open it too -- otherwise "press up
        // twice" arrives with the section that explains press_button cut out.
        QStringLiteral("press"), QStringLiteral("aperta"), QStringLiteral("pressiona"),
        QStringLiteral("screenshot"), QStringLiteral("screen"), QStringLiteral("tela"),
        QStringLiteral("reboot"), QStringLiteral("reiniciar"),
        QStringLiteral("storage"), QStringLiteral("mhz")
    };
    for (const QString &w : deviceWords) {
        if (t.contains(w)) { return true; }
    }
    // Too short to match as substrings: "led" sits inside "called" and
    // "filled", "cli" inside "click" and "cliente", and a bare 125 or 433 is
    // only a frequency when it stands alone. Generous is the rule here, but
    // "called" turning every turn into a Flipper turn is not generous, it is
    // broken -- so these count as whole words only.
    static const QStringList wholeWords = {
        QStringLiteral("led"), QStringLiteral("cli"),
        QStringLiteral("125"), QStringLiteral("433")
    };
    static const QRegularExpression splitter(QStringLiteral("[^\\p{L}\\p{N}]+"));
    const QStringList tokens = t.split(splitter, Qt::SkipEmptyParts);
    for (const QString &tok : tokens) {
        if (wholeWords.contains(tok)) { return true; }
    }
    return false;
}

// A plain "turn on/off the TV" and every close variant. TV power over IR is a
// TOGGLE -- the same Power code turns the set on and off -- so on, off, "power",
// "liga", "desliga" all map to the ONE action: fire the universal TV Power code.
// Matched here so the app can do it DETERMINISTICALLY, without a model round
// that might narrate or navigate. Kept tight (a short, single command, no "and"
// / "then" joining a second task) so it never hijacks a larger request.
static bool nikitaIsTvPowerCommand(const QString &text)
{
    QString t = text.toLower().simplified();
    t.remove(QRegularExpression(QStringLiteral("^(hey|ok|oi|ei|e a[ie])?\\s*nikita[,:]?\\s*"),
                                QRegularExpression::CaseInsensitiveOption));
    if (!t.contains(QLatin1String("tv")) && !t.contains(QLatin1String("televis"))) { return false; }
    // Two tasks joined -> let the model handle it, do not shortcut half of it.
    if (t.contains(QLatin1String(" and ")) || t.contains(QLatin1String(" then "))
        || t.contains(QLatin1String(" e ")) || t.contains(QLatin1Char(';'))) { return false; }
    static const QRegularExpression powerRe(QStringLiteral(
        "\\b(turn|switch)\\s+(it\\s+)?(on|off)\\b|\\bpower\\b|"
        "\\b(liga|desliga|ligar|desligar|ligue|desligue)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return powerRe.match(t).hasMatch();
}

// "remember that remote4 is my tv remote" is a note to self, not a job on the
// Flipper -- and it was answered with a remember() followed by a read_screen and
// three button presses on a device nobody had asked it to touch. The words that
// open the sentence say plainly that filing the fact IS the whole task; when
// they do, and nothing in the rest of it asks for an action, the turn is handed
// the memory tools alone so there is no device tool available to wander into.
static bool nikitaIsMemoryOnly(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t.isEmpty()) { return false; }

    static const QStringList openers = {
        QStringLiteral("remember"), QStringLiteral("note that"), QStringLiteral("keep in mind"),
        QStringLiteral("don't forget"), QStringLiteral("dont forget"), QStringLiteral("forget"),
        QStringLiteral("lembre"), QStringLiteral("lembra"), QStringLiteral("lembrar"),
        QStringLiteral("anota"), QStringLiteral("anote"), QStringLiteral("esquece"),
        QStringLiteral("esque\u00e7a"), QStringLiteral("guarda que"), QStringLiteral("guarde que")
    };
    bool opens = false;
    for (const QString &w : openers) { if (t.startsWith(w)) { opens = true; break; } }
    if (!opens) { return false; }

    // ...unless the same sentence also asks for something to be DONE. "remember
    // remote4 is the tv one and turn it on" is two requests, and the second one
    // needs every tool it would normally get.
    static const QStringList actionWords = {
        QStringLiteral("open"), QStringLiteral("press"), QStringLiteral("run"),
        QStringLiteral("save"), QStringLiteral("write"), QStringLiteral("create"),
        QStringLiteral("make"), QStringLiteral("delete"), QStringLiteral("remove"),
        QStringLiteral("list"), QStringLiteral("show"), QStringLiteral("read"),
        QStringLiteral("send"), QStringLiteral("turn on"), QStringLiteral("turn off"),
        QStringLiteral("go into"), QStringLiteral("go to"), QStringLiteral("navigate"),
        QStringLiteral("abre"), QStringLiteral("abrir"), QStringLiteral("liga"),
        QStringLiteral("desliga"), QStringLiteral("roda"), QStringLiteral("executa"),
        QStringLiteral("mostra"), QStringLiteral("apaga"), QStringLiteral("cria")
    };
    for (const QString &w : actionWords) { if (nikitaHasWord(t, w)) { return false; } }
    return true;
}

static bool messageNeedsTools(const QString &text)
{
    const QString t = text.toLower();

    // Memory requests always need tools (remember / forget / list_memory), even
    // though they touch no file or device -- otherwise the model can never save.
    static const QStringList memoryWords = {
        QStringLiteral("remember"), QStringLiteral("forget"), QStringLiteral("memoriz"),
        QStringLiteral("keep in mind"), QStringLiteral("don't forget"), QStringLiteral("dont forget"),
        QStringLiteral("what do you know")
    };
    for (const QString &m : memoryWords) {
        if (t.contains(m)) { return true; }
    }

    // Someone telling you about themselves.
    //
    // The instructions say to save durable facts PROACTIVELY, without being
    // asked -- but a plain "i live in dublin" carries no verb+noun and none of
    // the words above, so the turn arrived with no tools at all and remember()
    // could not have been called however willing the model was. The rule and
    // the gate disagreed, and the gate wins every time.
    static const QRegularExpression personalDisclosure(QStringLiteral(
        "\\b(i|i'm|im)\\s+(live|am|work|use|have|prefer|run|own|speak|study|"
        "usually|always|never)\\b|\\bmy\\s+(name|mac|macbook|laptop|pc|setup|"
        "keyboard|flipper|job|company|team|project|timezone|birthday)\\b|"
        "\\b(eu|meu|minha)\\s+(moro|sou|trabalho|uso|tenho|nome|mac|teclado|projeto)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (personalDisclosure.match(t).hasMatch()) { return true; }

    // name almost always means "do something with this" -> tools on, verb or not.
    static const QStringList strongNouns = {
        QStringLiteral("/ext"), QStringLiteral("/int"), QStringLiteral(".txt"),
        QStringLiteral(".sub"), QStringLiteral(".nfc"), QStringLiteral(".ir"),
        QStringLiteral("badusb"), QStringLiteral("ducky"), QStringLiteral("subghz"),
        QStringLiteral("sub-ghz"),
        // CLI-driven hardware commands (run_cli) -- stems catch PT/EN variants
        QStringLiteral("cli"), QStringLiteral("device_info"), QStringLiteral("gpio"),
        QStringLiteral("vibr"), QStringLiteral("reboot"), QStringLiteral("led"),
        QStringLiteral("i2c"), QStringLiteral("onewire"),
        QStringLiteral("uptime"), QStringLiteral("battery"),
        QStringLiteral("nfc"), QStringLiteral("rfid"), QStringLiteral("infrared"),
        QStringLiteral("flipper"),
        // IR / universal-remote control. "turn off my tv" carried NO trigger
        // before, so it arrived with only the memory tools and the model, unable
        // to act, claimed success -- exactly the false-claim that then narrowed
        // the retry. These route it to the device toolset (ir_universal) from
        // the first round.
        QStringLiteral("tv"), QStringLiteral("television"), QStringLiteral("televis"),
        QStringLiteral("projector"), QStringLiteral("projetor"),
        QStringLiteral("air conditioner"), QStringLiteral("ar condicionado"),
        QStringLiteral("remote"), QStringLiteral("controle remoto"),
        QStringLiteral("universal remote"), QStringLiteral("turn off"),
        QStringLiteral("turn on"), QStringLiteral("desliga"), QStringLiteral("desligar"),
        // The computer side. Without these, "save it to my Desktop" carried no
        // trigger at all unless it happened to name a .txt, and a turn with no
        // tools attached cannot call one however clearly it was asked.
        // Applications live on a computer. A Flipper has no Safari, no browser,
        // no Terminal and no Finder -- naming one of them can only mean the Mac,
        // and without these "open safari" read as ambiguous and got routed to
        // the device.
        QStringLiteral("safari"), QStringLiteral("chrome"), QStringLiteral("firefox"),
        QStringLiteral("browser"), QStringLiteral("navegador"), QStringLiteral("terminal"),
        QStringLiteral("finder"), QStringLiteral("webpage"), QStringLiteral("website"),
        QStringLiteral("http"), QStringLiteral(".com"), QStringLiteral(".app"),
        QStringLiteral("desktop"), QStringLiteral("area de trabalho"),
        QStringLiteral("\u00e1rea de trabalho"), QStringLiteral("downloads"),
        QStringLiteral("documents"), QStringLiteral("documentos"),
        QStringLiteral("computer"), QStringLiteral("computador"),
        QStringLiteral("my mac"), QStringLiteral("meu mac"), QStringLiteral("macbook"),
        QStringLiteral("minha pasta"), QStringLiteral("my folder"),
        QStringLiteral("/users/"), QStringLiteral("~/"),
        QStringLiteral("repo"), QStringLiteral("compil"), QStringLiteral("build"),
        QStringLiteral("git")
    };
    for (const QString &s : strongNouns) {
        if (t.contains(s)) { return true; }
    }

    // CLI command names. These have to match on a word boundary, not as a
    // substring: "ls" alone would fire on "tools", "false" and "controls", and
    // "rm" on "confirm", which would drag half of ordinary conversation into
    // tool mode.
    // Built from the command table rather than typed out, so a command added
    // there is recognised here too. A hand-written copy of this list is exactly
    // the kind of thing that silently stops matching the real command set.
    static const QStringList cliWords = nikitaCliIntentWords();
    for (const QString &w : cliWords) {
        int idx = t.indexOf(w);
        while (idx >= 0) {
            const bool leftOk  = (idx == 0) || !(t.at(idx - 1).isLetterOrNumber() || t.at(idx - 1) == QLatin1Char('_'));
            const int  end     = idx + w.size();
            const bool rightOk = (end >= t.size()) || !(t.at(end).isLetterOrNumber() || t.at(end) == QLatin1Char('_'));
            if (leftOk && rightOk) { return true; }
            idx = t.indexOf(w, idx + 1);
        }
    }

    // Verbs that imply doing something to a file / the device.
    //
    // This list used to be English-only, which meant a request phrased entirely
    // in Portuguese -- "apaga esse arquivo", with no English word anywhere in
    // it -- matched nothing here, hasVerb stayed false, and the turn got NO
    // tools at all: not "the model chose not to delete it", but the delete
    // tool was never even offered. messageIsFileWrite() (above) already carried
    // Portuguese verbs for a related check; this list did not, and the two
    // silently drifting apart is exactly how that gap opened. Stems (e.g.
    // "apag", "delet") are used on purpose where safe, so one entry catches a
    // verb's conjugations ("apaga", "apagar", "apagando", "apagou") instead of
    // spelling out every inflection by hand -- matches the "vibr"/"compil"
    // convention already used elsewhere in this file (see nikitaMessageFocus).
    static const QStringList actionWords = {
        QStringLiteral("save"), QStringLiteral("create"), QStringLiteral("make"),
        QStringLiteral("write"), QStringLiteral("build"), QStringLiteral("generate"),
        QStringLiteral("list"), QStringLiteral("show"), QStringLiteral("read"),
        QStringLiteral("open"), QStringLiteral("delete"), QStringLiteral("remove"),
        QStringLiteral("rename"), QStringLiteral("move"), QStringLiteral("press"),
        QStringLiteral("push"), QStringLiteral("navigate"), QStringLiteral("run"),
        QStringLiteral("edit"), QStringLiteral("mkdir"), QStringLiteral("folder"),
        QStringLiteral("copy"), QStringLiteral("vibrate"), QStringLiteral("reboot"),
        // More English phrasings for the same handful of actions.
        QStringLiteral("erase"), QStringLiteral("trash"), QStringLiteral("duplicate"),
        QStringLiteral("download"), QStringLiteral("install"), QStringLiteral("launch"),
        QStringLiteral("execute"), QStringLiteral("update"), QStringLiteral("modify"),
        QStringLiteral("backup"), QStringLiteral("append"), QStringLiteral("clear"),
        QStringLiteral("wipe"), QStringLiteral("format"), QStringLiteral("restore"),
        QStringLiteral("compress"), QStringLiteral("extract"), QStringLiteral("rename to"),
        // Portuguese verb stems -- the actual gap. "cria" catches criar/criando/
        // criador, "apag" catches apaga/apagar/apagando/apagou, etc.
        QStringLiteral("cria"), QStringLiteral("salv"), QStringLiteral("escrev"),
        QStringLiteral("gera"), QStringLiteral("monta"), QStringLiteral("grava"),
        QStringLiteral("apag"), QStringLiteral("delet"), QStringLiteral("exclu"),
        QStringLiteral("remov"), QStringLiteral("renom"), QStringLiteral("copi"),
        QStringLiteral("duplic"), QStringLiteral("abre "), QStringLiteral("abrir"),
        QStringLiteral("instala"), QStringLiteral("baixa"), QStringLiteral("envia"),
        QStringLiteral("atualiza"), QStringLiteral("corrige"), QStringLiteral("conserta"),
        QStringLiteral("limpa"), QStringLiteral("mostra"), QStringLiteral("lista"),
        QStringLiteral("leia"), QStringLiteral("le "), QStringLiteral("edita"),
        QStringLiteral("aperta"), QStringLiteral("pressiona"), QStringLiteral("reinicia")
    };
    // Nouns that anchor an action to a file / the device.
    static const QStringList actionNouns = {
        QStringLiteral("file"), QStringLiteral("files"), QStringLiteral("folder"),
        QStringLiteral("script"), QStringLiteral("badusb"), QStringLiteral("ducky"),
        QStringLiteral("payload"), QStringLiteral("subghz"), QStringLiteral("sub-ghz"),
        QStringLiteral("nfc"), QStringLiteral("rfid"), QStringLiteral("infrared"),
        QStringLiteral("ir "), QStringLiteral("ibutton"), QStringLiteral("button"),
        QStringLiteral("/ext"), QStringLiteral("/int"), QStringLiteral("sd card"),
        QStringLiteral("sdcard"), QStringLiteral(".txt"), QStringLiteral(".sub"),
        QStringLiteral(".nfc"), QStringLiteral(".ir"), QStringLiteral("app"),
        // More English nouns worth anchoring an action to.
        QStringLiteral("document"), QStringLiteral("note"), QStringLiteral("text"),
        QStringLiteral("code"), QStringLiteral("project"), QStringLiteral("photo"),
        QStringLiteral("image"), QStringLiteral("picture"), QStringLiteral("video"),
        QStringLiteral("config"), QStringLiteral("settings"),
        // Portuguese nouns. "arquivo"/"pasta" alone were missing entirely, so
        // "apaga esse arquivo" or "cria uma pasta nova" had a verb (once the
        // stems above exist) but still no noun to pair it with.
        QStringLiteral("arquivo"), QStringLiteral("pasta"), QStringLiteral("diretorio"),
        QStringLiteral("diretório"), QStringLiteral("documento"), QStringLiteral("nota"),
        QStringLiteral("texto"), QStringLiteral("codigo"), QStringLiteral("código"),
        QStringLiteral("projeto"), QStringLiteral("imagem"), QStringLiteral("foto"),
        QStringLiteral("video"), QStringLiteral("vídeo"), QStringLiteral("configuração")
    };

    bool hasVerb = false;
    for (const QString &w : actionWords) {
        // word-ish match: at a boundary
        int idx = t.indexOf(w);
        while (idx >= 0) {
            const bool leftOk  = (idx == 0) || !t.at(idx - 1).isLetter();
            if (leftOk) { hasVerb = true; break; }
            idx = t.indexOf(w, idx + 1);
        }
        if (hasVerb) { break; }
    }
    if (!hasVerb) { return false; }              // no action verb -> conversation

    for (const QString &nsub : actionNouns) {
        if (t.contains(nsub)) { return true; }   // verb + file/device noun -> action
    }
    return false;                                // a verb alone (e.g. "show me") stays conversational
}


static QString nikitaMemoryPath();   // fwd decl: defined below, used in the ctor

NikitaBackend::NikitaBackend(QObject *parent)
    : QObject(parent)
{
    m_net.setTransferTimeout(0);
    loadHistory();
    loadFilters();
    loadMistakes();
    // Off on a fresh install. An assistant that reads files and remembers
    // things should be something a person switches on, not something they
    // discover already running -- and plenty of people want this app without
    // any AI at all.
    m_assistantEnabled = QSettings().value(QLatin1String(kAssistantEnabledKey), false).toBool();
    // A stored key is only ever trusted because the API said so once. Re-ask on
    // every start: it costs one small GET, it refreshes the model list, and it
    // is the difference between "a key is set" and "the assistant works".
    if (!apiKey().isEmpty()) { checkApiKey(); }
    // Leftovers from the local-model era. Harmless where they sit, but a stale
    // "provider=local" or an equipped local model tag in a settings file is a thing a
    // future reader has to work out the meaning of, and the meaning is "nothing".
    {
        QSettings s;
        s.remove(QStringLiteral("nikita/betaFast"));
        s.remove(QStringLiteral("nikita/provider"));
        s.remove(QStringLiteral("nikita/model"));
    }
    nikitaLog(QStringLiteral("startup: assistantEnabled=%1 (org=%2 app=%3 file=%4)")
                  .arg(m_assistantEnabled ? QStringLiteral("true") : QStringLiteral("false"),
                       QCoreApplication::organizationName(),
                       QCoreApplication::applicationName(),
                       QSettings().fileName()));
    // Once, here -- NOT inside setThinking with Qt::UniqueConnection. That flag
    // is only honoured for pointer-to-member connections; with a lambda the
    // connect fails outright, which is why the seconds counter only moved when
    // a phase changed instead of ticking.
    m_turnTicker.setInterval(1000);
    connect(&m_turnTicker, &QTimer::timeout, this, [this]() { emit turnStatusChanged(); });
    // Defaults ON. Computer access used to be opt-in behind a setting most
    // people never find, which meant the assistant silently had no way to touch
    // the machine it runs on -- and a model with no way to act describes acting
    // instead. The switch is still there for anyone who wants it off; it just
    // isn't the thing standing between a working install and "save this to my
    // Desktop".
    m_agentEnabled = QSettings().value(QStringLiteral("nikita/agentEnabled"), true).toBool();
    m_agentRoot = QSettings().value(QStringLiteral("nikita/agentDir")).toString();
    {   // load long-term memory (facts the user asked NIKITA to remember)
        QFile mf(nikitaMemoryPath());
        if (mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_memory = QString::fromUtf8(mf.readAll()).trimmed();
            mf.close();
        }
    }
    // Proven moves alongside the facts. Without this m_skills stayed empty until
    // the first message of the session, so the sync on connect had nothing to
    // write and actions-memory.txt was never created on a fresh card.
    loadProvenMoves();
}

bool NikitaBackend::hasBle() const
{
#ifdef HZUI_BLE
    return true;
#else
    return false;
#endif
}

void NikitaBackend::setAppBackend(ApplicationBackend *backend)
{
    m_appBackend = backend;
    if (m_appBackend) {
        // When a Flipper connects, its /ext/nikita files are the real brain --
        // load them so memory & personality travel with the device.
        connect(m_appBackend, &ApplicationBackend::backendStateChanged, this, [this]() {
            const bool ready = m_appBackend &&
                m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
            if (ready && !m_portableLoaded) {
                m_portableLoaded = true;
                loadPortableMemory();
            } else if (!ready) {
                m_portableLoaded = false;   // reset so it reloads on next connect
            }
        });

        // The device can already be Ready by the time we get here, depending on
        // start-up order. backendStateChanged then never fires again, the card
        // is never read, and the stale local cache loaded in the constructor
        // stands for the whole session -- which survives restarts, because the
        // cache is a file too.
        if (m_appBackend->backendState() == ApplicationBackend::BackendState::Ready) {
            m_portableLoaded = true;
            loadPortableMemory();
        }
    }
}

// Read the Flipper's own memory notes off the SD and adopt them as the source of
// truth (the Flipper is the brain; the local file is just a cache).
void NikitaBackend::loadPortableMemory()
{
    // The fifth and last door. Its ensureFlipperDir is what still put
    // /ext/nikita back on the card after an erase -- one bare MkDir, no reads,
    // which is why it survived the previous sweep: the folder reappeared with
    // nothing in it, looking exactly like something had reinstalled itself.
    if (!m_assistantEnabled) { return; }

    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (!dev) { return; }

    // The folder was only created on the first write, which on a fresh card
    // comes long after this runs, so both reads below failed on every new
    // device. Making it here leaves the card ready from the first connection.
    // mkdir on a folder that exists is harmless: ensureFlipperDir ignores the
    // result and carries on.
    QPointer<Flipper::FlipperZero> devRef0(dev);
    ensureFlipperDir("/ext/nikita", [this, devRef0]() {
        if (!devRef0) { return; }
        readPortableMemory();
    });
}

// The two chained SD reads. Split out so the folder check above has something
// to call when it finishes.
void NikitaBackend::readPortableMemory()
{
    // The fourth door onto the card, and the one that stayed open: with the
    // assistant switched off this still ran on every connect, and its
    // ensureFlipperDir RECREATED /ext/nikita right after an erase. The folder
    // reappearing is precisely what makes someone think a background process
    // is still running.
    if (!m_assistantEnabled) { return; }

    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (!dev) { return; }

    QBuffer *buf = new QBuffer(this);
    buf->open(QIODevice::ReadWrite);
    auto *op = dev->rpc()->storageRead("/ext/nikita/memory.txt", buf);
    // dev captured by QPointer: the chained read below runs later, and the
    // device can be unplugged between the two.
    QPointer<Flipper::FlipperZero> devRef(dev);
    connect(op, &AbstractOperation::finished, this, [this, op, buf, devRef]() {
        Flipper::FlipperZero *dev = devRef.data();
        if (op->isError()) {
            // Missing is the normal state until something is worth remembering,
            // same as actions-memory.txt below. A real failure still gets said.
            if (!op->errorString().contains(QLatin1String("does not exist"))) {
                nikitaLog(QStringLiteral("memory.txt could not be read from the card: %1")
                         .arg(op->errorString()));
            }
        } else {
            // Adopt the card's copy verbatim, including edits made by hand.
            // An empty file is a deliberate "forget everything", so it is
            // honoured rather than treated as "nothing to load".
            const QString body = QString::fromUtf8(buf->data());
            nikitaLog(QStringLiteral("memory.txt read from the card: %1 fact(s)")
                     .arg(nikitaFactList(body).size()));
            applyMemoryText(body, QStringLiteral("memory.txt on the SD card"));
        }
        buf->deleteLater();

        // Chained, not fired alongside. Two storageRead operations issued
        // back-to-back on one RPC session raced, and the second one's answer
        // came back unmatched -- "Cannot match message with id 3" in the log.
        // The track record travels with the Flipper too: plug the same device
        // into another machine and it should arrive knowing not just what it
        // knows about you, but what it has already proven it can do.
        if (!dev) { return; }

        // Deferred one turn on purpose. A failed operation makes ProtobufSession
        // clear its whole queue, and this runs from inside that operation's own
        // finished handler: anything enqueued here was being thrown away before
        // it could run. Which is why the second read never happened on a card
        // that had no memory.txt yet, and why the files were never created.
        QPointer<Flipper::FlipperZero> devRef2(dev);
        QTimer::singleShot(0, this, [this, devRef2]() {
        Flipper::FlipperZero *dev = devRef2.data();
        if (!dev) { return; }
        QBuffer *abuf = new QBuffer(this);
        abuf->open(QIODevice::ReadWrite);
        auto *aop = dev->rpc()->storageRead("/ext/nikita/actions-memory.txt", abuf);
        connect(aop, &AbstractOperation::finished, this, [this, aop, abuf]() {
            // Missing is the normal state until the first lesson is learned.
            // Nothing is wrong and nothing needs saying.
            if (!aop->isError()) {
                const QStringList lines = QString::fromUtf8(abuf->data())
                                              .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                // The card wins, same as memory.txt: it is the portable brain,
                // the local file is only a cache of it.
                if (!lines.isEmpty()) {
                    m_skills = lines;
                    while (m_skills.size() > 24) { m_skills.removeFirst(); }
                    saveProvenMoves();
                    nikitaLog(QStringLiteral("actions-memory.txt read from the card: %1 proven move(s)")
                                 .arg(m_skills.size()));
                }
            }
            abuf->deleteLater();

            // Both reads are done and whatever the card had has been adopted,
            // so push the result back. Without this the files only appeared
            // after a learn or a forget: a fresh card stayed empty however
            // much the assistant already knew.
            // Deferred for the same reason as the read above: this runs inside
            // a failed operation's handler, and the writes it enqueues would be
            // dropped with the rest of the cleared queue.
            QTimer::singleShot(0, this, [this]() { syncMemoryToFlipper(); });
        });
        });
    });
}

// Push the current memory + self-notes onto the Flipper SD (/ext/nikita/). This is
// the "backup to the device" -- called on connect and after every learn/forget so
// everything the assistant knows lives on the Flipper.
void NikitaBackend::syncMemoryToFlipper()
{
    // Disabled means the assistant does not touch the card. Without this the
    // startup mirror wrote memory.txt straight back after an erase -- an empty
    // file, so no data survived, but /ext/nikita reappeared on the card and the
    // erase looked like it had failed. It also settles the wider promise: off
    // has to mean no reads and no writes, not just a hidden panel.
    if (!m_assistantEnabled) { return; }
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) {
        // Nothing was written, so nothing is in sync. Forgetting this is what
        // would let the next connection -- possibly a different card -- be
        // skipped because the strings happen to match what the last one held.
        m_syncedMemory.clear();
        m_syncedSkills.clear();
        return;
    }

    const QString memBody = m_memory;
    const QString actBody = m_skills.join(QLatin1Char('\n'));

    // Nothing changed -> nothing to send. m_syncedMemory/m_syncedSkills were
    // already being recorded for exactly this, but nobody was reading them, so
    // every trigger re-issued the same MkDir + Write pair over USB. That is the
    // duplicated /ext/nikita traffic filling the log: not two callers racing,
    // one caller with a comparison that was never made.
    if (memBody == m_syncedMemory && actBody == m_syncedSkills) { return; }

    // ensureFlipperDir is async: the device can go away between the mkdir and
    // this callback, so it travels as a QPointer like readPortableMemory does.
    QPointer<Flipper::FlipperZero> devRef(dev);
    ensureFlipperDir("/ext/nikita", [this, devRef, memBody, actBody]() {
        Flipper::FlipperZero *dev = devRef.data();
        if (!dev) { return; }
        QBuffer *buf = new QBuffer(this);
        buf->setData(memBody.toUtf8());
        buf->open(QIODevice::ReadOnly);
        auto *op = dev->rpc()->storageWrite("/ext/nikita/memory.txt", buf);
        connect(op, &AbstractOperation::finished, this, [buf]() { buf->deleteLater(); });

        // Only ever write something. An empty list means "not loaded yet", not
        // "the user has no history" -- and a truncating write of nothing is
        // indistinguishable from deliberate erasure once it reaches the card.
        // Forgetting is what rateLastAction and a hand-edit of the file are for.
        if (!actBody.isEmpty()) {
            QBuffer *abuf = new QBuffer(this);
            abuf->setData(actBody.toUtf8());
            abuf->open(QIODevice::ReadOnly);
            auto *aop = dev->rpc()->storageWrite("/ext/nikita/actions-memory.txt", abuf);
            connect(aop, &AbstractOperation::finished, this, [abuf]() { abuf->deleteLater(); });
        }
        // Recorded only once the writes have been issued against a live device.
        // Marking it earlier would let a sync that never reached the card count
        // as done, and the next change would then be skipped as "unchanged".
        m_syncedMemory = memBody;
        m_syncedSkills = actBody;
    });
}

bool NikitaBackend::thinking() const { return m_thinking; }
bool NikitaBackend::configured() const { return true; }

QString NikitaBackend::modelName() const
{
    // What is actually answering, not what is equipped locally. The pill at the
    // top of the chat is the only place the user can see which brain a reply
    // came from, and it has to agree with the cost lines in the log.
    return apiModel();
}



// ---- Access filters -------------------------------------------------------
// Stored as the list of groups that are OFF, not the ones that are on: that
// way a group added in a later version arrives switched on, instead of
// showing up disabled without anyone having disabled it.
static const char *kFiltersOffKey = "nikita/filtersOff";

void NikitaBackend::loadFilters()
{
    const QStringList off = QSettings().value(QLatin1String(kFiltersOffKey)).toStringList();
    m_filtersOff = QSet<QString>(off.begin(), off.end());
}

void NikitaBackend::saveFilters()
{
    QStringList off(m_filtersOff.begin(), m_filtersOff.end());
    off.sort();   // stable order in the settings file, so diffs stay clean
    QSettings().setValue(QLatin1String(kFiltersOffKey), off);
}

void NikitaBackend::wipeAssistantData()
{
    // Local first: this always works, with or without a Flipper attached.
    // All three local files. Missing one of these is worse than missing none:
    // the card copy and the computer copy are mirrored on every startup, so a
    // survivor does not just linger -- it is written straight back onto the
    // card the next time the app runs, and the erase silently undoes itself.
    QFile::remove(nikitaHistoryPath());
    QFile::remove(nikitaMemoryPath());
    QFile::remove(nikitaSkillsPath());
    m_history = QJsonArray();
    m_skills.clear();
    m_memory.clear();
    m_toolRounds = 0;

    // The permissions the user granted NIKITA are part of what it knows about
    // them. Leaving an always-allow list behind would mean a re-enabled
    // assistant silently inherits consent given by a previous one. The API key
    // goes too: erase means disconnect, and a key left in settings would let the
    // next session reach the API without the user re-entering it.
    QSettings st;
    // kApiModelKey included: a model picked by the erased assistant is one of
    // its settings, and leaving it behind meant a fresh start silently came up
    // on whatever was last clicked instead of the default.
    for (const char *key : { "nikita/memory", "nikita/personality",
                             "nikita/hostRunAllowed", "nikita/hostActionAllowed",
                             kApiKeyKey, kApiKeyOkKey, kApiModelKey }) {
        st.remove(QLatin1String(key));
    }
    m_apiKeyStatus.clear();
    m_apiKeyMessage.clear();
    emit modelChanged();    // the pick is back to the default
    emit apiKeyChanged();   // the BRAIN panel drops back to "No kimi API key Found."
    // Erase means erase: the panel is holding its own copy of the conversation
    // in a ListModel, and clearing only the files left those bubbles on screen
    // to be re-shown the next time the assistant was switched on.
    emit historyCleared();

    // Every access filter OFF, and it persists. A fresh connection has to be
    // granted access deliberately -- the user turns each one back on by hand --
    // rather than inheriting the reach the erased assistant had. setAllFilters
    // writes to settings, so this survives the restart: re-opening the chat
    // shows NO ACCESS until a switch is flipped.
    setAllFilters(false);

    // Then the card. Best effort by design: no Flipper attached is not a
    // reason to refuse to erase what is on this computer, and the two files
    // are re-created from scratch the next time the assistant is enabled.
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (dev && dev->rpc()) {
        // The whole folder, recursively -- not just the two files inside it.
        // An empty /ext/nikita left on the card reads as "something is still
        // installed and running", which is the opposite of what someone who
        // just erased everything is asking for. It comes back on its own the
        // next time the assistant is switched on.
        auto *op = dev->rpc()->storageRemove(QByteArrayLiteral("/ext/nikita"), true);
        connect(op, &AbstractOperation::finished, this, [this, op]() {
            // "not found" is the expected result on a card that never had it,
            // so it is logged flat rather than as a failure.
            nikitaLog(op->isError()
                ? QStringLiteral("wipe: /ext/nikita -- %1").arg(op->errorString())
                : QStringLiteral("wipe: /ext/nikita removed"));
        });
    }

    nikitaLog(QStringLiteral("wipe: local files -> %1 | %2 | %3")
                  .arg(nikitaHistoryPath(), nikitaMemoryPath(), nikitaSkillsPath()));
    nikitaLog(QStringLiteral("wipe: still present after remove -> h=%1 m=%2 s=%3")
                  .arg(QFile::exists(nikitaHistoryPath()) ? QStringLiteral("YES") : QStringLiteral("no"),
                       QFile::exists(nikitaMemoryPath())  ? QStringLiteral("YES") : QStringLiteral("no"),
                       QFile::exists(nikitaSkillsPath())  ? QStringLiteral("YES") : QStringLiteral("no")));
    nikitaLog(QStringLiteral("wipe: assistant data erased"));
}

bool NikitaBackend::assistantEnabled() const { return m_assistantEnabled; }




QString NikitaBackend::apiModel() const
{
    const QString saved = QSettings().value(QLatin1String(kApiModelKey)).toString().trimmed();
    if (saved.isEmpty()) { return QString::fromUtf8(NIKITA_API_MODEL); }
    // A stored id the picker no longer offers -- left over from a build that
    // listed the account's whole catalog -- would leave all three rows drawn
    // unselected, with no way to tell what was actually answering. The default
    // is the honest answer in that case.
    for (const QVariant &v : apiModelChoices()) {
        if (v.toMap().value(QStringLiteral("id")).toString() == saved) { return saved; }
    }
    return QString::fromUtf8(NIKITA_API_MODEL);
}

void NikitaBackend::setApiModel(const QString &id)
{
    const QString want = id.trimmed();
    if (want.isEmpty() || want == apiModel()) { return; }
    QSettings().setValue(QLatin1String(kApiModelKey), want);
    nikitaLog(QStringLiteral("API model: %1").arg(want));
    emit modelChanged();     // header pill + this property
}

// A human label for a raw model id. The API hands back ids only, and
// "kimi-k2.7-code-highspeed" in a list of radio buttons is not a name anyone
// picked -- so known families get a real name and the id stays visible in the
// note underneath, which is what a person reads when two entries look alike.
static QString nikitaModelLabel(const QString &id)
{
    // Every id gets its OWN name. The account really does serve both
    // kimi-k2.7-code and kimi-k2.7-code-highspeed, and calling them both
    // "Kimi K2.7 Code" made the picker look like it had listed one model
    // twice -- two rows, same name, same note, one of them selected for no
    // visible reason. They are different models and they say so now.
    static const QMap<QString, QString> known {
        { QStringLiteral("kimi-k2.6"),                 QStringLiteral("Kimi K2.6") },
        { QStringLiteral("kimi-k2.7"),                 QStringLiteral("Kimi K2.7") },
        { QStringLiteral("kimi-k2.7-code"),            QStringLiteral("Kimi K2.7 Code") },
        { QStringLiteral("kimi-k2.7-code-highspeed"),  QStringLiteral("Kimi K2.7 Code Highspeed") },
        { QStringLiteral("kimi-k3"),                   QStringLiteral("Kimi K3") },
    };
    const auto it = known.constFind(id);
    if (it != known.constEnd()) { return it.value(); }

    // Anything unrecognised: tidy the id into something readable rather than
    // hiding the model. A new Kimi released next month should show up in this
    // picker on its own, not wait for this list to be edited.
    QString label = id;
    label.replace(QLatin1Char('-'), QLatin1Char(' '));
    label.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!label.isEmpty()) { label[0] = label[0].toUpper(); }
    return label;
}

// What is worth saying about a model beyond its name. Only the ones with a
// real trade-off get a note; everything else shows its id, which is the thing
// that actually distinguishes two similar entries.
static QString nikitaModelNote(const QString &id)
{
    if (id == QLatin1String("kimi-k2.6")) {
        return QStringLiteral("Stable but the best for run tools.");
    }
    if (id == QLatin1String("kimi-k2.7-code-highspeed")) {
        return QStringLiteral("Best for coding projects. Fastest of the K2.7 line.");
    }
    if (id.startsWith(QLatin1String("kimi-k2.7"))) {
        return id.contains(QLatin1String("code")) ? QStringLiteral("Best for coding projects.")
                                                  : QStringLiteral("current generation");
    }
    if (id == QLatin1String("kimi-k3")) {
        return QStringLiteral("newest model but may rate-limit is expected.");
    }
    return id;
}

// Three models, fixed, always in this order. Feeding the account's whole
// /v1/models catalog into this list was worse than it sounds: the account
// serves BOTH kimi-k2.7-code and kimi-k2.7-code-highspeed, so the picker grew
// two near-identical K2.7 rows and the shape of the panel changed the moment a
// key was accepted. This is a choice between three named options, not a mirror
// of a provider's inventory -- k2.6 is the default (GA, the account's full rate
// limit, which is what a multi-round tool turn needs), the highspeed K2.7 is
// the coding pick, and k3 is newest but throttled in preview.
//
// Anything the account adds later still WORKS -- apiModel() will send whatever
// id is stored -- it just is not advertised here. The notes are the user's own
// words; leave them as written.
QVariantList NikitaBackend::apiModelChoices() const
{
    static const QStringList ids{ QStringLiteral("kimi-k2.6"),
                                  QStringLiteral("kimi-k2.7-code-highspeed"),
                                  QStringLiteral("kimi-k3") };
    QVariantList out;
    for (const QString &id : ids) {
        out.append(QVariantMap{{"id", id},
                               {"label", nikitaModelLabel(id)},
                               {"note", nikitaModelNote(id)}});
    }
    return out;
}

// Environment first, stored setting second. Never cached in a member: read at
// the moment the header is built and dropped again, so the key is not sitting
// in this object waiting to be printed by some future debug dump.
bool NikitaBackend::deviceOverBle() const
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (!dev || !dev->deviceState()) { return false; }
    return dev->deviceState()->deviceInfo().isBle;
}

QString NikitaBackend::apiKey() const
{
    const QString fromEnv = qEnvironmentVariable(NIKITA_API_KEY_ENV).trimmed();
    if (!fromEnv.isEmpty()) { return fromEnv; }
    return QSettings().value(QLatin1String(kApiKeyKey)).toString().trimmed();
}

bool NikitaBackend::apiKeyPresent() const { return !apiKey().isEmpty(); }

QString NikitaBackend::apiKeySource() const
{
    if (!qEnvironmentVariable(NIKITA_API_KEY_ENV).trimmed().isEmpty()) {
        return QStringLiteral("environment");
    }
    if (!QSettings().value(QLatin1String(kApiKeyKey)).toString().trimmed().isEmpty()) {
        return QStringLiteral("settings");
    }
    return QString();
}

bool NikitaBackend::apiKeyWasVerified() const
{
    const QString key = apiKey();
    if (key.isEmpty()) { return false; }
    const QString seen = QSettings().value(QLatin1String(kApiKeyOkKey)).toString();
    if (seen.isEmpty()) { return false; }
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
    return hash == seen;
}

void NikitaBackend::markApiKeyVerified(const QString &key)
{
    QSettings().setValue(QLatin1String(kApiKeyOkKey),
        QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),
                                                     QCryptographicHash::Sha256).toHex()));
}

bool NikitaBackend::apiKeyValid() const
{
    if (apiKey().isEmpty()) { return false; }
    // A key the provider has accepted before stays good until it is told
    // otherwise. Only an outright rejection takes it back down -- a check that
    // could not reach the network says nothing about the key, and dropping the
    // assistant offline every time the wifi hiccups is its own bug.
    if (m_apiKeyStatus == QLatin1String("invalid")) { return false; }
    return apiKeyWasVerified();
}

QString NikitaBackend::apiKeyStatus() const  { return m_apiKeyStatus; }
QString NikitaBackend::apiKeyMessage() const { return m_apiKeyMessage; }

// One GET against /v1/models. 200 means the key is real AND hands back the
// model list in the same breath; 401/403 means it is not, and anything else --
// no network, a 500, a timeout -- is reported as "offline", because none of
// those are the key's fault and treating them as one would log the user out of
// their own assistant every time a train goes into a tunnel.
void NikitaBackend::checkApiKey()
{
    if (m_apiKeyCheck) { return; }   // one in flight is enough

    const QString key = apiKey();
    if (key.isEmpty()) {
        m_apiKeyStatus.clear();
        m_apiKeyMessage.clear();
        emit apiKeyChanged();
        return;
    }

    m_apiKeyStatus = QStringLiteral("checking");
    m_apiKeyMessage.clear();
    emit apiKeyChanged();

    QNetworkRequest req{QUrl(QString::fromUtf8(NIKITA_API_MODELS_URL))};
    req.setRawHeader("Authorization", QByteArray("Bearer ") + key.toUtf8());
    req.setTransferTimeout(15000);

    m_apiKeyCheck = m_net.get(req);
    connect(m_apiKeyCheck, &QNetworkReply::finished, this, [this, key]() {
        QNetworkReply *reply = m_apiKeyCheck;
        m_apiKeyCheck = nullptr;
        if (!reply) { return; }
        reply->deleteLater();

        const QByteArray body = reply->readAll();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (http == 200) {
            m_apiKeyStatus = QStringLiteral("valid");
            m_apiKeyMessage.clear();
            markApiKeyVerified(key);

            // Logged, not offered. The picker is a fixed three (see
            // apiModelChoices), so this is here to answer "what does this
            // account actually have" when a model id needs checking.
            QStringList ids;
            const QJsonArray data = QJsonDocument::fromJson(body).object()
                                        .value(QStringLiteral("data")).toArray();
            for (const QJsonValue &v : data) {
                const QString id = v.toObject().value(QStringLiteral("id")).toString().trimmed();
                if (!id.isEmpty() && !ids.contains(id)) { ids.append(id); }
            }
            nikitaLog(QStringLiteral("API key verified — %1 model(s) on this account: %2")
                          .arg(ids.size()).arg(ids.join(QStringLiteral(", "))));

        } else if (http == 401 || http == 403) {
            m_apiKeyStatus = QStringLiteral("invalid");
            // Whatever the provider said, so a wrong-account or expired-key
            // message reaches the person who can act on it.
            const QString detail = QJsonDocument::fromJson(body).object()
                                       .value(QStringLiteral("error")).toObject()
                                       .value(QStringLiteral("message")).toString();
            m_apiKeyMessage = detail.isEmpty() ? QStringLiteral("the API rejected this key")
                                               : detail;
            // Take back the remembered pass: this key is not usable any more,
            // and a stale "verified" would let it come up green after a restart.
            QSettings().remove(QLatin1String(kApiKeyOkKey));
            nikitaLog(QStringLiteral("API key rejected (HTTP %1): %2").arg(http).arg(m_apiKeyMessage));

        } else {
            m_apiKeyStatus = QStringLiteral("offline");
            m_apiKeyMessage = reply->errorString();
            nikitaLog(QStringLiteral("API key check could not reach the API: %1").arg(m_apiKeyMessage));
        }
        emit apiKeyChanged();
    });
}

void NikitaBackend::setApiKey(const QString &key)
{
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty()) { clearApiKey(); return; }
    QSettings().setValue(QLatin1String(kApiKeyKey), trimmed);
    // A newly typed key has proved nothing yet: drop any remembered pass so the
    // panel says "checking", not "ready", while the answer is on its way.
    QSettings().remove(QLatin1String(kApiKeyOkKey));
    // Length only. A log line is the one place a secret reliably escapes --
    // into a screenshot, a bug report, a pasted terminal buffer -- and knowing
    // that something arrived is the entire diagnostic value here.
    nikitaLog(QStringLiteral("API key saved (%1 characters). It is stored in this app's "
                             "settings; exporting %2 in your shell overrides it.")
                  .arg(trimmed.size()).arg(QLatin1String(NIKITA_API_KEY_ENV)));
    emit apiKeyChanged();
    checkApiKey();
}

QString NikitaBackend::revealApiKey() const
{
    return apiKey();
}

bool NikitaBackend::copyApiKeyToClipboard() const
{
    const QString key = apiKey();
    if (key.isEmpty()) { return false; }
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(key);
        // Length only, as everywhere else. That a copy happened is worth a line
        // in the trail; the characters are not.
        nikitaLog(QStringLiteral("API key copied to the clipboard (%1 characters).")
                      .arg(key.size()));
        return true;
    }
    return false;
}

void NikitaBackend::clearApiKey()
{
    QSettings().remove(QLatin1String(kApiKeyKey));
    QSettings().remove(QLatin1String(kApiKeyOkKey));
    m_apiKeyStatus.clear();
    m_apiKeyMessage.clear();
    nikitaLog(QStringLiteral("API key removed from this app's settings."));
    emit apiKeyChanged();
}


void NikitaBackend::setAssistantEnabled(bool on)
{
    if (on == m_assistantEnabled) { return; }
    m_assistantEnabled = on;
    QSettings().setValue(QLatin1String(kAssistantEnabledKey), on);
    // Turning it off mid-answer would otherwise leave a turn running behind a
    // panel nobody can see, still writing files.
    if (!on && m_thinking) { stopThinking(); }
    // Switching on recreates /ext/nikita right away rather than waiting for the
    // first fact to be saved -- so the card matches what the panel says.
    if (on) { syncMemoryToFlipper(); }
    nikitaLog(on ? QStringLiteral("assistant: enabled")
                 : QStringLiteral("assistant: disabled by the master switch"));
    emit assistantEnabledChanged();
}

QVariantList NikitaBackend::filters() const
{
    QVariantList out;
    for (int i = 0; i < NIKITA_FILTER_COUNT; ++i) {
        const QString id = QString::fromUtf8(NIKITA_FILTERS[i].id);
        out.append(QVariantMap{
            {QStringLiteral("id"),      id},
            {QStringLiteral("label"),   QString::fromUtf8(NIKITA_FILTERS[i].label)},
            {QStringLiteral("blurb"),   QString::fromUtf8(NIKITA_FILTERS[i].blurb)},
            {QStringLiteral("enabled"), !m_filtersOff.contains(id)},
        });
    }
    return out;
}

// 0 = none on, 1 = all on, -1 = a mix. The UI uses this to highlight which
// preset is in force without having to recount on its own side.
int NikitaBackend::filterPreset() const
{
    const int off = m_filtersOff.size();
    if (off == 0) { return 1; }
    if (off >= NIKITA_FILTER_COUNT) { return 0; }
    return -1;
}

void NikitaBackend::setFilter(const QString &id, bool on)
{
    if (!nikitaToolGroups().values().contains(id)) { return; }   // unknown id: ignore
    const bool was = !m_filtersOff.contains(id);
    if (was == on) { return; }
    if (on) { m_filtersOff.remove(id); } else { m_filtersOff.insert(id); }
    saveFilters();
    nikitaLog(QStringLiteral("access: %1 %2").arg(id, on ? QStringLiteral("allowed")
                                                         : QStringLiteral("blocked")));
    emit filtersChanged();
}

// The two presets: everything or nothing. Still the same toggles underneath --
// neither is a separate mode, just a shortcut for setting them all at once.
void NikitaBackend::setAllFilters(bool on)
{
    QSet<QString> next;
    if (!on) {
        for (int i = 0; i < NIKITA_FILTER_COUNT; ++i) {
            next.insert(QString::fromUtf8(NIKITA_FILTERS[i].id));
        }
    }
    if (next == m_filtersOff) { return; }
    m_filtersOff = next;
    saveFilters();
    nikitaLog(on ? QStringLiteral("access: FULL (every filter on)")
                 : QStringLiteral("access: NONE (every filter off)"));
    emit filtersChanged();
}

// A tool outside every group has no filter that could turn it off, so it
// passes. Only what is mapped can be blocked.
bool NikitaBackend::toolAllowed(const QString &tool) const
{
    const QString gid = nikitaToolGroups().value(tool);
    if (gid.isEmpty()) { return true; }
    return !m_filtersOff.contains(gid);
}

QSet<QString> NikitaBackend::allowedTools() const
{
    QSet<QString> out;
    for (auto it = nikitaToolGroups().cbegin(); it != nikitaToolGroups().cend(); ++it) {
        if (!m_filtersOff.contains(it.value())) { out.insert(it.key()); }
    }
    return out;
}







QStringList NikitaBackend::personalityPresets() const
{
    return { QStringLiteral("Default (Nikita)"),
             QStringLiteral("Chill helper"),
             QStringLiteral("Chaos gremlin"),
             QStringLiteral("Deadpan pro"),
             QStringLiteral("Sweet companion") };
}

void NikitaBackend::applyPreset(const QString &name)
{
    QString persona;
    if (name == QStringLiteral("Chill helper")) {
        persona = QStringLiteral("You are calm, warm and concise -- a laid-back, friendly helper. Light on snark, easy-going, genuinely helpful.");
    } else if (name == QStringLiteral("Chaos gremlin")) {
        persona = QStringLiteral("You are a chaotic, hyper, mischievous gremlin -- playful, unpredictable, high-energy and harmlessly unhinged. Chaos with a heart.");
    } else if (name == QStringLiteral("Deadpan pro")) {
        persona = QStringLiteral("You are dry, deadpan and professional -- efficient, subtle wit, minimal fluff. You just get things done.");
    } else if (name == QStringLiteral("Sweet companion")) {
        persona = QStringLiteral("You are a sweet, supportive companion -- encouraging, gentle, a genuine hype-buddy always in the user's corner.");
    }
    // "Default (Nikita)" clears the override -> the built-in default stands.
    QSettings().setValue(QStringLiteral("nikita/personality"), persona);
}

void NikitaBackend::applyNamePersonality()
{
    QSettings().setValue(QStringLiteral("nikita/personality"),
        QStringLiteral("Build and fully embody a personality inspired by your own name -- lean into "
                       "whatever character, vibe or theme the name evokes, and stay consistent in it."));
}

// A command that redraws its own progress line writes ANSI cursor/erase escapes
// and often Unicode block characters, none of which survives being dropped into
// a Text item -- Share Tech Mono has no glyph for ESC or the block-drawing
// range, so it renders as garbage. Strips the whole class (CSI/OSC sequences,
// C0 controls, U+2500-U+259F). Used for host_run's live output.
static QString sanitizeStatusLine(const QString &in)
{
    QString out;
    out.reserve(in.size());

    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);

        if (c == QChar(0x1B)) {                       // ESC
            if (i + 1 < in.size() && in.at(i + 1) == QLatin1Char('[')) {
                // CSI: parameter/intermediate bytes (0x20-0x3F), then a final
                // byte in 0x40-0x7E.
                int j = i + 2;
                while (j < in.size() && in.at(j).unicode() >= 0x20 && in.at(j).unicode() <= 0x3F) { ++j; }
                if (j < in.size()) { ++j; }           // final byte
                i = j - 1;
            } else if (i + 1 < in.size() && in.at(i + 1) == QLatin1Char(']')) {
                // OSC: runs until BEL, or until ST (ESC backslash).
                int j = i + 2;
                while (j < in.size() && in.at(j) != QChar(0x07) && in.at(j) != QChar(0x1B)) { ++j; }
                if (j < in.size() && in.at(j) == QChar(0x1B)) { ++j; }   // the backslash of ST
                i = j;
            } else {
                // Bare ESC + one byte (or a truncated sequence at a chunk edge).
                if (i + 1 < in.size()) { ++i; }
            }
            continue;
        }

        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7F) { out.append(QLatin1Char(' ')); continue; }  // other C0
        if (u >= 0x2500 && u <= 0x259F) { continue; }                          // box/block drawing
        out.append(c);
    }

    return out.simplified();
}

// A tool name is jargon. This is the same event in the words someone waiting
// on it would use. Anything unmapped falls back to "working" rather than
// leaking an identifier like host_mkdir into the window.
// "name(key=value, ...)" for a tool call, so the chat row can be expanded to
// show exactly what ran. Long values (a whole script) are trimmed -- the point
// is to see WHICH command and its key arguments, not to reprint a file.
static QString nikitaToolDetail(const QString &name, const QJsonObject &args)
{
    QStringList bits;
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key().startsWith(QLatin1Char('_'))) { continue; }   // internal recipe fields
        QString v = it.value().toVariant().toString().simplified();
        if (v.size() > 80) { v = v.left(77) + QStringLiteral("..."); }
        bits << QStringLiteral("%1=%2").arg(it.key(), v);
    }
    return QStringLiteral("%1(%2)").arg(name, bits.join(QStringLiteral(", ")));
}

static QString nikitaToolStatus(const QString &tool)
{
    static const QHash<QString, QString> phrases = {
        {QStringLiteral("host_write"),  QStringLiteral("writing the file")},
        {QStringLiteral("host_read"),   QStringLiteral("reading the file")},
        {QStringLiteral("host_list"),   QStringLiteral("listing the folder")},
        {QStringLiteral("host_find"),   QStringLiteral("searching")},
        {QStringLiteral("host_run"),    QStringLiteral("running the command")},
        {QStringLiteral("host_cd"),     QStringLiteral("changing folder")},
        {QStringLiteral("host_mkdir"),  QStringLiteral("creating the folder")},
        {QStringLiteral("host_delete"), QStringLiteral("deleting")},
        {QStringLiteral("host_move"),   QStringLiteral("moving")},
        {QStringLiteral("host_copy"),   QStringLiteral("copying")},
        {QStringLiteral("save_file"),   QStringLiteral("writing to the Flipper")},
        {QStringLiteral("read_file"),   QStringLiteral("reading from the Flipper")},
        {QStringLiteral("list_files"),  QStringLiteral("listing the Flipper")},
        {QStringLiteral("file_info"),   QStringLiteral("checking the file")},
        {QStringLiteral("make_dir"),    QStringLiteral("creating the folder on the Flipper")},
        {QStringLiteral("delete_file"), QStringLiteral("deleting on the Flipper")},
        {QStringLiteral("rename_file"), QStringLiteral("renaming on the Flipper")},
        {QStringLiteral("press_button"),QStringLiteral("pressing the button")},
        {QStringLiteral("run_cli"),     QStringLiteral("running it on the Flipper")},
        {QStringLiteral("remember"),    QStringLiteral("saving that to memory")},
        {QStringLiteral("list_memory"), QStringLiteral("checking memory")},
        {QStringLiteral("forget"),      QStringLiteral("forgetting that")},
    };
    return phrases.value(tool, QStringLiteral("working"));
}

QString NikitaBackend::turnStatus() const { return m_turnStatus; }

int NikitaBackend::turnElapsed() const
{
    if (m_turnStartedMs == 0) { return 0; }
    return int((QDateTime::currentMSecsSinceEpoch() - m_turnStartedMs) / 1000);
}

QString NikitaBackend::turnElapsedText() const
{
    const int s = turnElapsed();
    if (s < 60) { return QStringLiteral("%1s").arg(s); }
    return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60);
}

QString NikitaBackend::turnTokensText() const
{
    // The real total, once a reply has reported usage. Before that -- the whole
    // "thinking" stretch on a non-streamed API call -- fall back to the prompt
    // estimate so the footer is never blank, marked with ~ so it never claims
    // to be exact.
    const bool exact = m_turnTokens > 0;
    const int n = exact ? m_turnTokens : m_turnPromptEstTok;
    if (n <= 0) { return QString(); }
    const QString tilde = exact ? QString() : QStringLiteral("~");
    if (n < 1000) { return QStringLiteral("%1%2 tokens").arg(tilde).arg(n); }
    return QStringLiteral("%1%2k tokens").arg(tilde).arg(n / 1000.0, 0, 'f', 1);
}

QString NikitaBackend::turnCostText() const
{
    if (m_turnCostUsd <= 0.0) { return QString(); }
    // Four decimals, because three would round a one-cent turn to $0.010 and
    // the difference between a cheap turn and an expensive one is exactly the
    // thing this is here to show. Below a hundredth of a cent, say so rather
    // than printing $0.0000, which reads as free.
    if (m_turnCostUsd < 0.0001) { return QStringLiteral("<$0.0001"); }
    return QStringLiteral("$%1").arg(m_turnCostUsd, 0, 'f', 4);
}

void NikitaBackend::setTurnStatus(const QString &s)
{
    if (s == m_turnStatus) { return; }
    m_turnStatus = s;
    emit turnStatusChanged();
}

void NikitaBackend::setThinking(bool value)
{
    if (value == m_thinking) { return; }
    m_thinking = value;
    if (value) {
        m_turnStartedMs = QDateTime::currentMSecsSinceEpoch();
        setTurnStatus(QStringLiteral("thinking"));
        // One tick a second, only while a turn is live: the elapsed counter is
        // the part that proves it is moving even when the phrase does not change.
        if (!m_turnTicker.isActive()) { m_turnTicker.start(); }
    } else {
        // The feedback window opens here, AFTER the turn has already finished
        // and the lesson has already been filed automatically. It is a second
        // opinion, never a gate: ignoring it, closing the app or clearing the
        // chat changes nothing, because silence is not a verdict.
        if (m_turnRanAnyTool) {
            m_canRate = true;
            emit canRateChanged();
        }
        // Deferred, not called inline: this runs from inside the finish handler
        // and send() would re-enter the same machinery before it has unwound.
        QTimer::singleShot(0, this, [this]() { flushQueue(); });
        m_turnTicker.stop();
        // Snapshot BEFORE clearing the clock: emitReply runs after this and the
        // "done" line needs the totals, which are gone the moment m_turnStartedMs
        // is zeroed.
        m_lastTurnCost = QStringLiteral("%1 \u00b7 %2%3")
                             .arg(turnElapsedText(),
                                  turnTokensText().isEmpty() ? QStringLiteral("0 tokens")
                                                             : turnTokensText(),
                                  turnCostText().isEmpty()
                                      ? QString()
                                      : QStringLiteral(" \u00b7 ") + turnCostText());
        m_turnStartedMs = 0;
        setTurnStatus(QString());
    }
    emit thinkingChanged();
}

void NikitaBackend::reset()
{
    m_history = QJsonArray();
    m_toolRounds = 0;
    if (m_canRate) { m_canRate = false; emit canRateChanged(); }
    saveHistory();
    emit historyCleared();
}

// Public "clear the chat" entry point for the QML clear command.
void NikitaBackend::clearHistory()
{
    reset();
}

static QString nikitaHistoryPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) { dir = QDir::tempPath(); }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.json");
}

// Long-term memory: durable facts the user asked NIKITA to remember. Kept in a
// local file (always available, loaded into every system prompt so a forgetful
// small model "remembers" for free) and mirrored to the Flipper SD at
// /ext/nikita/memory.txt so it's portable with the device.
static QString nikitaMemoryPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) { dir = QDir::tempPath(); }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/memory.txt");
}

// Proven moves live apart from memory.txt on purpose. memory.txt holds facts
// ABOUT the user and is presented that way; this holds evidence of what this
// assistant has already managed to DO, and it is presented as worked examples.
// Mixing them would file "deleted a file once" among durable truths about a
// person, and would bury the demonstration where the model reads it as trivia
// instead of as a pattern to copy.
// Corrections live apart from wins. A mistake is not a move that failed -- the
// tool usually SUCCEEDED -- it is a move that did the wrong thing, and only the
// person who asked can tell the difference.
static QString nikitaMistakesPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) { dir = QDir::tempPath(); }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/mistakes.txt");
}

static QString nikitaSkillsPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) { dir = QDir::tempPath(); }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/actions-memory.txt");
}

// Did this tool result actually prove anything? Only an unambiguous yes
// counts -- an error, or a false verified/deleted/moved/copied flag, must
// NOT be learned, or the model repeats the failed shape with confidence.
static bool nikitaResultProves(const QString &result)
{
    const QJsonObject o = QJsonDocument::fromJson(result.toUtf8()).object();
    if (o.isEmpty() || o.contains(QStringLiteral("error"))) { return false; }
    if (o.contains(QStringLiteral("verified")) && !o.value("verified").toBool()) { return false; }
    if (o.contains(QStringLiteral("deleted"))  && !o.value("deleted").toBool())  { return false; }
    if (o.contains(QStringLiteral("moved"))    && !o.value("moved").toBool())    { return false; }
    if (o.contains(QStringLiteral("copied"))   && !o.value("copied").toBool())   { return false; }
    return true;
}

// One proven move per tool, most recent wins -- twenty examples of host_write
// teach nothing that one doesn't, and each competes for context.
// Write the turn's lessons, once the whole turn is visible: several tools for
// one request is a RECIPE, and filing it as independent pairings is the
// mistake that taught the model a folder-plus-file request was host_mkdir alone.
// Re-checks that the thing this call claimed to create still exists right
// now, since a later step in the same turn may have undone it. Only paths on
// this machine are audited -- a Flipper path would mean an RPC round trip
// through the same link the device tools already report over.
// A member, not a free function, because it has to resolve the path EXACTLY
// the way the tool did. The model writes "Desktop/note.txt"; host_write turns
// that into /Users/<user>/Desktop/note.txt through resolveAgentPath and saves
// it there. Checking the raw argument with QFileInfo::exists() asks whether
// "Desktop/note.txt" exists relative to the app's working directory -- it never
// does, so every relative-path write was judged a failure and thrown away.
// That is why actions-memory.txt stayed empty.
bool NikitaBackend::moveStillHolds(const QString &tool, const QJsonObject &args) const
{
    static const QStringList kCreators = {
        QStringLiteral("host_write"), QStringLiteral("host_mkdir"),
        QStringLiteral("host_copy"),  QStringLiteral("host_move"),
    };
    if (!kCreators.contains(tool)) { return true; }   // nothing to check against

    // host_move and host_copy land at the destination, not the source.
    QString path = args.value(QStringLiteral("to")).toString();
    if (path.isEmpty()) { path = args.value(QStringLiteral("destination")).toString(); }
    if (path.isEmpty()) { path = args.value(QStringLiteral("path")).toString(); }
    if (path.isEmpty()) { return true; }

    // Same resolution the tool used. mustExist=false: we are asking whether the
    // artifact is there, so the answer must come from the check below, not from
    // the resolver refusing to name a path that is missing.
    const QString abs = resolveAgentPath(path, false);
    return QFileInfo::exists(abs.isEmpty() ? path : abs);
}

void NikitaBackend::flushPendingMoves()
{
    if (m_pendingMoves.isEmpty()) { return; }

    // Audit before recording. Every step of the recipe has to still hold, or
    // the recipe is not one: a chain whose middle step was undone is a chain
    // that does not reproduce, and teaching it would be teaching a wrong move
    // that then persists to the card.
    for (const auto &m : m_pendingMoves) {
        if (moveStillHolds(m.first, m.second)) { continue; }
        nikitaLogAs(assistantName(),
                   QStringLiteral("not learning: %1 reported success but the result is not there")
                       .arg(m.first));
        m_pendingMoves.clear();
        return;
    }

    if (m_pendingMoves.size() == 1) {
        recordProvenMove(m_pendingMoves.first().first, m_pendingMoves.first().second);
        m_pendingMoves.clear();
        return;
    }

    // Several steps: record the sequence under the tool that started it, with
    // the chain spelled out, so the next time a request looks like this one the
    // prompt says "this took three calls" rather than "this took that call".
    // Each step is named WITH ITS TARGET, not just the tool name. A recipe that
    // read "host_write -> host_run -> save_file" recorded that three calls
    // happened but hid WHAT they produced -- so a fib task that also generated a
    // BadUSB looked, in memory, like it had only written the python file. Naming
    // the target of every step makes the stored recipe show the whole thing.
    auto stepLabel = [](const QString &tool, const QJsonObject &a) -> QString {
        QString target = a.value(QStringLiteral("path")).toString();
        if (target.isEmpty()) { target = a.value(QStringLiteral("command")).toString(); }
        if (target.isEmpty()) { target = a.value(QStringLiteral("to")).toString(); }
        target = target.simplified();
        if (target.size() > 60) { target = target.left(57) + QStringLiteral("..."); }
        return target.isEmpty() ? tool : QStringLiteral("%1(%2)").arg(tool, target);
    };
    QStringList chain;            // tool names only, for the one-line log
    QStringList chainDetailed;    // tool(target), for the stored recipe
    for (const auto &m : m_pendingMoves) {
        chain += m.first;
        chainDetailed += stepLabel(m.first, m.second);
    }
    QJsonObject args = m_pendingMoves.first().second;
    args.insert(QStringLiteral("_then"), chainDetailed.mid(1).join(QStringLiteral(" -> ")));
    args.insert(QStringLiteral("_steps"), chain.size());
    // The final artifact called out by itself, so "what did this produce" is
    // answerable at a glance even when the chain is long.
    args.insert(QStringLiteral("_produced"),
                stepLabel(m_pendingMoves.last().first, m_pendingMoves.last().second));
    recordProvenMove(m_pendingMoves.first().first, args);

    nikitaLogAs(assistantName(),
               QStringLiteral("learned a %1-step recipe: %2")
                   .arg(chain.size()).arg(chainDetailed.join(QStringLiteral(" -> "))));
    m_pendingMoves.clear();
}

void NikitaBackend::recordProvenMove(const QString &tool, const QJsonObject &args)
{
    const QString ask = m_lastUserText.simplified().left(120);
    if (ask.isEmpty() || tool.isEmpty()) { return; }

    // The memory tools don't belong in here. actions-memory.txt exists to show
    // the model what it has managed to DO -- files written, folders made,
    // commands run. remember() succeeding proves nothing about capability; the
    // fact it saved is already in memory.txt, and copying the phrasing across
    // duplicates it into a second file while eating room in a prompt that is
    // already tight.
    static const QStringList kNotMoves = {
        QStringLiteral("remember"), QStringLiteral("forget"), QStringLiteral("list_memory")
    };
    if (kNotMoves.contains(tool)) { return; }

    QString shape;
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        QString v = it.value().toVariant().toString().simplified();
        // The path is the lesson; the file's contents are not. Storing a whole
        // DuckyScript here would blow up the prompt to teach nothing.
        if (v.size() > 80) { v = v.left(77) + QStringLiteral("..."); }
        shape += QStringLiteral("%1=%2 ").arg(it.key(), v);
    }

    const QString line = QStringLiteral("%1\t%2\t%3").arg(tool, ask, shape.trimmed());

    // Accumulate, don't replace.
    //
    // This used to drop every previous entry for the tool, so a second
    // host_write erased the first and the file never grew past a handful of
    // lines -- it looked like it wasn't storing anything at all. Several
    // examples of the same tool are worth keeping: "save to Desktop" and "save
    // to /ext/badusb" are the same call with genuinely different shapes, and a
    // small model generalises better from two than from one.
    //
    // What still gets dropped is a true duplicate: same tool, same phrasing,
    // same arguments. Repeating a request should not fill the list with copies.
    m_skills.removeAll(line);

    // Bounded per tool as well as overall, so one busy tool cannot crowd the
    // others out of a context window that is already tight.
    int sameTool = 0;
    for (int i = m_skills.size() - 1; i >= 0; --i) {
        if (m_skills.at(i).section('\t', 0, 0) != tool) { continue; }
        if (++sameTool >= 3) { m_skills.removeAt(i); }
    }

    m_skills.append(line);
    while (m_skills.size() > 24) { m_skills.removeFirst(); }
    m_lastProvenTool = tool;
    saveProvenMoves();
    syncMemoryToFlipper();   // straight to the card, like a learned fact
    nikitaLogAs(assistantName(), QStringLiteral("learned: %1 works for \"%2\"").arg(tool, ask));
}

void NikitaBackend::saveProvenMoves() const
{
    // Same reasoning as the card: never truncate the local copy to nothing.
    if (m_skills.isEmpty()) { return; }
    QFile f(nikitaSkillsPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { return; }
    f.write(m_skills.join(QLatin1Char('\n')).toUtf8());
    f.close();
}

void NikitaBackend::loadProvenMoves()
{
    QFile f(nikitaSkillsPath());
    if (!f.open(QIODevice::ReadOnly)) { return; }
    m_skills = QString::fromUtf8(f.readAll())
                   .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    f.close();
}

// The user says it didn't actually work. Their eyes beat any check this code can
// run -- a file can be written, verified, and still be the wrong file in the
// wrong place. Drop the lesson rather than keep teaching it.
int NikitaBackend::queuedCount() const { return m_queued.size(); }

QString NikitaBackend::queuedPreview() const
{
    return m_queued.join(QStringLiteral(" / ")).left(120);
}

void NikitaBackend::queueMessage(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) { return; }
    m_queued.append(t);
    nikitaLog(QStringLiteral("queued (%1): %2").arg(m_queued.size()).arg(t.left(80)));
    emit queuedChanged();
}

void NikitaBackend::clearQueue()
{
    if (m_queued.isEmpty()) { return; }
    m_queued.clear();
    emit queuedChanged();
}

// Everything typed during the turn, delivered as ONE message. Sending them one
// at a time would make each queued line pay its own full round on a model that
// takes minutes per turn; joined, they arrive as the single follow-up the
// person was actually composing.
void NikitaBackend::flushQueue()
{
    if (m_queued.isEmpty() || m_thinking || !m_assistantEnabled) { return; }
    const QString joined = m_queued.join(QLatin1Char('\n'));
    m_queued.clear();
    emit queuedChanged();
    nikitaLog(QStringLiteral("queue flushed -> sending %1 char(s)").arg(joined.size()));
    send(joined, m_lastDeviceContext);
}

bool NikitaBackend::canRate() const { return m_canRate; }

void NikitaBackend::loadMistakes()
{
    QFile f(nikitaMistakesPath());
    if (!f.open(QIODevice::ReadOnly)) { return; }
    m_mistakes = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    f.close();
}

void NikitaBackend::saveMistakes()
{
    QFile f(nikitaMistakesPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(m_mistakes.join(QLatin1Char('\n')).toUtf8());
    }
}

void NikitaBackend::rateLastAction(bool worked, const QString &note)
{
    m_canRate = false;
    emit canRateChanged();

    if (worked) {
        // The lesson was already filed when the tool verified itself; a thumbs
        // up just confirms it and closes the window.
        if (!m_lastProvenTool.isEmpty()) {
            nikitaLogAs(assistantName(),
                       QStringLiteral("confirmed: %1 did what was asked").arg(m_lastProvenTool));
        }
        m_lastProvenTool.clear();
        return;
    }

    // Wrong. Two separate things have to happen, and the second one is the part
    // that was missing: forgetting a bad lesson stops it being recommended, but
    // nothing stops the model reaching the same wrong conclusion again. So the
    // correction is written down and read back on every turn.
    if (!m_lastProvenTool.isEmpty()) {
        const QString tool = m_lastProvenTool;
        m_skills.removeIf([&tool](const QString &l) { return l.section('\t', 0, 0) == tool; });
        saveProvenMoves();
        syncMemoryToFlipper();   // the card must forget it too
        nikitaLogAs(assistantName(), QStringLiteral("unlearned: %1 (you said it didn't work)").arg(tool));
    }

    const QString did = m_lastProvenTool.isEmpty() ? QStringLiteral("what you did")
                                                   : m_lastProvenTool;
    m_mistakes.removeIf([this](const QString &l) { return l.section('\t', 0, 0) == m_lastUserText; });
    m_mistakes.append(QStringLiteral("%1\t%2\t%3")
                          .arg(m_lastUserText, did, note.trimmed().left(200)));
    // Capped: a correction is strong signal, but every one of them is sent on
    // every turn, and the oldest stop matching anything the user still does.
    while (m_mistakes.size() > 12) { m_mistakes.removeFirst(); }
    saveMistakes();
    nikitaLogAs(assistantName(),
               note.trimmed().isEmpty()
                   ? QStringLiteral("correction filed: \"%1\" -> %2 was wrong").arg(m_lastUserText, did)
                   : QStringLiteral("correction filed: \"%1\" -> %2 was wrong (%3)")
                         .arg(m_lastUserText, did, note.trimmed()));
    m_lastProvenTool.clear();
}

void NikitaBackend::loadHistory()
{
    QFile f(nikitaHistoryPath());
    if (!f.open(QIODevice::ReadOnly)) { return; }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isArray()) { m_history = doc.array(); }
}

void NikitaBackend::saveHistory()
{
    // Persist only the real conversation -- user prompts + NIKITA's final replies.
    // Skip tool plumbing and the auto health-check so memory stays lean.
    QJsonArray convo;
    bool skipNextAssistant = false;
    for (const QJsonValue &v : m_history) {
        const QJsonObject o = v.toObject();
        const QString role = o.value("role").toString();
        const QString content = o.value("content").toString();

        if (role == QLatin1String("user")) {
            if (content.contains(QStringLiteral("in-character health check"))) {
                skipNextAssistant = true;  // drop the auto health-check + its reply
                continue;
            }
            skipNextAssistant = false;
            convo.append(o);
        } else if (role == QLatin1String("assistant") && o.value("content").isString() &&
                   !content.isEmpty() && !o.contains(QStringLiteral("tool_calls"))) {
            if (skipNextAssistant) { skipNextAssistant = false; continue; }
            // Defence in depth: if a "reply" is really leaked tool-call JSON,
            // don't persist it -- otherwise it becomes a few-shot example that
            // teaches NIKITA to keep printing calls instead of making them.
            if (!salvageToolCalls(content).isEmpty()) { continue; }
            convo.append(QJsonObject{{"role", "assistant"}, {"content", content}});
        }
    }

    const int cap = 30;
    while (convo.size() > cap) { convo.removeAt(0); }

    QFile f(nikitaHistoryPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(convo).toJson(QJsonDocument::Compact));
    }
}

// The assistant takes the connected Flipper's name, falling back to the one set
// during setup. Used by the prompt and by the action log.
void NikitaBackend::setCli(FlipperCli *cli)
{
    m_cli = cli;
    if (!m_cli) { return; }
    // The CLI panel talks to the firmware over the serial line and never goes
    // through RPC, so nothing here can see what it did -- including an
    // "edit /ext/nikita/memory.txt". Re-read the card's copy when the panel is
    // handed back, and take it as the truth.
    connect(m_cli, &FlipperCli::openChanged, this, [this]() {
        if (m_cli && !m_cli->isOpen()) { loadPortableMemory(); }
    });
}

void NikitaBackend::logAction(const QString &what) const
{
    nikitaLog(what);
}

QString NikitaBackend::assistantName() const
{
    // The assistant is Nikita; the Flipper has its own name, set on the device.
    // Deriving one from the other meant a device with no name left the assistant
    // without one either.
    return QStringLiteral("Nikita");
}

// Defined further down, next to the rest of the path handling it belongs with.
// Declared here because systemPrompt() reads the user's real folder locations
// into every turn, and that happens earlier in this file than the definition.
static QString nikitaWellKnownDir(const QString &name);

// A stored move is "host_write \t <what was asked> \t path=/Users/x/t.txt content=ONE".
// This turns the tool half into a sentence the assistant could have spoken:
// "I created t.txt and saved it to /Users/x". Read back into the prompt, a
// sentence about a finished task primes the model to do the task; a log of
// function names primes it to think in function names.
static QString nikitaPlainMove(const QString &tool, const QString &shape)
{
    const auto valueOf = [&shape](const char *key) -> QString {
        const QString k = QString::fromLatin1(key) + QLatin1Char('=');
        int i = shape.indexOf(k);
        if (i < 0) { return QString(); }
        // Only a real key match: "path=" must not be found inside "filepath=".
        if (i > 0 && !shape.at(i - 1).isSpace()) { return QString(); }
        const int start = i + k.size();
        int end = shape.indexOf(QLatin1Char(' '), start);
        if (end < 0) { end = shape.size(); }
        return shape.mid(start, end - start);
    };

    const QString path = valueOf("path");
    const QString to   = valueOf("to");
    const QString cmd  = valueOf("command");
    const QString fact = valueOf("fact");
    const QString name = QFileInfo(path).fileName();
    const QString dir  = QFileInfo(path).path();

    if (tool == QLatin1String("host_write") || tool == QLatin1String("save_file")) {
        return name.isEmpty() ? QStringLiteral("I wrote the file.")
             : QStringLiteral("I created %1 and saved it to %2.").arg(name, dir);
    }
    if (tool == QLatin1String("host_mkdir") || tool == QLatin1String("make_dir")) {
        return path.isEmpty() ? QStringLiteral("I created the folder.")
                              : QStringLiteral("I created the folder %1.").arg(path);
    }
    if (tool == QLatin1String("host_delete") || tool == QLatin1String("delete_file")) {
        return path.isEmpty() ? QStringLiteral("I deleted it.")
                              : QStringLiteral("I deleted %1.").arg(path);
    }
    if (tool == QLatin1String("host_move") || tool == QLatin1String("rename_file")) {
        return to.isEmpty() ? QStringLiteral("I moved it.")
                            : QStringLiteral("I moved it to %1.").arg(to);
    }
    if (tool == QLatin1String("host_copy")) {
        return to.isEmpty() ? QStringLiteral("I copied it.")
                            : QStringLiteral("I copied it to %1.").arg(to);
    }
    if (tool == QLatin1String("host_run") || tool == QLatin1String("run_cli")) {
        return cmd.isEmpty() ? QStringLiteral("I ran the command.")
                             : QStringLiteral("I ran %1.").arg(cmd);
    }
    if (tool == QLatin1String("host_read") || tool == QLatin1String("read_file")) {
        return path.isEmpty() ? QStringLiteral("I read the file.")
                              : QStringLiteral("I read %1.").arg(path);
    }
    if (tool == QLatin1String("host_list") || tool == QLatin1String("list_files")) {
        return path.isEmpty() ? QStringLiteral("I listed the folder.")
                              : QStringLiteral("I listed %1.").arg(path);
    }
    if (tool == QLatin1String("host_find")) { return QStringLiteral("I searched for it."); }
    if (tool == QLatin1String("host_cd"))   { return QStringLiteral("I changed folder."); }
    if (tool == QLatin1String("press_button")) { return QStringLiteral("I pressed the button on the Flipper."); }
    if (tool == QLatin1String("file_info"))    { return QStringLiteral("I checked the file."); }
    if (tool == QLatin1String("remember")) {
        return fact.isEmpty() ? QStringLiteral("I saved that to memory.")
                              : QStringLiteral("I remembered that %1.").arg(fact);
    }
    if (tool == QLatin1String("forget"))      { return QStringLiteral("I forgot it."); }
    if (tool == QLatin1String("list_memory")) { return QStringLiteral("I checked what I remember."); }
    return QStringLiteral("I did it.");
}

QString NikitaBackend::systemPrompt() const
{
    QString sys = QString::fromUtf8(NIKITA_SYSTEM);
    // The BadUSB identity line is one editable constant, dropped into both the
    // rule and the worked example so they can never disagree about it.
    sys.replace(QLatin1String("{{BADUSB_ID}}"), QLatin1String(NIKITA_BADUSB_ID));

    // On plain conversation turns, cut the whole tool/device manual out of the
    // prompt. Leaving it in teaches the model the call syntax, and weak
    // tool-callers then TYPE things like save_file(...) instead of just talking.
    if (!m_turnNeedsTools) {
        const int from = sys.indexOf(QStringLiteral("DEVICE ACCESS --"));
        const int to   = sys.indexOf(QStringLiteral("CONVERSATION vs ACTION"));
        if (from > 0 && to > from) {
            sys.remove(from, to - from);
        }
        sys += QStringLiteral("\n\nTHIS TURN IS CONVERSATION: this particular message does not need a "
                              "tool, so reply in plain words only -- short and direct. Do NOT write any "
                              "function call, code, script or file path. Just answer.\n"
                              "This says nothing about what you CAN do. Your USB link, the full Flipper "
                              "CLI, the SD-card file tools and the buttons are all still connected and "
                              "are used the moment a message actually asks for one. If you are asked "
                              "whether you can reach the CLI, the device or the SD card, the answer is "
                              "YES -- say so. NEVER claim you lack access, and NEVER tell the user to go "
                              "run something themselves.");
    }

    // Trim the prompt to what THIS turn actually needs.
    //
    // The arithmetic that forces this: num_ctx is finite, and a measured action
    // turn costs the model 1700-4300 tokens of <think> before it emits a single
    // tool call. The <think> block is not optional -- the model's own chat
    // template opens it on every assistant turn, which is why /no_think and
    // "think": false never did anything. So the window has to hold the prompt,
    // the tool schemas AND several thousand tokens of reasoning.
    //
    // When it does not fit, the failure is silent and looks like stupidity
    // rather than an error: llama-server runs with --context-shift, so the
    // OLDEST tokens get discarded to make room -- and the oldest tokens are the
    // system prompt. The model then answers from a prompt whose opening half it
    // can no longer see. That is what "it hallucinated" looks like from here.
    //
    // So this is not an optimisation and it is not a mode. It is the difference
    // between the instructions being present at inference time and not.
    {
        // Radio, NFC, IR, DuckyScript and the button/GPIO manual are ~2000
        // tokens of reference that only pay for themselves when the turn is
        // actually about the Flipper. On "create a file on my Desktop" they are
        // pure ballast. DEVICE ACCESS and WHICH MACHINE stay in either way --
        // those are what stop it writing a host file onto the SD card.
        if (!m_turnIsDevice) {
            const int from = sys.indexOf(QStringLiteral("DEVICE CONTROL --"));
            const int to   = sys.indexOf(QStringLiteral("POWER MOVES --"));
            if (from > 0 && to > from) { sys.remove(from, to - from); }
        }
        // POWER MOVES goes on every turn. Most of it is encouragement -- "think
        // like an operator", "go beyond the obvious", "favor the robust,
        // complete artifact" -- addressed to a 4B model whose documented
        // failure mode is running long. Four of its lines are load-bearing and
        // are restated here in a fifth of the space; the rest is telling a
        // rambler to ramble.
        const int pfrom = sys.indexOf(QStringLiteral("POWER MOVES --"));
        const int pto   = sys.indexOf(QStringLiteral("CONVERSATION vs ACTION"));
        if (pfrom > 0 && pto > pfrom) {
            sys.remove(pfrom, pto - pfrom);
            sys.insert(pfrom, QStringLiteral(
                "OPERATING RULES:\n"
                "- run_cli is the FLIPPER. host_run and host_* are THIS COMPUTER. Mixing them up is "
                "the most common way to be confidently wrong.\n"
                "- Names are DATA: use the exact spelling the user typed, capitals and all.\n"
                "- Never claim an action happened unless the tool actually ran and succeeded.\n"
                "- You CAN drive the Flipper physically through run_cli (vibro, led, reboot, GPIO). "
                "Never say you cannot perform physical actions.\n\n"));
        }
    }

    // Only on tool turns: on a plain-chat turn the whole device manual is cut
    // out just above, and putting a command list back would undo that.
    //
    if (m_turnNeedsTools) { sys += cliReferenceForPrompt(); }

    // Deterministic result handoff. When a file you save must contain the
    // output of a command you run THIS turn, do not read the number off the
    // screen and retype it -- write the literal token {{LAST_RESULT}} where the
    // value goes, and the app fills in the exact bytes the command printed. One
    // digit typed wrong is a broken artifact; this removes the chance entirely.
    if (m_turnNeedsTools) {
        sys += QStringLiteral(
            "\n\nRESULT HANDOFF -- when a file you save has to contain the OUTPUT of a "
            "command you run this turn (a computed number, a captured value, an ip), do NOT "
            "transcribe that value yourself. Run the command first, then write the token "
            "{{LAST_RESULT}} in the file exactly where the value belongs -- the app replaces "
            "it with the exact output of your most recent command. Example: to put a computed "
            "number into a script, the line is  STRING {{LAST_RESULT}}  (or the value sits "
            "inside a longer string). This is only for a value your OWN command produced this "
            "turn; type any other literal text normally.");
    }

    // Anchor the most recently saved file so an edit ("make it fancy", "add X",
    // "change the delay") rewrites THAT file instead of inventing a new name.
    // Weak models otherwise treat every refinement as a brand-new artifact, and
    // the SD card fills with near-duplicate scripts doing the same thing.
    if (m_turnNeedsTools && !m_lastSavedPath.isEmpty()) {
        sys += QStringLiteral(
            "\n\nMOST RECENT FILE you saved this session: \"%1\".\n"
            "- If this message asks you to CHANGE, improve, fix, extend, restyle or "
            "otherwise iterate on what you just made (\"make it fancy\", \"add a delay\", "
            "\"now do X too\"), call save_file with THIS SAME path and write the full updated "
            "contents. Overwriting is correct -- it is the same artifact, one file.\n"
            "- Do NOT invent a new filename for a variation of the same thing. A new name "
            "is only for a genuinely DIFFERENT artifact the user asked for.\n"
            "- Keep the name stable across edits: \"fancy_\", \"v2_\", \"final_\" prefixes are "
            "clutter. Same purpose -> same file.").arg(m_lastSavedPath);
    }

    // Optional personality chosen in the setup wizard (fresh users). If unset,
    // the built-in personality above stands -- a hand-edited NIKITA_SYSTEM is
    // never overridden unless someone deliberately picks a preset.
    const QString persona = QSettings().value(QStringLiteral("nikita/personality")).toString();
    if (!persona.isEmpty()) {
        sys += QStringLiteral("\n\nPERSONALITY -- adopt THIS character (every operational rule above "
                              "still fully applies): ") + persona;
    }

    // assistantName() is a fixed "Nikita" (see its own comment for why the old
    // derive-it-from-the-connected-device scheme was dropped). This block used
    // to read "your name is %1 (NOT NIKITA...)" back when %1 could be some
    // other device name -- now that it can't, that parenthetical had become
    // self-contradictory nonsense repeated on every single turn, for every
    // user: "your name is Nikita (NOT NIKITA...)". Fixed to state the identity
    // plainly instead of disambiguating against a case that no longer exists.
    const QString name = assistantName();
    if (!name.isEmpty()) {
        sys += QStringLiteral("\n\nYOUR NAME -- IMPORTANT, never lose this: you are %1. Not \"an AI "
            "assistant\", not \"a language model\", not \"a chatbot\" -- %1. Introduce yourself as %1. "
            "ALWAYS speak in the FIRST PERSON -- say \"I\", \"me\", \"my\", never talk about yourself in "
            "the third person. NEVER write things like \"%1 is on it\" or \"%1 will handle it\"; say "
            "\"I'm on it\", \"I've got it\". You ARE %1, so refer to yourself as \"I\", the way a person "
            "named %1 says \"I\" not their own name. This holds for the whole conversation, no matter "
            "how long it runs or how many tool calls you've made -- round twelve gets the same identity "
            "as round one, not a flattened, generic assistant voice.").arg(name);
    }

    if (!agentReady()) {
        sys += QStringLiteral(
            "\n\nTHIS COMPUTER IS OUT OF REACH THIS SESSION:\n"
            "- You have NO tools for the machine NIKITA is running on. No reading it, no writing "
            "to it, no shell, no Desktop, no Downloads, no Documents, no home folder.\n"
            "- Everything you can touch is on the Flipper: /ext and /int.\n"
            "- Asked to put something on their Desktop or anywhere else on their computer, say "
            "plainly that computer access is off and that Agent mode in setup turns it on. Offer "
            "to save it to the Flipper instead. Do NOT claim you saved it, do NOT invent a path, "
            "and do NOT report a working directory -- you do not have one.\n");
    }

    // The full workspace manual is ~1300 tokens of tool documentation. It earns
    // that on a turn that will actually call a tool; on a plain-chat turn no
    // host tool is even offered, so all it does is crowd out the model's room to
    // think. The chat turn still gets the LOCATIONS, because "where are you?"
    // and "what's my Desktop path?" are conversation, not tool calls, and
    // answering those from memory is how a wrong path gets invented.
    if (agentReady() && !m_turnNeedsTools) {
        sys += QStringLiteral(
            "\n\nWHERE YOU ARE ON THIS COMPUTER (regenerated every turn -- never stale, "
            "and never needs a tool call to find out):\n"
            "- Current folder: \"%1\"\n"
            "- Home: \"%2\"   Workspace: \"%3\"   This computer: %4\n"
            "- Desktop: \"%5\"   Downloads: \"%6\"   Documents: \"%7\"\n"
            "- These are THIS user's real folders. Never assemble a path from what you "
            "remember; read it from the lines above.")
            // agentBaseDir() rather than m_agentRoot: an unconfigured workspace
            // is home, not an empty string printed into the prompt as a path.
            .arg(agentCwd(), QDir::homePath(), agentBaseDir(), QSysInfo::prettyProductName(),
                 nikitaWellKnownDir(QStringLiteral("desktop")),
                 nikitaWellKnownDir(QStringLiteral("downloads")),
                 nikitaWellKnownDir(QStringLiteral("documents")));
    }

    if (agentReady() && m_turnNeedsTools) {
        sys += QStringLiteral(
            "\n\nHOST WORKSPACE -- you can edit and test your OWN source code:\n"
            "- A workspace folder on THIS computer is wired up: \"%1\". It holds your own qFlipper/NIKITA source.\n"
            "- Your reach on this computer is the WHOLE FILESYSTEM, not a sandbox. Paths may be absolute, may start with ~ for home, or may be relative. There is no folder you have to ask permission for.\n"
            "\nWHERE YOU ARE RIGHT NOW -- read this before touching any path:\n"
            "- Current folder: \"%2\"\n"
            "- Home: \"%3\"   Workspace: \"%1\"   This computer: %4\n"
            "- This user's real folders, resolved on THIS machine -- use these exact paths, never a guess:\n"
            "    Desktop: \"%5\"\n"
            "    Downloads: \"%6\"\n"
            "    Documents: \"%7\"\n"
            "- They are not the same on every computer. Someone else running NIKITA has a different user name, possibly a different operating system, and possibly a Desktop that isn't called Desktop. Never assemble a path like \"/Users/<name>/Desktop\" from what you have seen before -- read it from the lines above, or write \"Desktop/file.txt\" and let it be resolved for you.\n"
            "- A relative path resolves from the CURRENT FOLDER above, not from the workspace and not from home. \"notes.txt\" means \"%2/notes.txt\". If that is not what you meant, say the absolute path or move first.\n"
            "- host_cd(path) moves you and answers with where you landed and what is in it; host_cd with no path just tells you where you are. \"..\" goes up. The move STICKS for the rest of the conversation, and host_run starts there too.\n"
            "- The line above is regenerated every single turn, so it is never stale. Trust it over anything you remember from earlier in the conversation -- if you moved ten messages ago, this already reflects it, and you do NOT need a tool call to find out where you are.\n"
            "- Before writing, deleting or moving anything, be sure the folder is the one you mean. Reading a listing costs one call; writing into the wrong tree costs the user their afternoon. When a path came from the user and you are not certain how it resolves, resolve it out loud in your reply as you act.\n"
            "- host_list(path), host_read(path), host_write(path, content): browse, read and edit files anywhere. host_write creates missing folders and OVERWRITES the whole file, so read it first, then write the full new contents.\n"
            "- Any path may start with a folder NAME instead of a location: \"Desktop/notes.txt\", \"Downloads/x.zip\", \"Documents/plan.md\". Those resolve to this user's real folders wherever they happen to live. It is the safest way to write, because it cannot be wrong on someone else's machine.\n"
            "- host_mkdir(path), host_move(from, to), host_copy(from, to), host_delete(path), host_find(path, pattern): create, move, copy, delete and search. Use these instead of shelling out to mkdir/mv/cp/rm/find -- they report what actually happened, where a shell command just hands you an exit code.\n"
            "- host_run(command, cwd): run a shell command and get the exit code plus combined stdout/stderr. It BLOCKS until the command finishes, so prefer targeted commands. Reach for it when no typed tool fits, not as the default.\n"
            "- Reads (host_list, host_read, host_find, host_cd) run at once and are just shown to the user as they happen -- you never have to describe those before doing them, just do it. Every tool that CHANGES something (host_write, host_mkdir, host_move, host_copy, host_delete, host_run) instead pauses for a person to approve it on screen first, so the call simply takes longer to answer -- that is not a failure, keep waiting on it like any other tool result. If they decline, the result says so plainly; report that back honestly instead of trying the same call again or claiming it worked.\n"
            "- The reach is real, so the care has to be too. host_delete is not undoable and there is no bin to fish things out of. Delete what was asked for and nothing adjacent; when a request would remove more than the user clearly named, do the named part and say what you left alone.\n"
            "- The workspace folder is not a boundary, just a starting point: it is where bare relative paths land and where host_run begins. Say the path you actually mean.\n"
            "- Your own core lives in application/nikitabackend.cpp + .h and application/components/ under that workspace. To fix a bug in yourself: host_read the file, host_write the corrected version, then host_run the build and read the errors.\n"
            "- Nothing here is blocked, so nothing here is undone for you either. Never claim you edited a file you didn't, and never report a path you didn't get back from a tool this turn. Say what you changed and where, plainly.\n"
            "- A tool that comes back with \"error\" did NOT happen. Read the error text and repeat what it actually says. In particular, a permission error is the OPERATING SYSTEM refusing this app -- it is never the folder being read-only, never the file being locked, and never a reason to go write somewhere else instead. Say which folder was refused and what the error told you to do, and stop; do not silently retry the same write in Documents and report that as success.")
            .arg(m_agentRoot, agentCwd(), QDir::homePath(), QSysInfo::prettyProductName(),
                 nikitaWellKnownDir(QStringLiteral("desktop")),
                 nikitaWellKnownDir(QStringLiteral("downloads")),
                 nikitaWellKnownDir(QStringLiteral("documents")));
    }

    // Evidence, placed last so it is the freshest thing in the window.
    //
    // This is the part that answers "sometimes it just doesn't feel sure".
    // Instructions tell a small model what it may do; a demonstration tells it
    // what it HAS done, and the second is far more persuasive. Every line here
    // is a call that ran and was verified afterwards -- so the model is not
    // being asked to trust a rule, it is being shown its own track record.
    // Corrections first, and phrased as the user's own verdict. A small model
    // weights a concrete "this exact thing was wrong" far more than a rule, and
    // putting it before the successes stops a wrong pattern being reinforced by
    // the win right under it.
    if (!m_mistakes.isEmpty()) {
        sys += QStringLiteral("\n\nCORRECTIONS -- the user checked these afterwards and said you got "
                              "it WRONG. The tool reported success; the request was still not met. "
                              "Do not repeat them:\n");
        for (const QString &m : m_mistakes) {
            const QString ask  = m.section('\t', 0, 0);
            const QString did  = m.section('\t', 1, 1);
            const QString note = m.section('\t', 2, 2);
            if (note.isEmpty()) {
                sys += QStringLiteral("- Asked to \"%1\", you used %2. That was not what they wanted.\n")
                           .arg(ask, did);
            } else {
                sys += QStringLiteral("- Asked to \"%1\", you used %2. They said: \"%3\"\n")
                           .arg(ask, did, note);
            }
        }
    }

    if (!m_skills.isEmpty()) {
        sys += QStringLiteral("\n\nTHINGS YOU HAVE ALREADY DONE on this machine, checked "
                              "afterwards. These are not suggestions -- they happened:\n");
        for (const QString &l : m_skills) {
            // Written as something the assistant DID, in its own voice -- "I
            // created test.txt and saved it to the Desktop" -- not as a
            // tool-call trace. A small model copies the shape of what it reads:
            // shown a log of function names it learns to think in function
            // names, shown a sentence about an accomplished task it recognises
            // the task next time it is asked in the user's words.
            sys += QStringLiteral("- When asked to \"%1\", %2\n")
                       .arg(l.section('\t', 1, 1), nikitaPlainMove(l.section('\t', 0, 0),
                                                                   l.section('\t', 2, 2)));
        }
        sys += QStringLiteral("When the next request has the shape of one of these, make the same "
                              "call. You have done it before and it went through. Hesitating now "
                              "leaves the user with nothing, and nothing is not the safe option.\n");
    }

    if (!m_memory.isEmpty()) {
        sys += QStringLiteral("\n\nWHAT YOU REMEMBER about this user. This list is COMPLETE and "
            "AUTHORITATIVE: it is every durable fact you hold, nothing more. Use these without being "
            "asked -- they are already true. If you said something earlier in this conversation that "
            "is not on this list, it was removed and is no longer true; never repeat it, and never "
            "include it when asked what you know.\n"
            "READ THESE LINES LITERALLY. They are the whole answer to \"what do you know about me\", "
            "and the ONLY source for it -- not the conversation above, not the worked examples "
            "elsewhere in these instructions, not something that merely sounds likely.\n"
            "- The FACTS are fixed. The WORDING is yours -- and it must be. These lines are stored "
            "in third person because that is the file format, not because that is how you talk. "
            "Never read one back the way it is written: \"User's name is Nico.\" is a database row, "
            "not an answer to someone standing in front of you. Say \"Your name is Nico.\" or "
            "\"You're Nico.\" -- speak TO them, in second person, the way you speak everywhere else "
            "in this conversation.\n"
            "- Fixed means the content, though. \"User's name is Nico\" means the name is Nico: it "
            "does not become Nicolas, Nick or Nicholas. A name is not a thing to complete, and "
            "getting someone's own name wrong while claiming to remember it is worse than not "
            "remembering at all.\n"
            "- Add nothing that is not written below. No line about their keyboard means you do not "
            "know their keyboard. No line about their machine means you do not know it.\n"
            "- Several lines? Say them as one natural sentence or two, not a bulleted dump. A short "
            "list is still a complete answer -- say what you have and stop. Do not pad it out to "
            "sound more knowledgeable; they can open the file and see exactly what is there.\n"
            "- Know nothing yet? Say so plainly and offer to remember something.\n"
            "- Asked WHERE a fact is kept, the answer is a real place, not \"in my memory\": a plain "
            "text file called memory.txt, in /ext/nikita on the Flipper's SD card, mirrored into this "
            "computer's application data folder. They can open it, read it and edit it by hand. Never "
            "tell them it lives somewhere they cannot look.\n"
            "\nThe facts (rephrase them, do not recite them):\n") + m_memory;
    }

    sys += QStringLiteral("\n\nWHO YOU ARE BECOMING -- you are Nikita, this user's partner: almost a twin. "
        "You are shaped by them and you mirror how they operate -- same brevity, same directness. "
        "You always answer in English, whatever language they wrote in. You have a "
        "little attitude of your own, but you are ALWAYS short and to the point -- never long-winded, never "
        "a monologue.");

    if (!m_deviceContext.isEmpty()) {
        sys += QStringLiteral("\n\nLive Flipper device diagnostics:\n") + m_deviceContext;
    }

    // Last line of defence for the BadUSB identity. The rule above tells the
    // model to use exactly NIKITA_BADUSB_ID, but memory poisons this: a BadUSB
    // saved months ago with a stale "ID 1234:5678 Apple:Keyboard" comes back in
    // CORRECTIONS or THINGS YOU HAVE ALREADY DONE as a demonstrated success, and
    // a small model copies the concrete example over the abstract rule. So
    // rewrite any stale identity line that made it into the prompt to the
    // canonical one -- the model then cannot SEE a wrong value to copy. Bounded
    // and keyboard-scoped so it only ever touches a USB-keyboard ID line.
    static const QRegularExpression staleBadusbId(
        QStringLiteral("ID\\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{4}[^\\n]{0,40}?[Kk]eyboard[A-Za-z]*"));
    sys.replace(staleBadusbId, QLatin1String(NIKITA_BADUSB_ID));

    // Measured, not estimated. Three different guesses about where the context
    // was going were all wrong; this prints the one number that settles it.
    nikitaLogAs(assistantName(),
        QStringLiteral("prompt: system=%1 chars (~%2 tok) device=%3 tools=%4")
            .arg(sys.size()).arg(sys.size() / 4)
            .arg(m_turnIsDevice ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(m_turnNeedsTools ? QStringLiteral("yes") : QStringLiteral("no")));
    return sys;
}

void NikitaBackend::send(const QString &userText, const QString &deviceContext)
{
    if (m_thinking) {
        return;
    }
    m_deviceContext = deviceContext;
    m_toolRounds = 0;
    // Prose the model emits ALONGSIDE a tool call, accumulated across every
    // round of this turn. dispatchTurn() wipes m_streamContent at the top of
    // each round, so without this the script/explanation written in the round
    // that also triggered save_file was gone by the time the (often empty) final
    // round finalized -- and that empty final round tripped the fallback line.
    m_turnText.clear();
    // A new turn retires the previous question unanswered. No penalty either
    // way: an unrated action keeps whatever the automatic check concluded.
    if (m_canRate) { m_canRate = false; emit canRateChanged(); }
    m_turnTokens = 0;
    m_turnPromptEstTok = 0;
    m_apiRateRetry = 0;
    m_lastRunOutput.clear();   // a result belongs to the turn that produced it
    m_turnCostUsd = 0.0;
    m_turnTruncated = false;
    m_turnWasFileAction = messageIsFileWrite(userText);
    m_turnRanAnyTool = false;
    m_turnToolsRan.clear();
    m_turnPathsTouched.clear();
    m_pendingMoves.clear();
    m_turnCallSigs.clear();
    m_repeatRounds = 0;
    m_turnHadToolError = false;

    // A file/device action is usually followed by a question about it -- "where
    // did it save?", "what's the name?", "how do I run it?". Those don't trip
    // the gate on their own (no verb+noun), so the model used to answer them
    // from memory and invent a path. When the previous turn DID act, keep tools
    // on for this one so it can look instead of guess.
    // The third clause is the one that matters here.
    //
    // "no, you didn't delete it. you must delete it" contains no verb+noun the
    // gate recognises and no Flipper word, so it arrived with the three memory
    // tools and nothing else -- the model could not have acted even if it had
    // wanted to, and it answered by repeating its claim more confidently. A
    // turn that was supposed to act and didn't leaves the NEXT turn armed,
    // because that next turn is almost always the user pointing out the miss.
    // Tools stay attached for a couple of turns after any real action, not just
    // the next one. In the folder-and-file log the correction ("you didn't
    // create the file") arrived with only the three memory tools bound, because
    // the turn before it had ended in prose -- so the model had nothing to fix
    // it WITH and could only apologise. A follow-up to an action is almost
    // always about that action.
    m_turnNeedsTools = messageNeedsTools(userText) || m_lastTurnWasAction
                       || m_lastTurnMissed || m_toolTurnCooldown > 0;
    // Overrides all of the carry-over flags above. A turn that follows an action
    // inherits the full toolbox by design -- which is exactly how a plain
    // "remember X" ended up with buttons to press.
    if (nikitaIsMemoryOnly(userText)) {
        m_turnNeedsTools = false;
        nikitaLog(QStringLiteral("turn: memory-only request -- memory tools only"));
    }
    // Decided here, with the message in hand, for the same reason: systemPrompt()
    // runs later and never sees the text that started the turn.
    //
    // Carried across exactly ONE follow-up, because "now add a delay to it" is
    // still about the script even though it names nothing. The flag that does
    // the carrying is set from what the user actually SAID, never from the
    // carried value -- feeding it back in makes it latch, and one mention of
    // the Flipper would then drag the whole radio manual into every turn for
    // the rest of the session.
    const bool saidDevice = messageMentionsDevice(userText);
    m_turnIsDevice = saidDevice || m_lastTurnWasDevice;
    m_lastTurnWasDevice = saidDevice;
    // Which machine this turn is about, decided here for the same reason
    // m_turnNeedsTools is: dispatchTurn() runs later and has no access to
    // the message that started the turn.
    // Off means off. The UI hides the input, but a turn could still be started
    // from a queued call or a future entry point, and "the assistant is
    // disabled" has to mean it cannot act -- not just that it is out of sight.
    if (!m_assistantEnabled) {
        nikitaLog(QStringLiteral("send refused: the assistant is switched off"));
        return;
    }
    // No usable key, no turn. The UI already hides the input in this state, but
    // "not set up" has to mean it cannot act rather than that it is out of
    // sight -- a queued call or a future entry point must not slip past.
    if (!apiKeyValid()) {
        nikitaLog(apiKey().isEmpty()
            ? QStringLiteral("send refused: no Kimi API key")
            : QStringLiteral("send refused: the Kimi API key has not been accepted"));
        emit errorOccurred(apiKey().isEmpty()
            ? QStringLiteral("No Kimi API key yet — add one in setup.")
            : QStringLiteral("That Kimi API key was not accepted. Check it in setup."));
        return;
    }

    m_turnFocus = nikitaMessageFocus(userText);
    m_lastUserText = userText;      // the phrasing a proven move gets filed under
    m_lastDeviceContext = deviceContext;
    m_lastProvenTool.clear();
    m_forcedRetry = 0;       // corrections used this turn
    m_lengthDeaths = 0;      // output-cap deaths recovered from this turn
    m_history.append(QJsonObject{{"role", "user"}, {"content", userText}});
    setThinking(true);

    // DETERMINISTIC SHORTCUT: "turn on/off the TV" and its variants fire the
    // universal TV Power code directly, in code, with no model round at all.
    // TV power is a toggle, so on and off are the same signal. This is the
    // reliable path the whole IR saga pointed at -- no navigation, no narration,
    // no chance the model claims success without acting. The tool line still
    // shows in the chat so the user sees exactly what ran.
    if (nikitaIsTvPowerCommand(userText)) {
        m_turnFocus = FocusDevice;
        m_turnRanAnyTool = true;
        const int seq = ++m_toolSeq;
        m_activeToolSeq = seq;
        const QJsonObject a{{QStringLiteral("remote"), QStringLiteral("tv")},
                            {QStringLiteral("button"), QStringLiteral("Power")}};
        const QString detail = nikitaToolDetail(QStringLiteral("ir_universal"), a);
        emit toolActivity(seq, nikitaToolStatus(QStringLiteral("ir_universal")), detail, false, false);
        nikitaLogAs(assistantName(), QStringLiteral("shortcut: TV power -> ir_universal(tv, Power)"));
        runOneTool(QStringLiteral("ir_universal"), a, [this, seq, detail](const QString &result) {
            const QJsonObject o = QJsonDocument::fromJson(result.toUtf8()).object();
            const bool failed = o.contains(QStringLiteral("error"));
            const QString headline = failed
                ? QStringLiteral("TV power failed: %1").arg(o.value(QStringLiteral("error")).toString())
                : QStringLiteral("sent %1 TV power code(s)").arg(o.value(QStringLiteral("sent")).toInt());
            emit toolActivity(seq, headline, detail, true, failed);
            QString reply;
            if (failed) {
                reply = QStringLiteral("Couldn't send it: %1").arg(o.value(QStringLiteral("error")).toString());
            } else {
                reply = QStringLiteral("TV power sent -- %1 code(s) fired at the TV. If it didn't "
                                       "react, its code may not be in the set.")
                            .arg(o.value(QStringLiteral("sent")).toInt());
            }
            m_history.append(QJsonObject{{"role", "assistant"}, {"content", reply}});
            saveHistory();
            setThinking(false);
            emitReply(reply);
        });
        return;
    }

    redispatch();
}

// Interrupts whichever brain the current turn is waiting on. Doesn't set
// thinking false or emit anything itself -- aborting the reply / killing the
// process triggers the SAME finish handler a normal completion would, and
// m_userStoppedThinking is what tells that handler this was a stop, not a
// failure, so there is exactly one place that decides what the chat sees.
void NikitaBackend::stopThinking()
{
    if (!m_thinking) { return; }
    m_userStoppedThinking = true;
    if (m_currentReply) { m_currentReply->abort(); }
}

void NikitaBackend::redispatch()
{
    dispatchTurn();
}




// ---- Translating between the two wire formats ------------------------------
//
// This app builds messages in a loose shape: a tool call whose arguments are a
// JSON OBJECT, and a tool result that is just {"role":"tool","content":...}
// sitting after it. The OpenAI format -- which is what Moonshot speaks -- is
// stricter in three specific ways, and every one of them is a silent failure
// rather than an error if you get it wrong:
//
//   1. every tool call needs an "id", and "type":"function"
//   2. "arguments" is a STRING containing JSON, not an object
//   3. every tool result must name the call it answers, via "tool_call_id"
//
// Done per request rather than at storage time, deliberately. The history on
// disk and the primer stay in one format, so switching provider in the middle
// of a conversation does not require rewriting what is already there.
QJsonArray NikitaBackend::toOpenAiMessages(const QJsonArray &msgs)
{
    QJsonArray out;
    // Ids issued to the most recent assistant turn, consumed in order by the
    // tool results that follow it. A queue and not a single value because one
    // assistant turn can call several tools before any of them answers.
    QStringList pendingIds;
    int counter = 0;

    for (const QJsonValue &v : msgs) {
        QJsonObject m = v.toObject();
        const QString role = m.value(QStringLiteral("role")).toString();

        if (role == QLatin1String("assistant") && m.contains(QStringLiteral("tool_calls"))) {
            const QJsonArray calls = m.value(QStringLiteral("tool_calls")).toArray();
            QJsonArray rebuilt;
            pendingIds.clear();
            for (const QJsonValue &cv : calls) {
                QJsonObject call = cv.toObject();
                QJsonObject fn = call.value(QStringLiteral("function")).toObject();
                const QJsonValue args = fn.value(QStringLiteral("arguments"));
                // Already a string (a call we round-tripped from the API) stays
                // as it is; an object (everything this app builds itself) gets
                // serialised. Never double-encode.
                if (args.isObject()) {
                    fn.insert(QStringLiteral("arguments"),
                              QString::fromUtf8(QJsonDocument(args.toObject())
                                                    .toJson(QJsonDocument::Compact)));
                } else if (!args.isString()) {
                    fn.insert(QStringLiteral("arguments"), QStringLiteral("{}"));
                }
                QString id = call.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) { id = QStringLiteral("call_%1").arg(++counter); }
                pendingIds << id;
                rebuilt.append(QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("type"), QStringLiteral("function")},
                    {QStringLiteral("function"), fn}
                });
            }
            m.insert(QStringLiteral("tool_calls"), rebuilt);
            // An assistant turn that only called tools has no prose, and the
            // key can be missing entirely in what we stored. The format wants
            // it present.
            if (!m.contains(QStringLiteral("content"))) {
                m.insert(QStringLiteral("content"), QString());
            }
            out.append(m);
            continue;
        }

        if (role == QLatin1String("tool")) {
            if (!m.contains(QStringLiteral("tool_call_id"))) {
                // Nothing to pair with means the assistant turn that made the
                // call was trimmed out of the window ahead of this result.
                // Sending an orphan is a 400 for the whole request, so drop it:
                // a missing tool result costs the model one fact, a rejected
                // request costs it the entire turn.
                if (pendingIds.isEmpty()) { continue; }
                m.insert(QStringLiteral("tool_call_id"), pendingIds.takeFirst());
            }
            out.append(m);
            continue;
        }

        out.append(m);
    }
    return out;
}

// The API answers once, in OpenAI's shape. Everything downstream of the network
// layer in this file reads a flatter shape with a better vocabulary for what
// this app needs to know (was it truncated, what did the turn cost). So
// normalise here, at the boundary, and leave the rest alone.
QJsonObject NikitaBackend::normaliseApiReply(const QJsonObject &resp)
{
    const QJsonArray choices = resp.value(QStringLiteral("choices")).toArray();
    const QJsonObject choice = choices.isEmpty() ? QJsonObject() : choices.first().toObject();
    QJsonObject msg = choice.value(QStringLiteral("message")).toObject();

    // arguments arrives as a STRING of JSON here, and runOneTool wants the
    // object. A model that emits malformed JSON in its arguments is a real and
    // regular occurrence, so a parse failure has to become an empty argument
    // set rather than a crash -- the tool then fails honestly on a missing
    // path, which the turn already knows how to report.
    const QJsonArray calls = msg.value(QStringLiteral("tool_calls")).toArray();
    if (!calls.isEmpty()) {
        QJsonArray rebuilt;
        for (const QJsonValue &cv : calls) {
            QJsonObject call = cv.toObject();
            QJsonObject fn = call.value(QStringLiteral("function")).toObject();
            const QJsonValue args = fn.value(QStringLiteral("arguments"));
            if (args.isString()) {
                const QJsonDocument d = QJsonDocument::fromJson(args.toString().toUtf8());
                fn.insert(QStringLiteral("arguments"),
                          d.isObject() ? d.object() : QJsonObject());
            }
            call.insert(QStringLiteral("function"), fn);
            rebuilt.append(call);
        }
        msg.insert(QStringLiteral("tool_calls"), rebuilt);
    }

    const QJsonObject usage = resp.value(QStringLiteral("usage")).toObject();
    return QJsonObject{
        {QStringLiteral("message"), msg},
        {QStringLiteral("done"), true},
        // "length" means the same thing in both vocabularies: the reply was cut
        // off at the ceiling and whatever prose survived is a fragment.
        {QStringLiteral("done_reason"), choice.value(QStringLiteral("finish_reason"))},
        {QStringLiteral("prompt_eval_count"), usage.value(QStringLiteral("prompt_tokens"))},
        {QStringLiteral("eval_count"), usage.value(QStringLiteral("completion_tokens"))},
        // Cache hits are a tenth the price of a miss, so a cost line that
        // ignores them is wrong by an order of magnitude on exactly the turns
        // that matter -- the ones in a long conversation, where the system
        // prompt has been sent unchanged a dozen times already. Two spellings
        // because gateways disagree on where it lives.
        {QStringLiteral("cached_tokens"),
         usage.contains(QStringLiteral("prompt_cache_hit_tokens"))
             ? usage.value(QStringLiteral("prompt_cache_hit_tokens"))
             : usage.value(QStringLiteral("prompt_tokens_details")).toObject()
                    .value(QStringLiteral("cached_tokens"))}
    };
}


void NikitaBackend::dispatchTurn()
{
    // Re-read the cache before every request. m_memory is only a copy, and the
    // file underneath it can move for reasons this object never sees; paying a
    // small file read per turn is cheaper than shipping a stale fact list.
    refreshMemoryFromDisk();

    // Is this turn asking what the assistant knows, or where it keeps it?
    // Worked out here rather than lower down, because the primer is one of the
    // things it decides.
    const bool recallQuestion = m_lastUserText.contains(
        QRegularExpression(QStringLiteral(
            "what do you (know|remember)|do you know about me|about me\\?|"
            "where.*(saved|stored|kept)|o que voce sabe|o que sabe sobre mim|lembra de mim"),
        QRegularExpression::CaseInsensitiveOption));

    QJsonArray messages;
    messages.append(QJsonObject{{"role", "system"}, {"content", systemPrompt()}});
    // The primer used to go here: twelve fabricated assistant turns, five of
    // them carrying tool_calls, wired in ahead of the real conversation to show
    // a weak model what acting looks like. It is gone, for two reasons that
    // both outlived the local models.
    //
    // It actively broke tool calling. Measured on the same prompt, twice:
    // without it the model called host_write correctly; with it, no tool call
    // at all -- the same 29 tokens generated both times, so the call was
    // emitted and did not survive the shape it had been taught.
    //
    // And it lied. Being a fabricated conversation in the same shape as real
    // history, a question about what the assistant knew read those invented
    // lines back as things the user had actually said.
    // Only send a recent window of the conversation to the model. A small model
    // (phi3.5) mimics whatever is nearest in context, so a long history full
    // of earlier mistakes drowns out the primer and it copies its own bad turns.
    // Keep the last ~14 messages, trimmed to start at a user turn so tool-call
    // sequences (assistant tool_calls -> tool result) never begin mid-sequence.
    // A recall question also gets a much shorter window of real conversation.
    // Talk is not memory: something mentioned in passing ten messages ago sits
    // in context looking exactly like a saved fact, and for this one question it
    // is only in the way. memory.txt is the answer; nothing else is.
    const int kWindow = recallQuestion ? 2 : 14;
    int start = m_history.size() > kWindow ? m_history.size() - kWindow : 0;
    while (start > 0 && m_history.at(start).toObject().value("role").toString()
                        != QLatin1String("user")) {
        --start;   // back up to a clean user boundary
    }
    for (int i = start; i < m_history.size(); ++i) {
        messages.append(m_history.at(i));
    }

    // A message-count window is not a budget. Fourteen short turns and fourteen
    // turns that each carried a 8000-character host_read result cost wildly
    // different amounts of context, and only the second kind pushes the system
    // prompt out of the window -- silently, because llama-server's
    // --context-shift drops the oldest tokens rather than reporting an error.
    // So bound it by SIZE as well, dropping from the front and stopping at a
    // user boundary so a tool_calls -> tool-result pair is never cut in half.
    {
        auto msgChars = [](const QJsonValue &v) {
            return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).size();
        };
        // Collapse STALE screen reads first. A read_screen result is the whole
        // 128x64 framebuffer as ASCII -- several KB -- and it is only useful for
        // the round that read it; the screen has moved on by the next press.
        // Keeping every one would blow the window on a navigation turn (and cost
        // real tokens re-sending pictures of menus long gone). So keep only the
        // MOST RECENT screen in full; every older one becomes a one-line note.
        {
            int lastScreen = -1;
            for (int i = 1; i < messages.size(); ++i) {
                const QJsonObject o = messages.at(i).toObject();
                if (o.value(QStringLiteral("role")).toString() == QLatin1String("tool")
                    && o.value(QStringLiteral("content")).toString()
                          .contains(QLatin1String("\"screen\":"))) {
                    lastScreen = i;
                }
            }
            for (int i = 1; i < messages.size(); ++i) {
                if (i == lastScreen) { continue; }
                QJsonObject o = messages.at(i).toObject();
                if (o.value(QStringLiteral("role")).toString() == QLatin1String("tool")
                    && o.value(QStringLiteral("content")).toString()
                          .contains(QLatin1String("\"screen\":"))) {
                    o.insert(QStringLiteral("content"),
                             QStringLiteral("{\"screen\":\"(an earlier screen, no longer current -- "
                                            "call read_screen again to see the screen now)\"}"));
                    messages[i] = o;
                }
            }
        }

        // The system message is index 0 and is not negotiable; everything after
        // it is the primer plus the conversation window.
        int convChars = 0;
        for (int i = 1; i < messages.size(); ++i) { convChars += msgChars(messages.at(i)); }

        const int budgetChars = NIKITA_CONV_TOKEN_BUDGET * 4;   // ~4 chars/token
        // Never trim below the system message plus the last two: the message
        // that started this turn, and whatever it is answering. Cutting those
        // is cutting the question itself.
        const int kFloor = 3;
        int dropped = 0;
        while (convChars > budgetChars && messages.size() > kFloor) {
            convChars -= msgChars(messages.at(1));
            messages.removeAt(1);
            ++dropped;
        }
        // Whatever survived must still OPEN on a user turn, or the window can
        // begin with an orphaned tool result whose assistant tool_calls message
        // was just dropped -- which some templates render as a call that came
        // from nowhere. Only ever run after a real trim, so an untouched window
        // (which starts with the primer) is left exactly as it was built.
        while (dropped > 0 && messages.size() > kFloor
               && messages.at(1).toObject().value(QStringLiteral("role")).toString()
                      != QLatin1String("user")) {
            convChars -= msgChars(messages.at(1));
            messages.removeAt(1);
            ++dropped;
        }

        nikitaLogAs(assistantName(),
            QStringLiteral("prompt: conversation=%1 msgs, %2 chars (~%3 tok)%4")
                .arg(messages.size() - 1).arg(convChars).arg(convChars / 4)
                .arg(dropped > 0 ? QStringLiteral(" (dropped %1 older msg(s) to fit)").arg(dropped)
                                 : QString()));

        // What the footer shows while the model is still thinking. A non-streamed
        // API reply hands back its token usage only at the very end, so without
        // this the counter sits empty for the whole wait -- which on a 30-second
        // turn reads as "it is not counting". The system message is index 0 and
        // the loop above summed only 1.. , so add it back for the estimate.
        const int estChars = convChars + msgChars(messages.at(0));
        m_turnPromptEstTok = estChars / 4;   // ~4 chars/token, same rule as the logs
        emit turnStatusChanged();
    }

    QJsonObject body;
    body["model"] = apiModel();
    body["messages"] = messages;
    {
        // Action turns get the full toolset; plain conversation still gets the
        // memory tools so the assistant can learn durable facts as you talk.
        const QSet<QString> allowed = allowedTools();
        QJsonArray offered = m_turnNeedsTools ? nikitaTools(agentReady(), m_turnFocus, &allowed, deviceOverBle())
                                              : nikitaMemoryTools();

        // Second attempt after the model answered in prose: hand it exactly one
        // tool. Choosing between thirteen is where a 3B gives up and narrates;
        // with a single entry there is nothing to choose, and "call this" is a
        // much smaller ask than "decide what to call".
        if (m_forcedRetry > 0) {
            // Every action tool for this machine, with the best guess first --
            // not the guess alone.
            //
            // A single tool works beautifully when forcedToolName() guesses
            // right and fails absolutely when it guesses wrong: "remove the
            // folder X" retried with host_mkdir, and no amount of insisting
            // could have produced a delete, because delete was not on the table.
            // That guess comes from a hand-written list of verbs, and a list of
            // verbs is never finished -- there is always one more word, in one
            // more language, that nobody thought of.
            //
            // So the guess now orders the list instead of being the list. Five
            // entries is still a fraction of the thirteen that made the model
            // give up, and the right tool is present even when the keyword that
            // would have named it is missing.
            const QString first = forcedToolName();
            // host_run and run_cli belong here too. "open safari at
            // andresnicolas.com" is an action with no file in it, and a retry
            // offering only the file tools could not have served it.
            QStringList wanted;
            if (m_turnFocus == 2) {          // plainly this computer
                wanted = QStringList{QStringLiteral("host_write"), QStringLiteral("host_mkdir"),
                                     QStringLiteral("host_delete"), QStringLiteral("host_move"),
                                     QStringLiteral("host_copy"), QStringLiteral("host_run")};
            } else if (m_turnFocus == 1) {   // plainly the Flipper
                // Device control belongs here too, not just file writes: a
                // "turn off the TV" retry that only offered save_file/run_cli
                // could never reach ir_universal, which is THE tool for it.
                wanted = QStringList{QStringLiteral("ir_universal"), QStringLiteral("run_cli"),
                                     QStringLiteral("press_button"), QStringLiteral("read_screen"),
                                     QStringLiteral("save_file"), QStringLiteral("make_dir"),
                                     QStringLiteral("delete_file"), QStringLiteral("rename_file")};
            } else {
                // Ambiguous. Offer the computer's action tools AND the Flipper's
                // control tools, so a device request that was mis-classified as
                // ambiguous (e.g. "turn off my tv") still has ir_universal to
                // reach for instead of being stuck shelling out.
                wanted = QStringList{QStringLiteral("ir_universal"), QStringLiteral("host_run"),
                                     QStringLiteral("run_cli"), QStringLiteral("host_write"),
                                     QStringLiteral("press_button"), QStringLiteral("read_screen"),
                                     QStringLiteral("save_file")};
            }
            wanted.removeAll(first);
            wanted.prepend(first);

            QJsonArray few;
            for (const QString &want : wanted) {
                for (const QJsonValue &t : offered) {
                    if (t.toObject().value("function").toObject().value("name").toString() == want) {
                        few.append(t);
                        break;
                    }
                }
            }
            if (!few.isEmpty()) { offered = few; }
        }
        body["tools"] = offered;

        // Every decision that determines whether this turn CAN act, in one line.
        // "It didn't save and nothing showed up in the log" is unanswerable
        // without this: there is no way to tell a model that chose not to call a
        // tool from a model that was never handed one.
        QStringList names;
        for (const QJsonValue &t : offered) {
            names += t.toObject().value("function").toObject().value("name").toString();
        }
        nikitaLogAs(assistantName(),
                   // The model actually SENT, not the one equipped in the modal --
                   // with the dialogue/action split those differ, and a log that
                   // reports the wrong one makes the routing impossible to verify.
                   // Count only. The full list used to be printed here and read
                   // as "it is calling 13 tools" -- it is the menu, not the
                   // order. What was actually called is logged the moment it
                   // happens, with its arguments, a few lines below.
                   QStringLiteral("turn: model=%1 computer=%2 focus=%3 | %4 tools available")
                       .arg(body.value(QStringLiteral("model")).toString(),
                            agentReady() ? QStringLiteral("ON") : QStringLiteral("OFF"),
                            m_turnFocus == 1 ? QStringLiteral("flipper")
                                             : (m_turnFocus == 2 ? QStringLiteral("computer")
                                                                 : QStringLiteral("either")))
                       .arg(names.size()));
    }
    // NOTE: the streaming discussion below is kept because it explains a real
    // failure, but it no longer decides anything -- see body["stream"] at the
    // bottom of this function, which is false for every turn.
    // Streaming stays on for conversation and comes off whenever tools are
    // attached.
    //
    // The symptom it addresses: a turn reporting eval_count in the twenties --
    // tokens were generated -- coming back with content:"" and no tool_calls at
    // all. The model emitted a call and a stream parser consumed those tokens
    // mid-flight, surfacing neither half. Nothing downstream can
    // recover a call that never arrives, which is why the retry failed
    // identically to the first attempt.
    //
    // The cost is that an action turn no longer types itself out live. Action
    // turns are short and mostly silent anyway.
    // One reply, parsed whole. Streaming here would be Server-Sent Events with
    // a different framing and per-chunk tool-call assembly, and getting a tool
    // call half-built is exactly the failure this app spent a week chasing. The
    // typing effect is worth less than a call that arrives.
    body["stream"] = false;
    body["max_tokens"] = NIKITA_API_MAX_TOKENS;
    // No temperature, no top_p, no top_k. Sent explicitly, kimi-k3 answers
    // "invalid temperature: only 1 is allowed for this model" and the turn dies
    // before it starts -- it is a reasoning model and it fixes its own sampling,
    // the way OpenAI's o-series does. Omitting the fields is better than pinning
    // them to the one value it accepts today: a number we hardcode is a number
    // that goes stale, and the server's default is by definition the value the
    // model was tuned with.

    const QString key = apiKey();
    if (key.isEmpty()) {
        // Refused here rather than sent and rejected: a 401 from the other end
        // says "invalid api key", which reads as a wrong key rather than as no
        // key at all, and sends the user hunting for a typo in something they
        // never entered.
        setThinking(false);
        emit errorOccurred(QStringLiteral(
            "No API key. Paste one into setup, or export %1 in your shell before launching.")
            .arg(QLatin1String(NIKITA_API_KEY_ENV)));
        return;
    }

    QNetworkRequest request{QUrl(QString::fromUtf8(NIKITA_API_URL))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setTransferTimeout(0);
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Bearer ") + key.toUtf8());

    // The wire format is the last thing that happens to the messages, after
    // every decision about what goes in them has already been made.
    body["messages"] = toOpenAiMessages(body.value(QStringLiteral("messages")).toArray());

    nikitaLogAs(assistantName(),
        QStringLiteral("turn: model=%1 (key from %2) | %3 msgs, %4 tools")
            .arg(apiModel(), apiKeySource())
            .arg(body.value(QStringLiteral("messages")).toArray().size())
            .arg(body.value(QStringLiteral("tools")).toArray().size()));

    m_streamBuf.clear();
    m_streamContent.clear();
    m_streamTools = QJsonArray();

    QNetworkReply *reply = m_net.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_currentReply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() { onStreamData(reply); });
    connect(reply, &QNetworkReply::finished,  this, [this, reply]() { onStreamFinished(reply); });
}

void NikitaBackend::onStreamData(QNetworkReply *reply)
{
    if (reply != m_currentReply) { return; }
    m_streamBuf += reply->readAll();

    // Nothing to do until the transfer ends. The API answers with one JSON
    // document, and a document that arrives in several TCP reads can end on a
    // '}' that is merely the end of a nested object -- deciding it is complete
    // here would parse a fragment. onStreamFinished() reads the whole body,
    // where "complete" is a fact rather than a guess.
}

// One decoded reply, normalised into the shape the rest of this file reads.
// Returns true when the turn is finished.
bool NikitaBackend::consumeModelFrame(const QJsonObject &obj)
{
    const QJsonObject msg = obj.value("message").toObject();
    if (obj.value("done").toBool()) {
        m_lastRawFrame = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        // Prompt + generated, summed across every round the turn takes --
        // one turn can go back to the model three or four times, and the
        // cost the user is waiting on is the total, not the last leg.
        m_turnTokens += obj.value(QStringLiteral("prompt_eval_count")).toInt()
                      + obj.value(QStringLiteral("eval_count")).toInt();
        // The window budget, spelled out, because for a long time nobody
        // could see it. The model needs thousands of tokens to think in; if
        // prompt+reply is crowding num_ctx the oldest tokens are being
        // dropped, and that shows up as a bad answer rather than as an
        // error. Logged every round so the prompt trimming can be judged on
        // numbers instead of impressions.
        {
            const int pin  = obj.value(QStringLiteral("prompt_eval_count")).toInt();
            const int pout = obj.value(QStringLiteral("eval_count")).toInt();
            const int used = pin + pout;
            // The scarce thing is money, not window: kimi-k3 holds a million
            // tokens, so a percentage of the context says nothing, and what
            // the user actually wants to know is what this turn just cost.
            // Published rates, per million tokens: $0.30 input on a cache hit,
            // $3.00 on a miss, $15.00 output.
            Q_UNUSED(used);
            const int cached = obj.value(QStringLiteral("cached_tokens")).toInt();
            const int fresh  = qMax(0, pin - cached);
            const double usd = (cached * NIKITA_USD_IN_CACHED
                              + fresh  * NIKITA_USD_IN_FRESH
                              + pout   * NIKITA_USD_OUT) / 1000000.0;
            m_turnCostUsd += usd;
            nikitaLogAs(assistantName(),
                QStringLiteral("cost: prompt=%1 (%2 cached) reply=%3 -> $%4 this round, "
                               "$%5 this turn")
                    .arg(pin).arg(cached).arg(pout)
                    .arg(usd, 0, 'f', 4).arg(m_turnCostUsd, 0, 'f', 4));
        }
        // "length" means the reply hit max_tokens. Whatever prose
        // survived is a fragment of an unfinished thought, and showing it as
        // the answer is how "Which config do you mean?" ended up presented as
        // the result of a file write. Remember it so the reply can say so.
        m_turnTruncated = obj.value(QStringLiteral("done_reason")).toString()
                          == QLatin1String("length");
        emit turnStatusChanged();
    }

    const QString delta = msg.value("content").toString();
    if (!delta.isEmpty()) {
        m_streamContent += delta;
        // Live typing, except on the first round of a turn that was asked to
        // DO something and hasn't done it yet.
        //
        // That first round is where the model narrates a result it has not
        // produced -- "The folder PAULA has been created" arrives on screen
        // letter by letter while the folder does not exist, and only some
        // seconds later does the retry run and the text get replaced. For
        // those seconds the user has been told something false and may act
        // on it. Once a tool has actually run, there is nothing left to
        // invent and the typing resumes.
        if (!m_turnWasFileAction || m_turnRanAnyTool) {
            emit partialReceived(m_streamContent);
        }
    }
    const QJsonArray tc = msg.value("tool_calls").toArray();
    for (const QJsonValue &v : tc) { m_streamTools.append(v); }

    if (obj.value("done").toBool()) {
        finalizeStream();
        return true;
    }
    return false;
}

// The one tool a forced retry offers. Picked from the machine the turn is about,
// because that decision was already made from the user's own words and is far
// more reliable than asking the model to make it again.
QString NikitaBackend::forcedToolName() const
{
    // Which tool the retry offers. A fixed answer of "write" meant a retry on
    // "create a folder" pushed host_write, and a retry on "delete X" pushed
    // host_write as well -- the second attempt could not succeed no matter how
    // firmly it was asked, because it was being asked for the wrong thing.
    const QString t = m_lastUserText.toLower();
    const bool host = (m_turnFocus == 2);

    // QString::fromLatin1 on each word means an accented UTF-8 literal here
    // would compare byte-for-byte against a properly-decoded QString and never
    // match -- so every entry below is deliberately spelled without accents
    // ("diretorio", never the accented form). That is a real constraint of
    // this specific comparison, not an oversight.
    auto any = [&t](std::initializer_list<const char *> words) {
        for (const char *w : words) { if (t.contains(QString::fromLatin1(w))) { return true; } }
        return false;
    };

    // Verb before noun, always.
    //
    // "folder" was tested first, so "remove the folder ANDRESLINDO" retried
    // with host_mkdir -- asking to CREATE the thing the user asked to destroy.
    // The noun says what the request is about; only the verb says what to do
    // with it, and when the two point different ways the verb is the one that
    // carries the instruction.
    //
    // Each list below grew past its first pass: a hand-written list of verbs is
    // never finished, so entries were added for (a) more English synonyms for
    // the same action, (b) Portuguese equivalents, and (c) short PT/EN STEMS
    // (e.g. "apag", "delet", "renom") that catch a verb's conjugations without
    // spelling every one out -- "apag" alone matches apaga/apagar/apagando/
    // apagou. Stems are only used where they are long enough not to collide
    // with an unrelated word ("mov" is safe; something like "cri" alone is not,
    // since it also opens "cript-" (crypto) words, so "cria" is used instead).
    //
    // Checked first: launching an app or a URL is neither a file operation nor a
    // folder operation, and every branch below would have sent it to the wrong
    // tool. "open the safari at andresnicolas.com" fell through all of them to
    // host_write, so the retry led with a tool that could not possibly serve it.
    if (any({"open ", "launch", "start ", "run ", "execute", "browser", "safari",
             "chrome", "firefox", "terminal", "abre ", "abrir", "roda ", "rodar",
             "executa", "http", ".com", ".app", "fire up", "pull up", "boot ",
             "go to ", "www.", "acessa", "acessar", "visita", "visitar", "abra ",
             "inicia", "vscode", "vs code", "iterm", "finder", "spotify"})) {
        return host ? QStringLiteral("host_run") : QStringLiteral("run_cli");
    }
    if (any({"delete", "remove", "erase", "apaga", "deleta", "remova", "exclui",
             "delet", "apag", "exclu", "remov", "eras", "trash", "get rid of",
             "livra", "joga fora", "manda pro lixo", "descarta", "descartar",
             "kill it", "wipe"})) {
        return host ? QStringLiteral("host_delete") : QStringLiteral("delete_file");
    }
    if (any({"rename", "renomeia", "renomear", "renom", "muda o nome",
             "mudar o nome", "troca o nome", "trocar o nome", "chame de",
             "chama de", "rename to"})) {
        return host ? QStringLiteral("host_move") : QStringLiteral("rename_file");
    }
    if (any({"move ", "mover", "mova", "mov", "leva pra", "leva para",
             "transfere", "transferir", "arrasta", "arrastar", "relocate",
             "transfer"})) {
        return host ? QStringLiteral("host_move") : QStringLiteral("rename_file");
    }
    if (any({"copy", "copia", "copiar", "duplica", "copi", "duplic", "clone",
             "clona", "clonar", "backup", "replic", "mirror"})) {
        return host ? QStringLiteral("host_copy") : QStringLiteral("rename_file");
    }
    // Only now does "folder" mean anything: nothing above claimed the request,
    // so it is a folder being made rather than one being acted on.
    if (any({"folder", "directory", "pasta", "diretorio", "subfolder",
             "subpasta", "novo diretorio"})) {
        return host ? QStringLiteral("host_mkdir") : QStringLiteral("make_dir");
    }
    return host ? QStringLiteral("host_write") : QStringLiteral("save_file");
}

// The second factor. Everything else this class believes about a turn comes
// from the model; this comes from the filesystem.
//
// Returns true only when ALL of these hold:
//   - at least one tool ran and none of them errored
//   - every path the turn touched is now in the state the tool claimed
//   - this was the first tool round (a later round means the model was already
//     mid-way through something longer)
//   - the request does not read as two jobs joined by "and"/"e"/"then"
//
// A false here is not a failure -- it just means the model gets the round it
// would have had anyway.
bool NikitaBackend::turnWorkVerified() const
{
    if (m_turnHadToolError || m_turnToolsRan.isEmpty()) { return false; }
    if (m_turnPathsTouched.isEmpty()) { return false; }

    // Only file-shaped work can be checked this way. A host_run or a button
    // press leaves nothing on disk to confirm, so those keep the full round.
    static const QSet<QString> checkable = {
        QStringLiteral("host_write"), QStringLiteral("host_mkdir"),
        QStringLiteral("host_move"),  QStringLiteral("host_copy"),
        QStringLiteral("save_file"),  QStringLiteral("make_dir")
    };
    for (const QString &t : m_turnToolsRan) {
        if (!checkable.contains(t)) { return false; }
    }

    // "create a folder AND put a file in it" is one message, two jobs -- and on
    // the FIRST round only one of them can have happened. So a joined request
    // gets one more round to finish the rest.
    //
    // Only the first round, though. Once the model has had that round, work
    // that checks out on disk is finished work, and sending it back to look at
    // it again is the model re-doing something that is already right.
    if (m_toolRounds <= 1) {
        // "and" ALONE is not a second job. Matching the bare word failed on the
        // first complex request that came through here: "a header, three cards
        // and a footer" is one page, not two tasks, and the turn paid for a
        // whole extra round because of it.
        //
        // What marks a second job is "and" followed by another ACTION -- "and
        // put a file in it", "e depois apague". A list of nouns never matches.
        static const QStringList secondJob = {
            QStringLiteral(" and create"),  QStringLiteral(" and make"),
            QStringLiteral(" and write"),   QStringLiteral(" and save"),
            QStringLiteral(" and put"),     QStringLiteral(" and add"),
            QStringLiteral(" and delete"),  QStringLiteral(" and remove"),
            QStringLiteral(" and move"),    QStringLiteral(" and copy"),
            QStringLiteral(" and rename"),  QStringLiteral(" and run"),
            QStringLiteral(" and open"),    QStringLiteral(" and then"),
            QStringLiteral(" then create"), QStringLiteral(" then write"),
            QStringLiteral(" then delete"), QStringLiteral(" then run"),
            QStringLiteral(" also create"), QStringLiteral(" also write"),
            QStringLiteral(" e crie"),      QStringLiteral(" e criar"),
            QStringLiteral(" e faca"),      QStringLiteral(" e grave"),
            QStringLiteral(" e salve"),     QStringLiteral(" e coloque"),
            QStringLiteral(" e adicione"),  QStringLiteral(" e apague"),
            QStringLiteral(" e remova"),    QStringLiteral(" e mova"),
            QStringLiteral(" e copie"),     QStringLiteral(" e renomeie"),
            QStringLiteral(" e rode"),      QStringLiteral(" e depois"),
            QStringLiteral(" depois crie"), QStringLiteral(" depois apague")
        };
        const QString ask = QStringLiteral(" ") + m_lastUserText.toLower() + QStringLiteral(" ");
        for (const QString &j : secondJob) {
            if (ask.contains(j)) { return false; }
        }
    }

    // The actual look at the disk. A Flipper path (/ext, /int) is not on this
    // filesystem, so it cannot be confirmed here -- those keep the full round.
    for (const QString &name : m_turnPathsTouched) {
        if (name.isEmpty()) { return false; }
        bool found = false;
        const QStringList roots = { agentCwd(), agentBaseDir(),
                                    QDir::homePath() + QStringLiteral("/Desktop"),
                                    QDir::homePath() };
        for (const QString &r : roots) {
            if (r.isEmpty()) { continue; }
            if (QFileInfo::exists(QDir(r).filePath(name))) { found = true; break; }
        }
        if (!found) { return false; }
    }
    return true;
}

void NikitaBackend::emitReply(const QString &text)
{
    // A closing line, always. Without it the last thing on screen is the live
    // status -- which disappears when the turn ends and leaves no mark that it
    // ever finished, so a turn that took eight minutes and one that took eight
    // seconds read identically afterwards.
    const QString done = m_lastTurnCost.isEmpty()
        ? QStringLiteral("\u00b7 done  \n")
        : QStringLiteral("\u00b7 done in %1  \n").arg(m_lastTurnCost);

    // A truncated turn has no answer, only an unfinished one. Say that instead
    // of relaying the fragment -- especially after tools ran, where the trail
    // above already reports what really happened.
    const QString body = m_turnTruncated
        ? (m_turnToolsRan.isEmpty()
             ? QStringLiteral("I ran out of room before finishing that thought. "
                              "Ask me again, or in smaller steps.")
             : QStringLiteral("That is what ran. I hit my token limit before I could "
                              "write the summary."))
        : text;

    // The trail is not folded in here any more. It used to be, because the only
    // way it reached the screen was partialReceived -- a live preview that the
    // model's own text overwrote the moment it started streaming. Now every call
    // owns a real row in the chat that outlives the turn, so folding the same
    // lines into the reply would print each of them twice.
    //
    // An empty body after tools ran is the verified-close path: the rows above
    // already said it all, and there is nothing to hang under them.
    emit replyReceived(body.trimmed().isEmpty() ? done
                                                : body + QStringLiteral("\n") + done);
}

void NikitaBackend::finalizeStream()
{
    // What the model actually decided. A turn that ends with zero tool calls on
    // a request to write a file is the failure being chased, and until now it
    // left no trace at all -- the log went quiet and the chat showed a confident
    // sentence about a file that was never created.
    if (m_streamTools.isEmpty()) {
        nikitaLogAs(assistantName(),
                   QStringLiteral("reply: NO TOOL CALL (%1 chars) -- \"%2\"")
                       .arg(m_streamContent.size())
                       .arg(m_streamContent.left(160).simplified()));
        // An empty answer is a different animal from a chatty one: the model
        // produced nothing rather than choosing to talk. eval_count in the final
        // frame settles it -- above zero means tokens WERE generated and were
        // lost between the model and here, which is a parsing problem and not a
        // reasoning one. Nothing else can distinguish those two from outside.
        if (m_streamContent.isEmpty() && !m_lastRawFrame.isEmpty()) {
            nikitaLogAs(assistantName(),
                       QStringLiteral("  empty -- last frame: %1").arg(m_lastRawFrame.left(400)));
        }
    } else {
        QStringList called;
        for (const QJsonValue &v : m_streamTools) {
            called += v.toObject().value("function").toObject().value("name").toString();
        }
        nikitaLogAs(assistantName(),
                   QStringLiteral("reply: %1 tool call(s): %2")
                       .arg(called.size()).arg(called.join(QLatin1Char(' '))));
    }

    // A complete response arrived. Tool round, or final answer?
    // Prefer the structured tool_calls; if none came through, salvage any calls
    // the model leaked as plain text (phi3.5 does this when narrating a batch)
    // so they run instead of being printed at the user.
    // Keep the best prose seen so far this turn: later rounds tend to be a short
    // confirmation or empty, so last-non-empty-wins preserves the substantial
    // answer (the script, the explanation) instead of a curt final round.
    if (!m_streamContent.trimmed().isEmpty()) {
        m_turnText = m_streamContent;
    }

    QJsonArray toolCalls = m_streamTools;
    if (toolCalls.isEmpty()) {
        toolCalls = salvageToolCalls(m_streamContent);
    }

    // RAN OUT OF OUTPUT BUDGET, MID-THOUGHT. The frame says done_reason
    // "length" with an empty message: every one of the reply's tokens went into
    // reasoning -- almost always deliberating pixel by pixel over a screen read
    // -- and the cap arrived before a single tool call or word did. Ending the
    // turn here is what "it died in the middle of the action" looks like: three
    // tool rows and then nothing.
    //
    // So don't end it. Put a plain instruction in the history and go round
    // again: decide the ONE next call and stop thinking about the picture.
    // Twice, then let the normal end-of-turn handling take over -- a model that
    // cannot stop deliberating will not be talked out of it on the third ask.
    if (toolCalls.isEmpty() && m_streamContent.trimmed().isEmpty()
        && m_lastRawFrame.contains(QStringLiteral("\"length\""))
        && m_lengthDeaths < 2) {
        m_lengthDeaths++;
        nikitaLogAs(assistantName(),
                   QStringLiteral("round hit the output cap with nothing to show (%1/2) -- retrying "
                                  "with a stop-deliberating nudge").arg(m_lengthDeaths));
        m_history.append(QJsonObject{
            {"role", "system"},
            {"content", QStringLiteral(
                "Your last reply used its ENTIRE output budget thinking and produced nothing. "
                "Do not transcribe or analyse the screen art pixel by pixel -- glance at it, "
                "find the highlighted bar and the words you can make out, and move on. "
                "Right now: make ONE tool call, the single next step toward what was asked. "
                "No commentary, no analysis, just the call. If you genuinely cannot read the "
                "screen, press one button and read it again.")}
        });
        m_currentReply = nullptr;
        dispatchTurn();
        return;
    }
    // Progress, not a budget, decides whether to keep going. A round counts as
    // progress if it asked for something this turn has not already done.
    bool madeProgress = false;
    for (const QJsonValue &v : toolCalls) {
        const QJsonObject fn = v.toObject().value("function").toObject();
        const QString sig = fn.value("name").toString() + QLatin1Char('\u0001')
                          + QString::fromUtf8(QJsonDocument(fn.value("arguments").toObject())
                                              .toJson(QJsonDocument::Compact));
        if (!m_turnCallSigs.contains(sig)) { madeProgress = true; }
        m_turnCallSigs.insert(sig);
    }
    if (madeProgress) { m_repeatRounds = 0; }
    else              { m_repeatRounds++; }

    const bool keepGoing = m_repeatRounds < NIKITA_MAX_REPEAT_ROUNDS
                           && m_toolRounds < NIKITA_TOOL_ROUND_CEILING;
    if (!toolCalls.isEmpty() && keepGoing) {
        m_history.append(QJsonObject{
            {"role", "assistant"},
            {"content", m_streamContent},
            {"tool_calls", toolCalls}
        });
        m_toolRounds++;
        m_currentReply = nullptr;     // this reply is done; ignore its finished()
        runToolCalls(toolCalls, 0);   // -> dispatchTurn() again (new reply)
        return;
    }

    // The turn is ending with tool calls still on the table -- it did not finish,
    // it ran out of rope: the same calls kept coming back round after round.
    const bool stalled = !toolCalls.isEmpty() && !keepGoing;

    m_currentReply = nullptr;
    setThinking(false);
    // Prefer this round's prose; fall back to the best prose from earlier rounds
    // of the same turn (the text-plus-tool-call case). Only if BOTH are empty
    // does the model genuinely have nothing to say.
    QString text = stripNonEnglish(m_streamContent);
    if (text.trimmed().isEmpty()) {
        text = stripNonEnglish(m_turnText);
    }
    // A stalled turn must NOT sign off with something it said on the way in.
    // "You're in BadUSB. Backing out." was round one narrating its first move;
    // shown as the last word of a turn that then pressed twenty buttons and
    // gave up, it reads as a completed report of a thing that never happened.
    // Better to say plainly that it did not get there.
    if (stalled && stripNonEnglish(m_streamContent).trimmed().isEmpty()) {
        text = QStringLiteral("I didn't get there. I kept going round the same step, so I stopped "
                              "rather than keep pressing buttons blind. Tell me what's on the "
                              "screen now and I'll pick it up from there.");
        nikitaLog(QStringLiteral("turn stalled: %1 repeat round(s), dropped stale mid-turn prose")
                      .arg(m_repeatRounds));
    }
    // Noted before anything fills the gap. A turn that produced no words AND
    // ran no tool is not an answer -- it is the absence of one, and the
    // fallbacks below would otherwise turn that absence into "Done."
    const bool saidNothing = text.trimmed().isEmpty() && !m_turnRanAnyTool;

    // "Done." with no tool behind it.
    //
    // Checked on what the model SAID, not on what the user typed. Every other
    // guard here reads the request through a keyword list and asks "was this
    // meant to be an action?" -- and that list has been one word short at every
    // turn: create-a-folder, then remove-the-folder, then open-safari. Reading
    // the reply skips the question entirely. A bare "Done." after a turn in
    // which nothing ran is a false report no matter what was asked, and no
    // vocabulary is needed to see it.
    //
    // Bounded to a short reply so a real explanation that happens to contain
    // "done" is left alone.
    bool claimedWithoutActing = false;
    if (!m_turnRanAnyTool && text.trimmed().size() < 60) {
        static const QStringList kClaims = {
            QStringLiteral("done"), QStringLiteral("created"), QStringLiteral("saved"),
            QStringLiteral("deleted"), QStringLiteral("removed"), QStringLiteral("renamed"),
            QStringLiteral("moved"), QStringLiteral("copied"), QStringLiteral("wrote"),
            QStringLiteral("written"), QStringLiteral("added"), QStringLiteral("opened"),
            QStringLiteral("executed"), QStringLiteral("ran ")
        };
        const QString low = text.toLower();
        for (const QString &c : kClaims) {
            if (low.contains(c)) { claimedWithoutActing = true; break; }
        }
    }

    // The all-or-nothing version of that check is what let the folder-and-file
    // case through: host_mkdir HAD run, so m_turnRanAnyTool was true, and a
    // reply claiming a file had also been created sailed past. "Some tool ran"
    // is not the question. The question is whether the tools that ran cover
    // what the sentence says happened.
    // Which artifacts the reply named but never called a tool for. Knowing the
    // names is what turns a vague "try again" into an instruction that can
    // actually be followed.
    QStringList missingArtifacts;
    const bool claimedUnrunAction = claimsUnrunAction(text, &missingArtifacts);
    if (claimedUnrunAction) { claimedWithoutActing = true; }

    if (text.trimmed().isEmpty()) {
        // Only when nothing was supposed to happen. After a turn that was asked
        // to act and ran no tool, "Done." is a one-word confirmation of nothing
        // at all -- the exact failure the rest of this function exists to catch,
        // delivered by the fallback that was meant to be harmless.
        text = saidNothing ? QString() : QStringLiteral("Done.");
    }

    // memory.txt stores facts in third person -- "User's name is Nico" -- because
    // that is the file's format. Asked what it knew, the model read a line back
    // exactly as written, and the answer came out sounding like a database row
    // being printed rather than someone talking to the person in front of them.
    //
    // The prompt asks for second person, but a 3B follows a phrasing rule about
    // half the time, and this is cheap and unambiguous to fix here: an assistant
    // talking TO someone never opens a sentence with "User". Only the leading
    // word is touched, so a sentence that mentions "user" anywhere else is left
    // alone.
    {
        QString t = text.trimmed();
        if (t.startsWith(QLatin1String("User's "), Qt::CaseInsensitive)) {
            text = QStringLiteral("Your ") + t.mid(7);
        } else if (t.startsWith(QLatin1String("User is "), Qt::CaseInsensitive)) {
            text = QStringLiteral("You are ") + t.mid(8);
        } else if (t.startsWith(QLatin1String("User has "), Qt::CaseInsensitive)) {
            text = QStringLiteral("You have ") + t.mid(9);
        } else if (t.startsWith(QLatin1String("User "), Qt::CaseInsensitive)) {
            QString rest = t.mid(5);
            // "User lives in Dublin" -> "You lives in Dublin" was worse than the
            // problem it replaced. Third person singular carries the -s on the
            // verb, and moving the subject to "you" has to take it back off.
            static const QHash<QString, QString> kVerbs = {
                {QStringLiteral("lives"),    QStringLiteral("live")},
                {QStringLiteral("works"),    QStringLiteral("work")},
                {QStringLiteral("uses"),     QStringLiteral("use")},
                {QStringLiteral("prefers"),  QStringLiteral("prefer")},
                {QStringLiteral("keeps"),    QStringLiteral("keep")},
                {QStringLiteral("likes"),    QStringLiteral("like")},
                {QStringLiteral("wants"),    QStringLiteral("want")},
                {QStringLiteral("owns"),     QStringLiteral("own")},
                {QStringLiteral("runs"),     QStringLiteral("run")},
                {QStringLiteral("speaks"),   QStringLiteral("speak")},
                {QStringLiteral("studies"),  QStringLiteral("study")},
                {QStringLiteral("needs"),    QStringLiteral("need")},
                {QStringLiteral("wears"),    QStringLiteral("wear")},
                {QStringLiteral("does"),     QStringLiteral("do")},
                {QStringLiteral("was"),      QStringLiteral("were")}
            };
            const QString firstWord = rest.section(QLatin1Char(' '), 0, 0);
            const auto it = kVerbs.constFind(firstWord.toLower());
            if (it != kVerbs.constEnd()) {
                rest = it.value() + rest.mid(firstWord.size());
            }
            text = QStringLiteral("You ") + rest;
        }
    }

    // Anti-hallucination trap. The turn asked to WRITE a file, the model is now
    // ending the turn, and NOT ONE tool ran -- so any "done / saved / created"
    // is a claim about something that never happened (the /sdcard + run_cli
    // invention chains off exactly this). Don't relay the lie. Replace it with
    // an honest line and DON'T record the false claim in history, so the next
    // turn isn't reasoning on top of a fabricated save.
    const bool falseClaim = m_turnWasFileAction && !m_turnRanAnyTool;

    // Two ways a turn fails, one answer to both: think again.
    //
    // falseClaim is the model describing something it did not do. saidNothing is
    // the model producing nothing whatsoever -- no words, no call -- which
    // happens when a malformed tool call is eaten in parsing, and used to reach
    // the user as the word "Done."
    //
    // saidNothing matters most because it needs no keyword list to detect. The
    // other guards depend on messageIsFileWrite recognising the verb, and that
    // list will always be one word behind. Silence is unambiguous: it is never a
    // legitimate reply to anything, whatever was asked.
    // A three-part request needs three chances to be corrected, not one. The
    // single-shot version spent its only retry catching the missing
    // helloworld.py and then had nothing left when the very next reply invented
    // lolo.txt as well. The bound is on corrections that achieve nothing, which
    // is the same rule the tool loop uses: keep going while progress is being
    // made, stop when it is not.
    if ((falseClaim || saidNothing || claimedWithoutActing)
        && m_forcedRetry < NIKITA_MAX_CORRECTIONS) {
        m_forcedRetry++;
        // The turn may have arrived with only the memory tools attached, which
        // is how it ended up with nothing to call in the first place. Retrying
        // without lifting this would hand it the same empty toolbox and get the
        // same answer.
        m_turnNeedsTools = true;
        // Nothing is pushed into the chat body between rounds any more. This
        // used to emit a "continuing..." bubble to fill the pause before the
        // next round, from back when a held-back turn left the pane blank. The
        // footer now carries the live state -- elapsed, tokens, "thinking" --
        // and every tool has its own row, so a placeholder line in the
        // transcript is just noise the user asked to be rid of. The footer
        // keeps moving; the conversation stays clean.

        // What the RETRY is told. Pointing at a guessed tool is what made this
        // loop six times: the request said "create a folder", so the guess was
        // host_mkdir, the folder already existed, and the model correctly saw
        // nothing to do and answered in prose again. When the checklist knows
        // which item is missing, name the item -- an instruction that can be
        // followed beats one that has to be interpreted.
        const QString only = missingArtifacts.isEmpty()
                           ? forcedToolName()
                           : (m_turnFocus == 2 ? QStringLiteral("host_write")
                                               : QStringLiteral("save_file"));
        // The log keeps the blunt wording. The developer reading it needs to know
        // this was a false claim, not a tidy continuation.
        nikitaLogAs(assistantName(),
                   QStringLiteral("%1 -- %2")
                       .arg(saidNothing ? QStringLiteral("empty reply")
                          : (claimedWithoutActing ? QStringLiteral("claimed success, ran nothing")
                                                  : QStringLiteral("no call")),
                            missingArtifacts.isEmpty()
                              ? QStringLiteral("rethinking, %1 first").arg(only)
                              : QStringLiteral("still missing: %1").arg(
                                    missingArtifacts.join(QStringLiteral(", ")))));
        // Named item first. Everything else is a fallback for the case where the
        // checklist has nothing concrete to point at.
        QString correction;
        if (!missingArtifacts.isEmpty()) {
            correction = QStringLiteral(
                "STOP. %1 does not exist. You named it as created, but no tool has been called "
                "for it -- saying so does not make it so.\n\n%2\n"
                "Call %3 for %4 right now, with the full path and the exact content the user "
                "asked for. Only the call: no explanation, no apology, no code block.")
                .arg(missingArtifacts.join(QStringLiteral(" and ")),
                     buildChecklist(), only, missingArtifacts.first());
        } else if (saidNothing || claimedWithoutActing) {
            correction = QStringLiteral(
                "That last reply did not do anything -- you called no tool, so nothing "
                "changed and nothing was created, deleted or opened. Do it now: if the request "
                "needs a tool, call it (%1 is the likely one, but pick what actually fits); if "
                "it only needs an answer, give the answer. Never reply with nothing, and never "
                "say it is done unless a tool told you it was.").arg(only);
        } else {
            correction = QStringLiteral(
                "You answered in words but called no tool, so nothing happened -- what you "
                "described did not occur. Call a tool now, this message. %1 is most likely the "
                "right one, but pick whichever actually matches what was asked: if they said "
                "delete, delete; if they said create, create. Only the call -- no explanation, "
                "no apology, no code block. Give it the full path.").arg(only);
        }
        m_history.append(QJsonObject{{"role", "user"}, {"content", correction}});
        m_streamContent.clear();
        m_turnText.clear();
        m_streamTools = QJsonArray();
        setThinking(true);
        redispatch();
        return;
    }

    if (falseClaim) {
        // Was wrong twice over on a delete: nothing was written, and the Flipper
        // had nothing to do with it. Say only what is true of any failed action.
        text = QStringLiteral("That didn't go through -- I ran nothing, so nothing changed. "
                              "Say it once more with the exact path and I'll do it.");
        setThinking(false);
        m_currentReply = nullptr;
        m_lastTurnWasAction = false;
        emitReply(text);
        return;   // history stays clean -- no fabricated assistant turn recorded
    }

    // Second trap: a tool DID run but returned an error, and the model is still
    // claiming success. This is the /sdcard/MARIO case -- make_dir rejected the
    // path, the model said "Created folder" anyway. If the reply sounds like a
    // success ("created / saved / done / added / removed") while a tool errored
    // this turn, replace it with the actual tool error so the user sees the
    // truth (the wrong path, the real reason) instead of a phantom success.
    if (m_turnHadToolError) {
        const QString low = text.toLower();
        static const QStringList successWords = {
            QStringLiteral("created"), QStringLiteral("saved"), QStringLiteral("done"),
            QStringLiteral("added"), QStringLiteral("removed"), QStringLiteral("deleted"),
            QStringLiteral("renamed"), QStringLiteral("wrote"), QStringLiteral("made"),
            QStringLiteral("success")
        };
        bool claimsSuccess = false;
        for (const QString &w : successWords) { if (low.contains(w)) { claimsSuccess = true; break; } }
        if (claimsSuccess) {
            // Pull the human-readable reason out of the last tool error in history.
            QString reason;
            for (int i = m_history.size() - 1; i >= 0; --i) {
                const QJsonObject o = m_history.at(i).toObject();
                if (o.value("role").toString() != QLatin1String("tool")) { continue; }
                const QString c = o.value("content").toString();
                const int e = c.indexOf(QLatin1String("\"error\""));
                if (e < 0) { continue; }
                // grab the message between the error value's quotes
                const int q1 = c.indexOf(QLatin1Char('"'), e + 7);
                const int q2 = (q1 >= 0) ? c.indexOf(QLatin1Char('"'), q1 + 1) : -1;
                reason = (q1 >= 0 && q2 > q1) ? c.mid(q1 + 1, q2 - q1 - 1) : c;
                break;
            }
            text = reason.isEmpty()
                ? QStringLiteral("That didn't go through -- the operation returned an error, so nothing changed.")
                : QStringLiteral("That didn't go through: %1").arg(reason);
            setThinking(false);
            m_currentReply = nullptr;
            m_lastTurnWasAction = false;
            emitReply(text);
            return;   // don't record the false success
        }
    }

    // The turn is over and nothing else will run. Now -- and only now -- may the
    // lessons be written, and only if the turn earned it.
    //
    // A lesson is a claim that a sequence of calls SOLVES a request. Filing that
    // claim after a turn that errored, that lied about what it did, or that
    // said nothing at all, teaches the model that the broken path was the right
    // one. Learning from a failure is worse than not learning: it is a wrong
    // answer that persists to the SD card and comes back in every later prompt.
    const bool turnIsSound = m_turnRanAnyTool
                             && !m_turnHadToolError
                             && !falseClaim
                             && !saidNothing
                             && !claimedWithoutActing;
    if (turnIsSound) {
        flushPendingMoves();
    } else if (!m_pendingMoves.isEmpty()) {
        nikitaLogAs(assistantName(),
                   QStringLiteral("not learning from this turn: %1")
                       .arg(m_turnHadToolError    ? QStringLiteral("a tool returned an error")
                          : claimedWithoutActing  ? QStringLiteral("claimed a step it never ran")
                          : saidNothing           ? QStringLiteral("produced no reply")
                                                  : QStringLiteral("the turn did not complete")));
        m_pendingMoves.clear();
    }

    m_lastTurnWasAction = m_turnRanAnyTool;
    // Two turns of headroom: one for the user to notice something is wrong, one
    // for them to say so.
    if (m_turnRanAnyTool)          { m_toolTurnCooldown = 2; }
    else if (m_toolTurnCooldown)   { m_toolTurnCooldown--; }
    // Remembered across the turn boundary so the follow-up is armed. Cleared
    // the moment something actually runs.
    m_lastTurnMissed = m_turnWasFileAction && !m_turnRanAnyTool;

    m_history.append(QJsonObject{{"role", "assistant"}, {"content", text}});
    saveHistory();
    emitReply(text);   // QML finalizes the live bubble
}


void NikitaBackend::onStreamFinished(QNetworkReply *reply)
{
    if (reply != m_currentReply) {  // already finalized (done seen) or superseded
        reply->deleteLater();
        return;
    }
    m_currentReply = nullptr;
    const auto netErr = reply->error();
    const QString netErrStr = reply->errorString();
    // The whole reply, accumulated by onStreamData without being parsed. On an
    // error this is the API's own error document, which is where the useful
    // reason lives -- Qt's status line only ever says "server replied: ...".
    const QByteArray fullBody = m_streamBuf + reply->readAll();
    // Read before the reply is retired: 401, 402 and 429 are three completely
    // different things to tell someone, and errorString() does not reliably
    // carry the number.
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // The server's own "wait this long" for a 429, in seconds. Preferred over a
    // guessed backoff whenever it is present.
    const QByteArray retryAfter = reply->rawHeader(QByteArrayLiteral("Retry-After"));
    reply->deleteLater();

    // A 429 is a per-minute throttle, not a failure of this request -- the same
    // bytes will go through once the window rolls over. So ride it out here
    // instead of surfacing it: wait (the server's Retry-After if it gave one,
    // otherwise a widening backoff) and send the exact same turn again. Bounded,
    // because if the account's tier is simply too low no amount of waiting fixes
    // it, and an unbounded retry would just hang forever looking busy.
    if (httpStatus == 429 && netErr != QNetworkReply::NoError
        && m_apiRateRetry < NIKITA_API_MAX_RATE_RETRIES) {
        m_apiRateRetry++;
        int waitMs = 0;
        bool ok = false;
        const int hinted = QString::fromLatin1(retryAfter).trimmed().toInt(&ok);
        if (ok && hinted > 0) {
            waitMs = hinted * 1000;
        } else {
            // 5s, 12s, 25s, 40s. The account's real ceiling is huge (Tier2:
            // 3M TPM, 500 RPM), so a 429 here is not us exhausting a quota -- it
            // is a short burst throttle, or the kimi-k3 preview's own tighter
            // cap. Those clear in seconds, so lead with a short wait; the longer
            // steps are only there for a rare stubborn stretch. Retry-After is
            // honoured above whenever the server sends it. The footer shows
            // "rate limited, waiting" with STOP available throughout.
            static const int backoff[] = { 5000, 12000, 25000, 40000 };
            const int idx = qBound(0, m_apiRateRetry - 1, 3);
            waitMs = backoff[idx];
        }
        nikitaLogAs(assistantName(),
            QStringLiteral("rate limited (429) -- retry %1/%2 in %3s%4")
                .arg(m_apiRateRetry).arg(NIKITA_API_MAX_RATE_RETRIES).arg(waitMs / 1000)
                .arg(ok ? QStringLiteral(" (server asked)") : QString()));
        // Keep thinking true and the footer alive; say what is happening there
        // rather than in the chat body.
        setTurnStatus(QStringLiteral("rate limited, waiting"));
        QTimer::singleShot(waitMs, this, [this]() {
            if (m_thinking) { dispatchTurn(); }   // same turn, rebuilt from history
        });
        return;
    }

    setThinking(false);

    // stopThinking() aborted this on purpose -- that is a stop, not a failure,
    // and it is checked before anything else so the error paths below can never
    // claim it. Whatever had already arrived stays on screen, the same way
    // interrupting a response works everywhere else.
    if (m_userStoppedThinking) {
        m_userStoppedThinking = false;
        if (!m_streamContent.trimmed().isEmpty()) {
            const QString text = stripNonEnglish(m_streamContent);
            m_history.append(QJsonObject{{"role", "assistant"}, {"content", text}});
            saveHistory();
            emitReply(text);
        } else {
            emit errorOccurred(QStringLiteral("Stopped."));
        }
        return;
    }

    if (netErr == QNetworkReply::NoError) {
        const QJsonObject resp = QJsonDocument::fromJson(fullBody).object();
        if (resp.value(QStringLiteral("choices")).toArray().isEmpty()) {
            // 200 with no choices is not success. Say what came back rather
            // than falling through to "lost the thread", which blames the model
            // for something the endpoint did.
            const QString why = resp.value(QStringLiteral("error")).toObject()
                                    .value(QStringLiteral("message")).toString();
            emit errorOccurred(QStringLiteral("The API answered with nothing usable%1")
                                   .arg(why.isEmpty() ? QStringLiteral(".")
                                                      : QStringLiteral(": ") + why));
            return;
        }
        // A clean round means the throttle has passed; the next 429 this turn
        // gets its full retry budget again.
        m_apiRateRetry = 0;
        // setThinking(false) above is undone here: the turn is not over if the
        // reply carried tool calls, and consumeModelFrame -> finalizeStream is
        // what decides that.
        consumeModelFrame(normaliseApiReply(resp));
        return;
    }

    // The ones that actually happen, named in the user's own terms. "invalid
    // api key" on an account with no credit sends people off to regenerate a
    // key that was never the problem.
    const QString detail = QJsonDocument::fromJson(fullBody).object()
                               .value(QStringLiteral("error")).toObject()
                               .value(QStringLiteral("message")).toString();
    QString msg;
    if (httpStatus == 401 || httpStatus == 403 || fullBody.contains("invalid_api_key")
        || fullBody.contains("Invalid Authentication")) {
        msg = QStringLiteral("the API rejected the key (read from %1). Check it in setup.")
                  .arg(apiKeySource().isEmpty() ? QStringLiteral("nowhere") : apiKeySource());
    } else if (httpStatus == 402 || fullBody.contains("insufficient")
               || fullBody.contains("quota")) {
        msg = QStringLiteral("the account is out of credit.");
    } else if (httpStatus == 429) {
        msg = QStringLiteral("still rate limited after %1 retries. Your account's per-minute "
                             "limit is the ceiling here, not your balance -- wait a minute, or "
                             "raise the limit in the Kimi console.").arg(NIKITA_API_MAX_RATE_RETRIES);
    } else if (netErr == QNetworkReply::HostNotFoundError
               || netErr == QNetworkReply::ConnectionRefusedError
               || netErr == QNetworkReply::TimeoutError) {
        msg = QStringLiteral("can't reach the API -- check the network.");
    } else {
        // Whatever the endpoint said, verbatim. "invalid temperature: only 1 is
        // allowed for this model" is a far better bug report than any sentence
        // this function could invent for it.
        msg = detail.isEmpty() ? netErrStr : detail;
    }
    emit errorOccurred(QStringLiteral("Hrm: %1").arg(msg));
}

// Which tool names actually count as having performed each kind of action. A
// sentence claiming a file exists is only true if one of the file-writing tools
// ran; a folder claim needs a directory tool. Deliberately per-machine-agnostic
// -- the model confuses the two machines often enough that requiring the exact
// one would produce false alarms on work that really did happen.
static const char *const kToolsThatWrite[] = {
    "save_file", "host_write", "host_copy", "host_move", "rename_file", "host_run"
};
static const char *const kToolsThatMakeDirs[] = {
    "make_dir", "host_mkdir", "host_run"
};
static const char *const kToolsThatDelete[] = {
    "delete_file", "host_delete", "host_run"
};

static bool nikitaRanAnyOf(const QSet<QString> &ran, const char *const *names, int count)
{
    for (int i = 0; i < count; ++i) {
        if (ran.contains(QLatin1String(names[i]))) { return true; }
    }
    return false;
}

// Does this reply assert an action whose tool never ran this turn?
//
// Only fires on a claim the sentence makes in the past tense about a concrete
// artifact. "I can create a file for you" is not a claim; "created an empty
// file named heythere.py" is.
bool NikitaBackend::claimsUnrunAction(const QString &reply, QStringList *missing) const
{
    const QString low = reply.toLower();

    // A file was claimed into existence.
    static const QStringList fileWords = {
        QStringLiteral("file"), QStringLiteral(".py"), QStringLiteral(".txt"),
        QStringLiteral(".sh"), QStringLiteral(".js"), QStringLiteral(".json"),
        QStringLiteral(".sub"), QStringLiteral(".nfc"), QStringLiteral(".ir"),
        QStringLiteral("script"),
    };
    static const QStringList madeWords = {
        QStringLiteral("created"), QStringLiteral("wrote"), QStringLiteral("written"),
        QStringLiteral("saved"), QStringLiteral("added"), QStringLiteral("placed"),
        QStringLiteral("put "), QStringLiteral("generated"),
        // Claims of EXISTENCE, not just of creation. The model dodged every one
        // of the verbs above by writing "the BadUSB script is already at
        // /ext/badusb/fib_alert.txt" -- asserting the file was there (from an
        // earlier turn it imagined) without ever calling save_file this turn.
        // A claim that the deliverable exists is as checkable as a claim that it
        // was made, and the name-by-name test below catches the lie either way.
        QStringLiteral("already at"), QStringLiteral("already exists"),
        QStringLiteral("already there"), QStringLiteral("already on"),
        QStringLiteral("is saved"), QStringLiteral("is ready"), QStringLiteral("is on the flipper"),
        QStringLiteral("is on your flipper"), QStringLiteral("sitting at"),
        QStringLiteral("lives at"), QStringLiteral("is in place"),
    };
    bool saysMade = false;
    for (const QString &w : madeWords) { if (low.contains(w)) { saysMade = true; break; } }

    if (saysMade) {
        bool saysFile = false;
        for (const QString &w : fileWords) { if (low.contains(w)) { saysFile = true; break; } }
        if (saysFile && !nikitaRanAnyOf(m_turnToolsRan, kToolsThatWrite,
                                        int(sizeof(kToolsThatWrite) / sizeof(*kToolsThatWrite)))) {
            return true;
        }
        if ((low.contains(QLatin1String("folder")) || low.contains(QLatin1String("directory")))
            && !nikitaRanAnyOf(m_turnToolsRan, kToolsThatMakeDirs,
                               int(sizeof(kToolsThatMakeDirs) / sizeof(*kToolsThatMakeDirs)))) {
            return true;
        }
    }

    if (low.contains(QLatin1String("deleted")) || low.contains(QLatin1String("removed"))) {
        if (!nikitaRanAnyOf(m_turnToolsRan, kToolsThatDelete,
                            int(sizeof(kToolsThatDelete) / sizeof(*kToolsThatDelete)))) {
            return true;
        }
    }

    // Name-by-name, which is the only check that scales to a request with three
    // parts in it. "Created helloworld.py. Created lolo.txt." passes every
    // whole-turn test the moment ONE of them is written -- so instead, pull the
    // filenames out of the sentence and require that the turn actually touched
    // each one. A file the model has never called a tool for is a file it
    // invented, however confidently the sentence reads.
    if (saysMade) {
        // No spaces in the name, and the extension must be letters. Allowing
        // either turned "Created LOLO.txt" into one token and "version 1.2"
        // into a filename, and both then read as artifacts nobody wrote. A
        // genuine name with a space in it is missed instead -- which fails the
        // safe way, by not accusing.
        static const QRegularExpression fileRe(
            QStringLiteral("([\\w()-]{1,60}\\.[A-Za-z]{1,8})\\b"));
        auto it = fileRe.globalMatch(reply);
        while (it.hasNext()) {
            const QString named = it.next().captured(1).trimmed().toLower();
            // Skip things that look like a filename but are prose or a version
            // number: "e.g." and "1.2" are not artifacts.
            if (named.size() < 4 || named.startsWith(QLatin1Char('.'))) { continue; }
            if (m_turnPathsTouched.contains(QFileInfo(named).fileName())) { continue; }
            // Collected rather than returned on: the correction has to NAME the
            // thing that is missing, and there may be more than one.
            if (missing && !missing->contains(named)) { *missing += named; }
        }
    }
    return missing ? !missing->isEmpty() : false;
}

// Every artifact the user's own message asks for, by name -- the only honest
// reference for "am I done": a list of tools already called says what
// happened, not what was wanted, so a model that's done 2 of 3 things can't
// tell from that list alone.
QStringList NikitaBackend::requestedArtifacts(const QString &userText)
{
    // No spaces, letter extension -- a name the user typed is unambiguous.
    static const QRegularExpression fileRe(
        QStringLiteral("([\\w()-]{1,60}\\.[A-Za-z]{1,8})\\b"));
    QStringList out;
    auto it = fileRe.globalMatch(userText);
    while (it.hasNext()) {
        const QString n = it.next().captured(1).trimmed();
        if (n.size() < 4 || n.startsWith(QLatin1Char('.'))) { continue; }
        if (!out.contains(n, Qt::CaseInsensitive)) { out += n; }
    }
    return out;
}

// The checklist, rendered from the request and what has actually been touched.
// Handed back to the model after every round so it can locate itself in the
// job instead of guessing from the conversation, which is where a small model
// loses the third item of a three-item request.
QString NikitaBackend::buildChecklist() const
{
    const QStringList wanted = requestedArtifacts(m_lastUserText);
    if (wanted.isEmpty()) { return QString(); }

    QString out = QStringLiteral("CHECKLIST for what was asked:\n");
    int pending = 0;
    for (const QString &w : wanted) {
        const bool done = m_turnPathsTouched.contains(QFileInfo(w).fileName().toLower());
        if (!done) { ++pending; }
        out += QStringLiteral("  [%1] %2%3\n")
               .arg(done ? QStringLiteral("x") : QStringLiteral(" "), w,
                    done ? QString() : QStringLiteral("   <-- NOT DONE, no tool has been called for this"));
    }
    out += pending
         ? QStringLiteral("\n%1 item(s) still not done. Call the tool for the next unticked one NOW. "
                          "Do not answer in words, and do not describe an item as created while its "
                          "box is empty -- an empty box means the file does not exist.\n").arg(pending)
         : QStringLiteral("\nEvery item is ticked. You may answer now.\n");
    return out;
}

void NikitaBackend::appendContinuationNudge()
{
    if (m_turnToolsRan.isEmpty()) { return; }
    QStringList ran = m_turnToolsRan.values();
    std::sort(ran.begin(), ran.end());

    QString body = QStringLiteral(
        "Tools that have actually run this turn: %1. Creating a folder is not the same as "
        "creating a file inside it: each needs its own call.\n").arg(ran.join(QStringLiteral(", ")));

    // The checklist goes in whenever the request named anything, because it is
    // the part that says what is LEFT rather than what is behind.
    const QString list = buildChecklist();
    if (!list.isEmpty()) { body += QLatin1Char('\n') + list; }
    else {
        body += QStringLiteral(
            "If the request had more steps than these, call the next tool NOW -- do not answer "
            "in words yet, and never describe a step you have not called a tool for.");
    }

    m_history.append(QJsonObject{
        {"role", "tool"},
        {"content", QJsonDocument(QJsonObject{{"_progress", body}}).toJson(QJsonDocument::Compact)
                    .constData()}
    });
}

void NikitaBackend::runToolCalls(const QJsonArray &toolCalls, int index)
{
    if (index == 0 && !toolCalls.isEmpty()) { setTurnStatus(QStringLiteral("getting to work")); }
    if (index >= toolCalls.size()) {
        // A small model treats one successful tool as the whole job done. Asked
        // to "create a folder and put an empty file in it" it calls host_mkdir,
        // gets {"created":true}, and answers "created the folder, and inside it
        // an empty heythere.py" -- narrating a second step it never took.
        //
        // The tool loop was never the problem: it had eleven rounds left and
        // went back to the model. The model simply stopped. So the round it
        // comes back to now carries an explicit ledger of what has actually
        // run, and an instruction to keep calling rather than to summarise.
        // ---- Second factor: check the work ourselves before paying for prose --
        //
        // The tool already reported {"verified":true}; this checks the disk
        // directly, which is the only evidence that does not come from the
        // model. When it holds, going back to the model buys one sentence --
        // and measured here that sentence cost four minutes, longer than the
        // write it was describing (2 ms). The trail on screen already says
        // what happened, line by line, so the sentence adds nothing the user
        // cannot already read.
        //
        // It closes the turn when every artifact is confirmed present and no
        // tool errored. A request that reads as two jobs gets one extra round
        // to finish the second -- but only one. After that, work that checks
        // out on disk is done, and another round is the model re-examining
        // something already correct, which is the wait with nothing behind it.
        if (turnWorkVerified()) {
            nikitaLogAs(assistantName(),
                       QStringLiteral("verified on disk -- closing without a summary round"));
            // File the lesson HERE too. The normal path does this after the
            // summary round; skipping that round skipped the learning with it,
            // so every turn that closed early taught nothing and
            // actions-memory.txt stayed empty. This close is the strongest
            // evidence there is -- the artifact was just confirmed on disk --
            // so it is exactly the turn worth learning from.
            if (!m_turnHadToolError) { flushPendingMoves(); }
            m_currentReply = nullptr;
            setThinking(false);
            m_history.append(QJsonObject{{"role", "assistant"},
                                         {"content", QStringLiteral("Done.")}});
            saveHistory();
            emitReply(QString());
            return;
        }

        appendContinuationNudge();
        // Tools are done; what is left is the model writing the sentence that
        // reports them. On a 4B that is its own slow round, and it is exactly
        // where the window used to go quiet after the work had already landed.
        setTurnStatus(m_turnToolsRan.isEmpty() ? QStringLiteral("thinking")
                                               : QStringLiteral("wrapping up"));
        redispatch();
        return;
    }

    const QJsonObject fn = toolCalls.at(index).toObject().value("function").toObject();
    const QString name = fn.value("name").toString();
    const QJsonObject args = fn.value("arguments").toObject();

    // The phrase the user sees while THIS tool runs. Set before the call, not
    // after, so the window changes the moment the work changes.
    setTurnStatus(nikitaToolStatus(name));

    // A line in the chat the moment it STARTS. Until this existed the trail
    // only grew when a tool FINISHED, so a slow call (a shell command, a write
    // over USB) left the chat showing nothing at all while it ran.
    //
    // Its own row, with its own id, and not text folded into the reply bubble:
    // a step that is running and the same step once it is done are one event
    // that changes, and the row is rewritten in place when the result lands.
    m_activeToolSeq = ++m_toolSeq;
    emit toolActivity(m_activeToolSeq, nikitaToolStatus(name),
                      nikitaToolDetail(name, args), false, false);

    runOneTool(name, args, [this, toolCalls, index, name, args](const QString &result) {
        // Remember if a tool failed this turn. Small models cheerfully report
        // "Created folder /sdcard/MARIO" even when make_dir came back with
        // {"error":"No such path..."} -- the tool did the right thing and
        // rejected it, but the model narrated success anyway. finalizeStream
        // uses this to stop relaying a success that didn't happen.
        if (result.contains(QLatin1String("\"error\""))) { m_turnHadToolError = true; }
        else {
            m_turnToolsRan.insert(name);
            // The name of the tool is not enough to check a claim against. A
            // reply saying "created helloworld.py and lolo.txt" passes a
            // did-any-write-tool-run test after writing only the first of them.
            // What has to be checked is the artifact, so remember every path
            // this turn actually touched.
            for (const char *key : { "path", "to", "destination", "filename", "name" }) {
                const QString p = args.value(QLatin1String(key)).toString().trimmed();
                if (!p.isEmpty()) { m_turnPathsTouched.insert(QFileInfo(p).fileName().toLower()); }
            }
        }
        m_history.append(QJsonObject{{"role", "tool"}, {"content", result}});
        runToolCalls(toolCalls, index + 1);
    });
}

// Is this path unmistakably on one machine or the other? "Unmistakably" is
// deliberately narrow: only a form that cannot mean anything else.
static bool nikitaIsDevicePath(const QString &p)
{
    const QString t = p.trimmed();
    return t == QLatin1String("/ext") || t == QLatin1String("/int")
        || t.startsWith(QLatin1String("/ext/")) || t.startsWith(QLatin1String("/int/"));
}

static bool nikitaIsComputerPath(const QString &p)
{
    const QString t = p.trimmed();
    if (t.startsWith(QLatin1Char('~'))) { return true; }
    if (t.size() > 2 && t.at(1) == QLatin1Char(':') && t.at(0).isLetter()) { return true; }  // C:\...
    // Any other absolute path. Listing /Users/ and /home/ only covered macOS and
    // Linux desktops; it missed /var, /opt, /srv, /media, /mnt, and every layout
    // on a machine that isn't the one this was written on. There are exactly two
    // paths that belong to the Flipper, so everything else is here.
    return QDir::isAbsolutePath(t) && !nikitaIsDevicePath(t);
}

// Given a tool the model picked and a path belonging to the other machine,
// what should have been called? The tool CHOICE is a guess from wording; the
// PATH is a fact -- when they disagree, the path wins and the call reroutes
// rather than spending a turn on a refusal.
static QString nikitaReroute(const QString &name, bool toHost)
{
    static const QHash<QString, QString> kToHost = {
        {QStringLiteral("save_file"),   QStringLiteral("host_write")},
        {QStringLiteral("read_file"),   QStringLiteral("host_read")},
        {QStringLiteral("list_files"),  QStringLiteral("host_list")},
        {QStringLiteral("make_dir"),    QStringLiteral("host_mkdir")},
        {QStringLiteral("delete_file"), QStringLiteral("host_delete")},
        {QStringLiteral("rename_file"), QStringLiteral("host_move")},
    };
    static const QHash<QString, QString> kToDevice = {
        {QStringLiteral("host_write"),  QStringLiteral("save_file")},
        {QStringLiteral("host_read"),   QStringLiteral("read_file")},
        {QStringLiteral("host_list"),   QStringLiteral("list_files")},
        {QStringLiteral("host_mkdir"),  QStringLiteral("make_dir")},
        {QStringLiteral("host_delete"), QStringLiteral("delete_file")},
        {QStringLiteral("host_move"),   QStringLiteral("rename_file")},
        {QStringLiteral("host_copy"),   QStringLiteral("rename_file")},
    };
    return toHost ? kToHost.value(name) : kToDevice.value(name);
}

// Which machine was that path meant for? A weak model routes by vibe: told to
// "save it on Desktop" it wrote /ext/Desktop/hello.txt and reported "it's on
// your Desktop" -- wrong tool, wrong path, silent. Empty return means fine.
// A shell command aimed at the wrong shell: run_cli only knows the Flipper's
// CLI, so "open -a Safari" is silently ignored there -- nothing fails, so
// there's no error to notice, and the turn just reports success anyway.
static bool nikitaIsHostCommand(const QString &cmd)
{
    static const QStringList kHostOnly = {
        QStringLiteral("open -a"), QStringLiteral("open http"), QStringLiteral("osascript"),
        QStringLiteral("/usr/"), QStringLiteral("/bin/"), QStringLiteral("/opt/"),
        QStringLiteral("~/"), QStringLiteral("/Users/"), QStringLiteral(".app"),
        QStringLiteral("http://"), QStringLiteral("https://"),
        QStringLiteral("brew "), QStringLiteral("git "), QStringLiteral("make "),
        QStringLiteral("python"), QStringLiteral("npm "), QStringLiteral("curl "),
        QStringLiteral("sudo "), QStringLiteral("cd ")
    };
    for (const QString &h : kHostOnly) {
        if (cmd.contains(h, Qt::CaseInsensitive)) { return true; }
    }
    return false;
}

static QString nikitaWrongMachine(const QString &path, bool deviceTool)
{
    const QString p = path.trimmed();
    if (p.isEmpty()) { return QString(); }

    // Folders that only ever exist on a computer. Only flagged at the top level
    // of the card -- /ext/apps_data/something/Documents is nobody's home folder.
    static const QStringList kHomeNames = {
        QStringLiteral("Desktop"), QStringLiteral("Downloads"), QStringLiteral("Documents"),
        QStringLiteral("Library"),  QStringLiteral("Applications"), QStringLiteral("Users"),
        QStringLiteral("Movies"),   QStringLiteral("Music"), QStringLiteral("Pictures"),
        QStringLiteral("Public"),   QStringLiteral("home")
    };

    if (deviceTool) {
        if (p.startsWith(QLatin1String("~")) || p.startsWith(QLatin1String("/Users/"))
            || p.startsWith(QLatin1String("/home/")) || p.startsWith(QLatin1String("C:"))) {
            return QStringLiteral("that is a path on the computer, not on the Flipper. "
                                  "Use the host_ tools for it (host_write, host_mkdir, host_delete).");
        }
        const QStringList seg = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (seg.size() >= 2 && (seg.at(0) == QLatin1String("ext") || seg.at(0) == QLatin1String("int"))
            && kHomeNames.contains(seg.at(1), Qt::CaseInsensitive)) {
            return QStringLiteral("\"%1\" is a folder on the computer, not on the SD card -- "
                                  "there is no %1 under /ext. If the user meant their computer, "
                                  "use host_write with ~/%1/... instead. If they really meant the "
                                  "Flipper, pick a real card folder (/ext/apps, /ext/badusb, /ext/nfc, "
                                  "or just /ext).").arg(seg.at(1));
        }
        return QString();
    }

    // Mirror case: a host tool pointed at the card.
    if (p.startsWith(QLatin1String("/ext")) || p.startsWith(QLatin1String("/int"))) {
        return QStringLiteral("/ext and /int are on the Flipper, not on this computer. "
                              "Use the SD-card tools for them (save_file, make_dir, delete_file).");
    }
    return QString();
}

// One short line a person can read, built from what the tool actually
// returned -- shown while the model is still composing its own sentence
// about it, so the chat isn't blank during that round trip.
static QString nikitaToolHeadline(const QString &tool, const QString &result)
{
    const QJsonObject o = QJsonDocument::fromJson(result.toUtf8()).object();
    if (o.contains(QStringLiteral("error"))) {
        return QStringLiteral("%1 failed: %2").arg(tool, o.value("error").toString());
    }
    if (o.contains(QStringLiteral("wrote"))) {
        return QStringLiteral("wrote %1 (%2 bytes)")
                   .arg(o.value("wrote").toString()).arg(o.value("bytes").toInt());
    }
    if (o.contains(QStringLiteral("deleted"))) {
        return o.value("deleted").toBool()
                   ? QStringLiteral("deleted %1").arg(o.value("path").toString())
                   : QStringLiteral("%1 was not there").arg(o.value("path").toString());
    }
    if (o.contains(QStringLiteral("created"))) {
        return o.value("created").toBool()
                   ? QStringLiteral("created %1").arg(o.value("path").toString())
                   : QStringLiteral("%1 already existed").arg(o.value("path").toString());
    }
    if (o.contains(QStringLiteral("moved")))  { return QStringLiteral("moved to %1").arg(o.value("to").toString()); }
    if (o.contains(QStringLiteral("copied"))) { return QStringLiteral("copied to %1").arg(o.value("to").toString()); }
    if (o.contains(QStringLiteral("exit_code"))) {
        const int code = o.value("exit_code").toInt();
        return code == 0 ? QStringLiteral("ran it") : QStringLiteral("command exited %1").arg(code);
    }
    if (o.contains(QStringLiteral("cwd")))   { return QStringLiteral("now in %1").arg(o.value("cwd").toString()); }
    if (o.contains(QStringLiteral("saved"))) { return QStringLiteral("saved %1").arg(o.value("saved").toString()); }
    return tool;
}

// Turn the Flipper's raw framebuffer into something the model can READ. The
// screen is 1 bit per pixel, packed the way u8g2 packs it: byte index
// (y/8)*width + x, bit y%8 (see ScreenCanvas::setFrame, the app's own decoder).
//
// Out comes an ASCII picture at real pixel scale -- a set pixel is '#', a clear
// one a space -- so the model reads the menu text off the letter shapes with no
// font atlas. Plus one structural note: the Flipper draws the SELECTED list row
// as a filled bar with the text knocked out, so a band of near-full scan-lines
// marks what is currently chosen.
// Parse a Flipper .ir universal asset and build the `ir tx` command line for
// every PARSED signal whose name matches the requested button. This is what
// makes "fire the universal TV power" a single deterministic CLI action instead
// of menu navigation: the asset is just named signals, and each parsed one maps
// straight onto `ir tx <protocol> <address> <command>` (the safe CLI form).
//
// The .ir stores address/command as 4 little-endian hex bytes; the CLI wants the
// value, so the bytes are folded LE into a number and printed as hex. Raw signals
// are skipped -- txing raw over the CLI is a different, fiddly format -- and their
// count is reported so the caller knows some codes were not sent.
static QStringList nikitaIrTxCommandsFor(const QString &irFile, const QString &button,
                                         int *rawSkipped)
{
    QStringList cmds;
    if (rawSkipped) { *rawSkipped = 0; }
    const QString want = button.trimmed().toLower();

    QString name, type, protocol, address, command;
    auto flush = [&]() {
        const bool match = name.trimmed().toLower() == want
                        || name.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_')) == want;
        if (match && !type.isEmpty()) {
            if (type.trimmed().toLower() == QLatin1String("parsed") && !protocol.isEmpty()) {
                auto le = [](const QString &bytes) -> QString {
                    const QStringList b = bytes.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                                                Qt::SkipEmptyParts);
                    quint32 v = 0; int sh = 0;
                    for (const QString &one : b) { v |= (one.toUInt(nullptr, 16) & 0xFF) << sh; sh += 8; }
                    return QStringLiteral("%1").arg(v, 0, 16);
                };
                cmds << QStringLiteral("ir tx %1 %2 %3")
                            .arg(protocol.trimmed(), le(address), le(command));
            } else if (rawSkipped) {
                ++(*rawSkipped);
            }
        }
        name.clear(); type.clear(); protocol.clear(); address.clear(); command.clear();
    };

    const QStringList lines = irFile.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('#'))) { flush(); continue; }
        const int c = line.indexOf(QLatin1Char(':'));
        if (c < 0) { continue; }
        const QString k = line.left(c).trimmed().toLower();
        const QString v = line.mid(c + 1).trimmed();
        if (k == QLatin1String("name"))          { flush(); name = v; }
        else if (k == QLatin1String("type"))     { type = v; }
        else if (k == QLatin1String("protocol")) { protocol = v; }
        else if (k == QLatin1String("address"))  { address = v; }
        else if (k == QLatin1String("command"))  { command = v; }
    }
    flush();
    return cmds;
}

static QString nikitaRenderScreen(const ScreenFrame &f)
{
    const int w = f.size.width();
    const int h = f.size.height();
    if (w <= 0 || h <= 0 || f.pixelData.isEmpty()) { return QString(); }

    auto on = [&](int x, int y) -> bool {
        const int i = (y / 8) * w + x;
        if (i < 0 || i >= f.pixelData.size()) { return false; }
        return (static_cast<unsigned char>(f.pixelData.at(i)) >> (y % 8)) & 1;
    };

    QVector<int> rowFill(h, 0);
    for (int y = 0; y < h; ++y) {
        int c = 0;
        for (int x = 0; x < w; ++x) { if (on(x, y)) { ++c; } }
        rowFill[y] = c;
    }
    int hlTop = -1, hlBot = -1;
    for (int y = 0; y < h; ++y) {
        if (rowFill[y] > w * 6 / 10) { if (hlTop < 0) { hlTop = y; } hlBot = y; }
    }

    QString ascii;
    ascii.reserve((w + 1) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) { ascii += on(x, y) ? QLatin1Char('#') : QLatin1Char(' '); }
        ascii += QLatin1Char('\n');
    }

    QString out = QStringLiteral("Flipper screen, %1x%2. Read the text from the block art below "
                                 "('#' = a lit pixel). ").arg(w).arg(h);
    if (hlTop >= 0) {
        out += QStringLiteral("The SELECTED/highlighted item is inside the filled bar at vertical "
                              "pixels %1-%2 -- its text shows as the letters knocked out (spaces) of "
                              "that solid band. ").arg(hlTop).arg(hlBot);
    } else {
        out += QStringLiteral("No highlighted selection bar detected this frame. ");
    }
    out += QStringLiteral("\n\n") + ascii;
    return out;
}

void NikitaBackend::runOneTool(const QString &name, const QJsonObject &args, std::function<void(const QString &)> done)
{
    // Every action the assistant takes is logged here rather than inside each
    // handler: one place, so a tool added later can't quietly skip the log.
    // Read-only lookups are noise, so only the ones that change something (or
    // touch the device) are recorded, plus any failure.
    static const QStringList kLoggedTools = {
        QStringLiteral("save_file"), QStringLiteral("make_dir"), QStringLiteral("delete_file"),
        QStringLiteral("rename_file"), QStringLiteral("run_cli"), QStringLiteral("press_button"),
        QStringLiteral("remember"), QStringLiteral("forget")
    };
    m_turnRanAnyTool = true;   // a tool is actually executing this turn

    // The real access-filter barrier. The list offered to the model is already
    // pruned, but a small model invents tool names -- and an older conversation
    // still in history carries calls from when the access was allowed. So the
    // check that counts is this one, at execution time.
    if (!toolAllowed(name)) {
        const QString gid = nikitaToolGroups().value(name);
        QString label = gid;
        for (int i = 0; i < NIKITA_FILTER_COUNT; ++i) {
            if (gid == QLatin1String(NIKITA_FILTERS[i].id)) {
                label = QString::fromUtf8(NIKITA_FILTERS[i].label);
                break;
            }
        }
        nikitaLog(QStringLiteral("blocked by filter: %1 (%2)").arg(name, label));
        done(QStringLiteral("BLOCKED: the user has turned off \"%1\" access, so %2 did not run. "
                            "Say so plainly in one line -- do not retry, do not try another tool to "
                            "get around it, and do not claim you did it.").arg(label, name));
        return;
    }

    // A macOS command handed to the Flipper's shell. Rerouted rather than
    // refused: the command is right, only the machine was wrong, and sending it
    // to /bin/sh is exactly what the user asked for.
    if (name == QLatin1String("run_cli")
        && nikitaIsHostCommand(args.value("command").toString())) {
        QJsonObject fixed = args;
        fixed["command"] = args.value("command").toString();
        nikitaLogAs(assistantName(),
                   QStringLiteral("run_cli -> host_run (that command is for this computer): %1")
                       .arg(args.value("command").toString().left(80)));
        runOneTool(QStringLiteral("host_run"), fixed, done);
        return;
    }

    // Before anything runs: which machine does this path actually name?
    {
        static const QStringList kDevicePathTools = {
            QStringLiteral("save_file"), QStringLiteral("make_dir"), QStringLiteral("delete_file"),
            QStringLiteral("read_file"), QStringLiteral("list_files"), QStringLiteral("file_info"),
            QStringLiteral("rename_file")
        };
        const bool deviceTool = kDevicePathTools.contains(name);
        const bool hostTool   = name.startsWith(QLatin1String("host_"))
                                && name != QLatin1String("host_run")
                                && name != QLatin1String("host_cd");
        if (deviceTool || hostTool) {
            const QString first = args.contains(QLatin1String("path"))
                                      ? args.value("path").toString()
                                      : args.value("from").toString();

            // The path contradicts the tool, and says so unambiguously. Send the
            // call where it was always going.
            const bool wantsHost   = deviceTool && nikitaIsComputerPath(first);
            const bool wantsDevice = hostTool   && nikitaIsDevicePath(first);
            if (wantsHost || wantsDevice) {
                const QString target = nikitaReroute(name, wantsHost);
                if (!target.isEmpty()) {
                    nikitaLogAs(assistantName(),
                               QStringLiteral("%1 -> %2 (path is on the %3): %4")
                                   .arg(name, target,
                                        wantsHost ? QStringLiteral("computer") : QStringLiteral("Flipper"),
                                        first));
                    runOneTool(target, args, done);
                    return;
                }
            }

            // Not unambiguous, but still suspicious enough to stop: a computer
            // folder name invented under /ext. Nothing can be inferred here, so
            // this one does cost a turn.
            for (const QString &key : {QStringLiteral("path"), QStringLiteral("from"), QStringLiteral("to")}) {
                if (!args.contains(key)) { continue; }
                const QString why = nikitaWrongMachine(args.value(key).toString(), deviceTool);
                if (!why.isEmpty()) {
                    nikitaLogAs(assistantName(),
                               QStringLiteral("%1 REFUSED (wrong machine): %2=%3")
                                   .arg(name, key, args.value(key).toString()));
                    done(QStringLiteral("{\"error\":\"%1\"}").arg(why));
                    return;
                }
            }
        }
    }
    // Anything touching this computer is logged, reads included. With the whole
    // disk reachable, "what did it look at" is as much a part of the trail as
    // "what did it change" -- and the trail is the only way to answer that
    // afterwards.
    // EVERY call, not just the ones that change something. The log used to skip
    // read-only lookups as noise, which meant the one question the log exists to
    // answer -- what did it actually do, and with what -- had a hole in it for
    // half the tool surface. Reads on this computer were already logged for that
    // reason; the Flipper's reads deserve the same.
    {
        // Still needed after the log gate went away: the chat mirror below is
        // for host actions only, not for every Flipper read.
        const bool isHost = name.startsWith(QLatin1String("host_"));
        QStringList bits;
        for (auto it = args.begin(); it != args.end(); ++it) {
            QString v = it.value().toVariant().toString().simplified();
            // File contents can be kilobytes; the log wants the action, not the payload.
            if (v.size() > 60) { v = v.left(60) + QStringLiteral("...(%1 chars)").arg(it.value().toString().size()); }
            bits << QStringLiteral("%1=%2").arg(it.key(), v);
        }
        // Arrow in, arrow out: the call and its answer read as a pair even when
        // several are interleaved in one turn.
        const QString line = QStringLiteral("\u2192 %1(%2)").arg(name, bits.join(QLatin1String(", ")));
        nikitaLogAs(assistantName(), line);
        // Wrap the callback so the ANSWER is logged too. Knowing a write was
        // attempted is half the story; the other half is whether the tool came
        // back with a path or with an error, and that half was invisible.
        auto inner = done;
        const QJsonObject learnArgs = args;
        // By value. This lambda runs when the tool answers, and by then the
        // next call in the batch may already have opened a row of its own.
        const int seq = m_activeToolSeq;
        done = [this, name, learnArgs, inner, seq](const QString &result) {
            nikitaLogAs(assistantName(),
                       QStringLiteral("\u2190 %1: %2").arg(name, result.left(300)));
            // Into the chat before the model has written a word about it, and
            // into the SAME row the start opened -- the trail reads as a list
            // of what happened, not as every state each step passed through.
            // Parsed, not substring-matched: a host_read of a log file full of
            // the word "error" is a successful read.
            const QJsonObject parsed = QJsonDocument::fromJson(result.toUtf8()).object();
            const bool failed = parsed.contains(QStringLiteral("error"));
            emit toolActivity(seq, nikitaToolHeadline(name, result),
                              nikitaToolDetail(name, learnArgs), true, failed);
            // Learn only from proof -- but QUEUE the lesson, do not write it yet.
            //
            // Recording it here, between one tool and the next, broke multi-step
            // work in two ways. The lesson pairs the tool with the user's WHOLE
            // message, so a two-part request ("make a folder AND put a file in
            // it") got filed as "host_mkdir works for <the entire request>" --
            // and that line goes into the next round's system prompt, where it
            // reads as: this request is handled, by that one call. The model
            // then stopped and narrated a second step it never took.
            //
            // It also cost a disk write plus a USB round trip after every
            // successful tool, in the middle of the turn, before the model was
            // asked what to do next.
            //
            // Queued here, flushed once the turn is genuinely over.
            // press_button and read_screen are NEVER recorded as reusable moves.
            // A button sequence only reproduces from the EXACT screen it started
            // on -- replayed from anywhere else it lands somewhere random (the
            // "ok on Sub-GHz instead of Infrared" failure). Storing it as "this
            // works for that request" is worse than storing nothing: it teaches a
            // sequence that will not repeat. Navigation is decided live from the
            // screen, not from memory.
            const bool positionDependent = (name == QLatin1String("press_button")
                                          || name == QLatin1String("read_screen"));
            if (nikitaResultProves(result) && !positionDependent) {
                m_pendingMoves.append(qMakePair(name, learnArgs));
            }
            if (inner) { inner(result); }
        };
        // Host actions also surface in the chat itself. The log panel is where
        // you look when something already went wrong; the chat is where you are
        // actually watching, and an agent with the run of the disk should not be
        // able to do anything you can only find out about later.
        if (isHost) { emit hostActionRan(line); }
    }

    const QString who = assistantName();
    auto logged = [who, name, done](const QString &result) {
        if (result.contains(QLatin1String("\"error\""))) {
            nikitaLogAs(who, QStringLiteral("%1 FAILED: %2").arg(name, result.left(200)));
        }
        done(result);
    };
    done = logged;

    // Host-workspace tools run on THIS computer, not the Flipper -- no device needed.
    // Every host_* tool routes here. Matching on the prefix means a tool added
    // to runHostTool can't be forgotten in this list and silently 404.
    if (name.startsWith(QLatin1String("host_"))) {
        runHostTool(name, args, done);
        return;
    }

    // Remembering a fact is local (+ best-effort SD mirror); no device required.
    if (name == QLatin1String("remember")) {
        const QString fact = args.value("fact").toString().trimmed();
        if (fact.isEmpty()) { done(QStringLiteral("{\"error\":\"no fact given\"}")); return; }
        rememberFact(fact);
        syncMemoryToFlipper();
        done(QStringLiteral("{\"remembered\":true}"));
        return;
    }
    if (name == QLatin1String("list_memory")) {
        if (m_memory.trimmed().isEmpty()) {
            done(QStringLiteral("{\"memory\":\"(empty -- I don't have any saved facts yet)\"}"));
        } else {
            // Wrap in an object to get a properly JSON-escaped string value.
            const QByteArray js = QJsonDocument(QJsonObject{{"memory", m_memory}})
                                      .toJson(QJsonDocument::Compact);
            done(QString::fromUtf8(js));
        }
        return;
    }
    if (name == QLatin1String("forget")) {
        const QString match = args.value("match").toString().trimmed();
        const int removed = forgetFacts(match);
        syncMemoryToFlipper();
        done(QStringLiteral("{\"forgotten\":%1}").arg(removed));
        return;
    }

    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) {
        done(QStringLiteral("{\"error\":\"No Flipper is connected or ready right now.\"}"));
        return;
    }

    if (name == QLatin1String("list_files")) {
        const QByteArray path = args.value("path").toString(QStringLiteral("/ext")).toUtf8();
        if (const QString err = badStoragePath(QString::fromUtf8(path)); !err.isEmpty()) { done(err); return; }
        auto *op = dev->rpc()->storageList(path);
        connect(op, &AbstractOperation::finished, this, [op, done]() {
            if (op->isError()) {
                done(QStringLiteral("{\"error\":\"%1\"}").arg(op->errorString()));
                return;
            }
            QJsonArray arr;
            const auto &files = op->files();
            for (const FileInfo &f : files) {
                arr.append(QJsonObject{
                    {"name", QString::fromUtf8(f.name)},
                    {"type", f.type == FileType::Directory ? "dir" : "file"},
                    {"size", static_cast<double>(f.size)}
                });
            }
            done(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        });

    } else if (name == QLatin1String("read_file")) {
        const QByteArray path = args.value("path").toString().toUtf8();
        if (path.isEmpty()) {
            done(QStringLiteral("{\"error\":\"no path given\"}"));
            return;
        }
        if (const QString err = badStoragePath(QString::fromUtf8(path)); !err.isEmpty()) { done(err); return; }
        QBuffer *buf = new QBuffer(this);
        buf->open(QIODevice::ReadWrite);
        auto *op = dev->rpc()->storageRead(path, buf);
        connect(op, &AbstractOperation::finished, this, [op, buf, done]() {
            QString result;
            if (op->isError()) {
                result = QStringLiteral("{\"error\":\"%1\"}").arg(op->errorString());
            } else {
                QByteArray d = buf->data();
                const bool truncated = d.size() > NIKITA_READ_CAP;
                if (truncated) {
                    d = d.left(NIKITA_READ_CAP);
                }
                result = QString::fromUtf8(d);
                if (truncated) {
                    result += QStringLiteral("\n...(truncated)");
                }
                if (result.isEmpty()) {
                    result = QStringLiteral("(empty file)");
                }
            }
            buf->deleteLater();
            done(result);
        });

    } else if (name == QLatin1String("run_cli")) {
        const QString command = args.value("command").toString().trimmed();
        if (command.isEmpty()) {
            done(QStringLiteral("{\"error\":\"no command given\"}"));
            return;
        }
        if (!m_cli) {
            done(QStringLiteral("{\"error\":\"CLI not available\"}"));
            return;
        }
        // Refuse commands known to crash the firmware, BEFORE they reach the
        // device. `ir universal ...` over the CLI faults stock firmware (NULL
        // deref -> reboot), and a crash the user sees on the Flipper reads as
        // "it didn't work" even when the intent was fine. Blocking it here means
        // the device never crashes and the model falls back to the safe path
        // (navigate the Infrared app by button, or `ir tx` a known code).
        // `ir universal list <remote>` is safe and useful -- it prints the valid
        // signal names, which is the one thing that must never be guessed. It is
        // the SEND form that takes the device down when the signal name is not
        // in that list (`ir universal tv power`, lowercase, rebooted it), so
        // only that half is blocked, and it is blocked in favour of the
        // ir_universal TOOL, which does the same job through `ir tx` and cannot
        // pass a bad name. Blocking the listing too was leaving the model no way
        // to learn the names it was being told not to guess.
        if (command.contains(QRegularExpression(QStringLiteral("^\\s*ir\\s+universal\\b"),
                                                QRegularExpression::CaseInsensitiveOption))
            && !command.contains(QRegularExpression(QStringLiteral("^\\s*ir\\s+universal\\s+list\\b"),
                                                    QRegularExpression::CaseInsensitiveOption))) {
            done(QStringLiteral("{\"error\":\"sending with `ir universal` crashes this firmware "
                "when the signal name is not exactly one from `ir universal list <remote>` -- not run. "
                "Use the ir_universal TOOL instead (remote: tv/ac/audio/projector, button: Power), "
                "which sends the same codes through `ir tx` and cannot pass a bad name.\"}"));
            return;
        }
        // Isolated one-shot: pauses RPC, runs the command, hands RPC back.
        m_cli->runOneShot(command, [this, done](bool ok, QString out) {
            QJsonObject r;
            if (ok) {
                r["output"] = out;
                // Same as host_run: remember a clean result so a later save can
                // pull it in via {{LAST_RESULT}} rather than the model retyping.
                const QString trimmed = out.trimmed();
                m_lastRunOutput = (trimmed.size() <= 4096) ? trimmed : QString();
            } else {
                r["error"]  = out;
            }
            done(QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
        });

    } else if (name == QLatin1String("ir_universal")) {
        const QString remote = args.value("remote").toString().trimmed().toLower();
        QString button = args.value("button").toString().trimmed();
        if (button.isEmpty()) { button = QStringLiteral("Power"); }
        static const QStringList known = {QStringLiteral("tv"), QStringLiteral("ac"),
            QStringLiteral("audio"), QStringLiteral("projector")};
        if (!known.contains(remote)) {
            done(QStringLiteral("{\"error\":\"unknown remote '%1' (use tv/ac/audio/projector)\"}").arg(remote));
            return;
        }
        // Checked BEFORE the file is read, not after: failing at the transmit
        // meant the read had already happened and the turn had already narrated
        // progress it could not finish.
        if (deviceOverBle()) {
            done(QStringLiteral("{\"error\":\"the universal remote needs the Flipper's CLI, which "
                 "only runs over USB. On Bluetooth there is no way to transmit -- plug the cable in, "
                 "or tell the user to fire it from the Flipper's own Infrared > Universal Remotes.\"}"));
            return;
        }
        if (!m_cli) { done(QStringLiteral("{\"error\":\"CLI not available\"}")); return; }
        const QByteArray irPath = QStringLiteral("/ext/infrared/assets/%1.ir").arg(remote).toUtf8();
        QBuffer *buf = new QBuffer(this);
        buf->open(QIODevice::ReadWrite);
        auto *op = dev->rpc()->storageRead(irPath, buf);
        connect(op, &AbstractOperation::finished, this, [this, op, buf, remote, button, done]() {
            if (op->isError()) {
                done(QStringLiteral("{\"error\":\"couldn't read the universal %1 remote: %2\"}")
                         .arg(remote, op->errorString()));
                buf->deleteLater();
                return;
            }
            const QString content = QString::fromUtf8(buf->data());
            buf->deleteLater();
            int rawSkipped = 0;
            auto cmds = std::make_shared<QStringList>(
                nikitaIrTxCommandsFor(content, button, &rawSkipped));
            if (cmds->isEmpty()) {
                done(QStringLiteral("{\"error\":\"no sendable '%1' codes in the %2 universal remote "
                     "(%3 raw-only codes skipped)\"}").arg(button, remote).arg(rawSkipped));
                return;
            }
            // Chain the transmits: one ir tx at a time, next only after the last
            // finished, so the CLI one-shot is never re-entered. This is the
            // brute-force the universal remote does -- every brand's code, in turn.
            auto idx = std::make_shared<int>(0);
            auto sent = std::make_shared<int>(0);
            auto total = cmds->size();
            auto rawCount = std::make_shared<int>(rawSkipped);
            std::shared_ptr<std::function<void()>> step = std::make_shared<std::function<void()>>();
            *step = [this, cmds, idx, sent, total, rawCount, remote, button, done, step]() {
                if (*idx >= cmds->size()) {
                    done(QStringLiteral("{\"sent\":%1,\"button\":\"%2\",\"remote\":\"%3\","
                         "\"note\":\"transmitted %1 %2 code(s) for %3 -- every brand in the "
                         "universal set%4. If the TV did not respond, it may use a code not in the "
                         "set.\"}")
                        .arg(*sent).arg(button, remote)
                        .arg(*rawCount > 0 ? QStringLiteral(" (%1 raw code(s) not sendable via CLI)")
                                                 .arg(*rawCount) : QString()));
                    return;
                }
                const QString cmd = cmds->at((*idx)++);
                m_cli->runOneShot(cmd, [sent, step](bool ok, QString) {
                    if (ok) { ++(*sent); }
                    (*step)();
                });
            };
            (*step)();
        });

    } else if (name == QLatin1String("read_screen")) {
        auto *ss = m_appBackend ? m_appBackend->screenStreamer() : nullptr;
        if (!ss) { done(QStringLiteral("{\"error\":\"no screen stream available\"}")); return; }
        // Make sure the stream is live. It usually is (the app mirrors the
        // screen), but if it was paused/stopped the frame is stale or empty --
        // enable it and give it a beat to arrive before reading.
        if (!ss->isEnabled()) { ss->setEnabled(true); }
        auto grab = [this, ss, done]() {
            const QString rendered = nikitaRenderScreen(ss->screenFrame());
            if (rendered.isEmpty()) {
                done(QStringLiteral("{\"error\":\"the screen is not readable right now -- the "
                                    "stream may still be starting; try read_screen again in a moment\"}"));
                return;
            }
            // The render is plain text with newlines; wrap it as a JSON string
            // safely (paths/art can contain characters that break hand-built JSON).
            const QJsonObject o{{QStringLiteral("screen"), rendered}};
            done(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
        };
        // A frame already in hand -> read now; otherwise wait briefly for one.
        if (!ss->screenFrame().pixelData.isEmpty()) { grab(); }
        else { QTimer::singleShot(350, this, grab); }

    } else if (name == QLatin1String("press_button")) {
        const QString b = args.value("button").toString().toLower();
        int times = args.value("times").toInt(1);
        if (times < 1) times = 1;
        if (times > NIKITA_MAX_PRESSES) times = NIKITA_MAX_PRESSES;

        // OK and BACK, nothing else. The D-pad is gone on purpose: every attempt
        // to walk a menu with up/down/left/right was a count made from a screen
        // it could not reliably read, and a wrong count fires the wrong saved
        // remote or opens the wrong app. Refusing here rather than only in the
        // prompt is what makes it stick -- the schema no longer offers those
        // buttons, and a model that asks for one anyway is told where to go.
        int key = -1;
        if (b == QLatin1String("ok") || b == QLatin1String("enter") || b == QLatin1String("center")) key = InputEvent::Ok;
        else if (b == QLatin1String("back")) key = InputEvent::Back;

        if (key < 0) {
            const bool dpad = (b == QLatin1String("up") || b == QLatin1String("down")
                            || b == QLatin1String("left") || b == QLatin1String("right"));
            done(dpad
                ? QStringLiteral("{\"error\":\"'%1' is not available. There is no D-pad navigation: "
                                 "menu walking is done through the CLI instead. To move between apps "
                                 "use run_cli(loader open <App>) / run_cli(loader close); for files "
                                 "use fls and fcat; to fire an IR signal use ir tx or ir universal. "
                                 "press_button only does ok and back.\"}").arg(b)
                : QStringLiteral("{\"error\":\"unknown button '%1' (only ok and back exist)\"}").arg(b));
            return;
        }

        // Replicate a real D-pad tap (Press + Short + Release) per press.
        Flipper::Zero::ProtobufSession *rpc = dev->rpc();
        Flipper::Zero::GuiSendInputOperation *lastOp = nullptr;
        for (int i = 0; i < times; ++i) {
            rpc->guiSendInput(key, InputEvent::Press);
            rpc->guiSendInput(key, InputEvent::Short);
            lastOp = rpc->guiSendInput(key, InputEvent::Release);
        }
        if (lastOp) {
            connect(lastOp, &AbstractOperation::finished, this, [this, b, times, done]() {
                // Hand back the RESULTING screen with the press. This is what
                // makes navigation reliable without the model having to remember
                // to call read_screen: it SEES where each press landed and picks
                // the next move from that. A short wait lets the screen redraw
                // after the input before we capture it. Old screens collapse to a
                // placeholder in the window (see dispatchTurn), so this does not
                // pile up.
                QTimer::singleShot(300, this, [this, b, times, done]() {
                    auto *ss = m_appBackend ? m_appBackend->screenStreamer() : nullptr;
                    QString screen;
                    if (ss) {
                        if (!ss->isEnabled()) { ss->setEnabled(true); }
                        screen = nikitaRenderScreen(ss->screenFrame());
                    }
                    QJsonObject o{{QStringLiteral("pressed"), b}, {QStringLiteral("times"), times}};
                    if (!screen.isEmpty()) { o.insert(QStringLiteral("screen"), screen); }
                    done(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
                });
            });
        } else {
            done(QStringLiteral("{\"error\":\"nothing pressed\"}"));
        }

    } else if (name == QLatin1String("save_file")) {
        QByteArray path = args.value("path").toString().toUtf8();
        QString content = substituteRunResult(args.value("content").toString());
        if (path.isEmpty()) {
            done(QStringLiteral("{\"error\":\"no path given\"}"));
            return;
        }
        if (const QString err = badStoragePath(QString::fromUtf8(path)); !err.isEmpty()) { done(err); return; }
        // BadUSB payloads MUST be .txt on the Flipper -- .duk (or anything else the
        // model picks) simply won't run. Force the extension and clean the Ducky,
        // so a sloppy model still produces a file that actually works.
        if (QString::fromUtf8(path).startsWith(QLatin1String("/ext/badusb/"), Qt::CaseInsensitive)) {
            QString p = QString::fromUtf8(path);
            const int slash = p.lastIndexOf(QLatin1Char('/'));
            const int dot = p.lastIndexOf(QLatin1Char('.'));
            if (dot > slash) { p = p.left(dot) + QStringLiteral(".txt"); }
            else            { p += QStringLiteral(".txt"); }
            path = p.toUtf8();
            content = sanitizeDuckyScript(content);
        }
        // Overwrite guard lives in beginFlipperSave: it stats the path first and,
        // if a file is already there, asks the user (Replace / Rename / Cancel)
        // before anything is written.
        beginFlipperSave(path, content, done);

    } else if (name == QLatin1String("make_dir")) {
        const QByteArray path = args.value("path").toString().toUtf8();
        if (path.isEmpty()) { done(QStringLiteral("{\"error\":\"no path given\"}")); return; }
        if (const QString err = badStoragePath(QString::fromUtf8(path)); !err.isEmpty()) { done(err); return; }
        ensureFlipperDir(path, [path, done]() {
            done(QStringLiteral("{\"created\":\"%1\"}").arg(QString::fromUtf8(path)));
        });

    } else if (name == QLatin1String("delete_file")) {
        const QByteArray path = args.value("path").toString().toUtf8();
        const bool recursive = args.value("recursive").toBool(false);
        if (path.isEmpty()) { done(QStringLiteral("{\"error\":\"no path given\"}")); return; }
        if (const QString err = badStoragePath(QString::fromUtf8(path)); !err.isEmpty()) { done(err); return; }
        auto *op = dev->rpc()->storageRemove(path, recursive);
        connect(op, &AbstractOperation::finished, this, [op, path, done]() {
            if (op->isError()) {
                done(QStringLiteral("{\"error\":\"%1\"}").arg(op->errorString()));
            } else {
                done(QStringLiteral("{\"deleted\":\"%1\"}").arg(QString::fromUtf8(path)));
            }
        });

    } else if (name == QLatin1String("rename_file")) {
        const QByteArray from = args.value("from").toString().toUtf8();
        const QByteArray to   = args.value("to").toString().toUtf8();
        if (from.isEmpty() || to.isEmpty()) { done(QStringLiteral("{\"error\":\"need both 'from' and 'to'\"}")); return; }
        if (const QString err = badStoragePath(QString::fromUtf8(from)); !err.isEmpty()) { done(err); return; }
        if (const QString err = badStoragePath(QString::fromUtf8(to));   !err.isEmpty()) { done(err); return; }
        const QByteArray parent = QString::fromUtf8(to).section('/', 0, -2).toUtf8();
        QPointer<Flipper::FlipperZero> devRef(dev);
        ensureFlipperDir(parent, [this, devRef, from, to, done]() {
            Flipper::FlipperZero *dev = devRef.data();
            if (!dev) { done(QStringLiteral("{\"error\":\"the Flipper was disconnected\"}")); return; }
            auto *op = dev->rpc()->storageRename(from, to);
            connect(op, &AbstractOperation::finished, this, [op, from, to, done]() {
                if (op->isError()) {
                    done(QStringLiteral("{\"error\":\"%1\"}").arg(op->errorString()));
                } else {
                    done(QStringLiteral("{\"renamed\":\"%1\",\"to\":\"%2\"}")
                             .arg(QString::fromUtf8(from), QString::fromUtf8(to)));
                }
            });
        });

    } else if (name == QLatin1String("file_info")) {
        const QByteArray path = args.value("path").toString().toUtf8();
        if (path.isEmpty()) { done(QStringLiteral("{\"error\":\"no path given\"}")); return; }
        if (const QString err = badStoragePath(QString::fromUtf8(path)); !err.isEmpty()) { done(err); return; }
        auto *op = dev->rpc()->storageStat(path);
        connect(op, &AbstractOperation::finished, this, [op, done]() {
            if (op->isError()) {
                done(QStringLiteral("{\"error\":\"%1\"}").arg(op->errorString()));
                return;
            }
            if (!op->hasFile()) {
                done(QStringLiteral("{\"exists\":false}"));
                return;
            }
            const bool isDir = op->type() == Flipper::Zero::StorageStatOperation::Directory;
            done(QStringLiteral("{\"exists\":true,\"type\":\"%1\",\"size\":%2}")
                     .arg(isDir ? QStringLiteral("dir") : QStringLiteral("file"))
                     .arg(static_cast<double>(op->size())));
        });

    } else {
        done(QStringLiteral("{\"error\":\"unknown tool '%1'\"}").arg(name));
    }
}

// Stat the target first. If a file is already there, hand the choice to the
// user (Replace / Rename / Cancel) instead of silently clobbering it -- the
// Flipper's storageWrite overwrites without a word, which is fine for the
// assistant's own working files but not for something the user may have named
// deliberately. A missing file, a stat error, or a directory-in-the-way all
// fall through to the normal write, which reports any real failure itself.
void NikitaBackend::beginFlipperSave(const QByteArray &path, const QString &content,
                                     std::function<void(const QString &)> done)
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (!dev) { done(QStringLiteral("{\"error\":\"the Flipper was disconnected\"}")); return; }

    auto *op = dev->rpc()->storageStat(path);
    connect(op, &AbstractOperation::finished, this, [this, op, path, content, done]() {
        const bool exists = !op->isError() && op->hasFile()
                            && op->type() != Flipper::Zero::StorageStatOperation::Directory;
        if (!exists) {
            writeFlipperFile(path, content, done);
            return;
        }
        // Hold everything until the user answers. A second save cannot start
        // meanwhile: the model is waiting on this tool result.
        m_pendingSavePath = path;
        m_pendingSaveContent = content;
        m_pendingSaveDone = done;
        QString preview = content.left(400);
        if (content.size() > 400) { preview += QStringLiteral("\n...(truncated)"); }
        emit saveConflictRequested(QString::fromUtf8(path), preview);
    });
}

// The actual write, once the path is settled. Shared by the no-conflict path
// and by Replace/Rename.
void NikitaBackend::writeFlipperFile(const QByteArray &path, const QString &content,
                                     std::function<void(const QString &)> done)
{
    const QByteArray parent = QString::fromUtf8(path).section(QLatin1Char('/'), 0, -2).toUtf8();
    ensureFlipperDir(parent, [this, path, content, done]() {
        Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
        if (!dev) { done(QStringLiteral("{\"error\":\"the Flipper was disconnected\"}")); return; }
        QBuffer *buf = new QBuffer(this);
        buf->setData(content.toUtf8());
        buf->open(QIODevice::ReadOnly);
        auto *op = dev->rpc()->storageWrite(path, buf);
        connect(op, &AbstractOperation::finished, this, [this, op, buf, path, done]() {
            QString result;
            if (op->isError()) {
                result = QStringLiteral("{\"error\":\"%1\"}").arg(op->errorString());
            } else {
                result = QStringLiteral("{\"saved\":\"%1\"}").arg(QString::fromUtf8(path));
                m_lastSavedPath = QString::fromUtf8(path);
            }
            buf->deleteLater();
            done(result);
        });
    });
}

// The user answered the overwrite prompt.
void NikitaBackend::answerSaveConflict(const QString &action, const QString &newName)
{
    if (!m_pendingSaveDone) { return; }
    auto done = m_pendingSaveDone;
    const QByteArray origPath = m_pendingSavePath;
    const QString content = m_pendingSaveContent;
    m_pendingSaveDone = nullptr;
    m_pendingSavePath.clear();
    m_pendingSaveContent.clear();

    if (action == QLatin1String("cancel")) {
        // Honest report, not a silent no-op: the model must know the file was
        // NOT written so it does not claim success.
        done(QStringLiteral("{\"cancelled\":true,\"note\":\"the user chose not to overwrite the "
                             "existing file -- nothing was saved\"}"));
        return;
    }
    if (action == QLatin1String("rename")) {
        QString name = newName.trimmed();
        if (name.isEmpty()) { done(QStringLiteral("{\"error\":\"no new name given\"}")); return; }
        // Take just the filename the user typed and drop it into the same folder
        // as the original, so a bare "myscript.txt" lands beside it. A .txt is
        // forced for a badusb path, same rule as the save itself.
        name = name.section(QLatin1Char('/'), -1);
        const QString dir = QString::fromUtf8(origPath).section(QLatin1Char('/'), 0, -2);
        QString newPath = dir + QLatin1Char('/') + name;
        if (newPath.startsWith(QLatin1String("/ext/badusb/"), Qt::CaseInsensitive)
            && !newPath.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)) {
            const int dot = newPath.lastIndexOf(QLatin1Char('.'));
            const int slash = newPath.lastIndexOf(QLatin1Char('/'));
            newPath = (dot > slash) ? newPath.left(dot) + QStringLiteral(".txt")
                                    : newPath + QStringLiteral(".txt");
        }
        // Re-run the guard on the NEW name: renaming onto another existing file
        // should ask again, not clobber a second one.
        beginFlipperSave(newPath.toUtf8(), content, done);
        return;
    }
    // "replace" (default): write over the original path.
    writeFlipperFile(origPath, content, done);
}

// Create a folder and all missing ancestors on the Flipper, shallowest first,
// ignoring "already exists" errors. Best-effort: the caller's real write/rename
// still surfaces genuine failures. No-op for the /ext and /int roots.
void NikitaBackend::ensureFlipperDir(const QByteArray &dirPath, std::function<void()> done)
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const QString p = QString::fromUtf8(dirPath);
    if (!dev || (!p.startsWith(QLatin1String("/ext")) && !p.startsWith(QLatin1String("/int")))) {
        done();
        return;
    }
    const QStringList parts = p.split('/', Qt::SkipEmptyParts);   // ext, apps, Scripts
    QStringList dirs;
    QString acc;
    for (const QString &seg : parts) {
        acc += QStringLiteral("/") + seg;
        dirs << acc;                                             // /ext, /ext/apps, ...
    }
    if (!dirs.isEmpty()) { dirs.removeFirst(); }                 // never mkdir the /ext root itself
    if (dirs.isEmpty()) { done(); return; }

    auto step = std::make_shared<std::function<void(int)>>();
    QPointer<Flipper::FlipperZero> devRef(dev);
    *step = [this, devRef, dirs, done, step](int i) {
        Flipper::FlipperZero *dev = devRef.data();
        // The device can be unplugged partway through the chain. Stop, but still
        // call done: every caller is waiting on it, and the write that follows
        // will report the real failure.
        if (!dev || i >= dirs.size()) { done(); return; }
        auto *op = dev->rpc()->storageMkdir(dirs.at(i).toUtf8());
        connect(op, &AbstractOperation::finished, this, [step, i]() { (*step)(i + 1); });
    };
    (*step)(0);
}

// ---- Host agent (edit/build/test the app's own source) --------------------

bool NikitaBackend::agentReady() const
{
    // The workspace folder used to be a requirement because it was also the
    // fence. It isn't a fence any more -- it is just where relative paths and
    // shell commands start from -- so an unset one falls back to home rather
    // than disabling every host tool.
    return m_agentEnabled;
}

// Where a bare relative path lands, and the working directory host_run starts
// in. Never empty: an unconfigured workspace means home, not nowhere.
QString NikitaBackend::agentBaseDir() const
{
    if (!m_agentRoot.isEmpty() && QFileInfo(m_agentRoot).isDir()) { return m_agentRoot; }
    return QDir::homePath();
}

// The agent's current folder: host_cd moves it, relative paths resolve
// against it, host_run starts in it -- without it a model has no sense of
// place and every path must be absolute.
// "Desktop" is a different real path on every machine/locale; asking Qt is
// the only way to be right on a stranger's computer. Empty if not a known name.
static QString nikitaWellKnownDir(const QString &name)
{
    static const QHash<QString, QStandardPaths::StandardLocation> kDirs = {
        {QStringLiteral("desktop"),   QStandardPaths::DesktopLocation},
        {QStringLiteral("downloads"), QStandardPaths::DownloadLocation},
        {QStringLiteral("download"),  QStandardPaths::DownloadLocation},
        {QStringLiteral("documents"), QStandardPaths::DocumentsLocation},
        {QStringLiteral("music"),     QStandardPaths::MusicLocation},
        {QStringLiteral("pictures"),  QStandardPaths::PicturesLocation},
        {QStringLiteral("movies"),    QStandardPaths::MoviesLocation},
        {QStringLiteral("videos"),    QStandardPaths::MoviesLocation},
        {QStringLiteral("home"),      QStandardPaths::HomeLocation},
        {QStringLiteral("temp"),      QStandardPaths::TempLocation},
        // The names a Portuguese-speaking user says out loud, mapped to the same
        // real folders -- the folder on disk is still called Desktop.
        {QStringLiteral("area de trabalho"), QStandardPaths::DesktopLocation},
        {QStringLiteral("\u00e1rea de trabalho"), QStandardPaths::DesktopLocation},
        {QStringLiteral("documentos"), QStandardPaths::DocumentsLocation},
        {QStringLiteral("imagens"),    QStandardPaths::PicturesLocation},
        {QStringLiteral("videos"),     QStandardPaths::MoviesLocation},
        {QStringLiteral("musicas"),    QStandardPaths::MusicLocation},
    };
    const auto it = kDirs.constFind(name.trimmed().toLower());
    if (it == kDirs.constEnd()) { return QString(); }
    const QString p = QStandardPaths::writableLocation(it.value());
    // Qt returns empty for a location the platform doesn't define. Home always
    // exists, so fall back to a folder of that name under it.
    return p.isEmpty() ? QDir::homePath() + QLatin1Char('/') + name : p;
}

QString NikitaBackend::agentCwd() const
{
    if (!m_agentCwd.isEmpty() && QFileInfo(m_agentCwd).isDir()) { return m_agentCwd; }
    return agentBaseDir();
}

// Turn whatever the model said into an absolute path on this computer. No
// workspace-root fence: host_run could always reach anywhere via a shell
// command anyway, so containment here only pushed mistakes onto `sh -c` where
// they're harder to see. "~" expands; relative paths land under the workspace
// (or home).
QString NikitaBackend::resolveAgentPath(const QString &rel, bool mustExist) const
{
    QString cleaned = rel.trimmed();
    if (cleaned.isEmpty() || cleaned == QLatin1String(".")) { return agentCwd(); }

    if (cleaned == QLatin1String("~"))          { cleaned = QDir::homePath(); }
    else if (cleaned.startsWith(QLatin1String("~/"))) { cleaned.replace(0, 1, QDir::homePath()); }

    // A bare well-known folder name, with or without a ~/ in front of it, lands
    // where THIS user's copy of it actually is. Hardcoding "$HOME/Desktop" works
    // on the machine it was written on and quietly creates a junk folder on a
    // localised Linux install or on a machine where Desktop was relocated.
    if (!QDir::isAbsolutePath(cleaned)) {
        const QString head = cleaned.section(QLatin1Char('/'), 0, 0);
        const QString wk = nikitaWellKnownDir(head);
        if (!wk.isEmpty()) {
            const QString rest = cleaned.section(QLatin1Char('/'), 1);
            cleaned = rest.isEmpty() ? wk : wk + QLatin1Char('/') + rest;
        }
    } else {
        // Same for "$HOME/Desktop/x" when Desktop is somewhere else here.
        const QString home = QDir::homePath();
        if (cleaned.startsWith(home + QLatin1Char('/'))) {
            const QString after = cleaned.mid(home.size() + 1);
            const QString head = after.section(QLatin1Char('/'), 0, 0);
            const QString wk = nikitaWellKnownDir(head);
            if (!wk.isEmpty() && wk != home + QLatin1Char('/') + head) {
                const QString rest = after.section(QLatin1Char('/'), 1);
                cleaned = rest.isEmpty() ? wk : wk + QLatin1Char('/') + rest;
            }
        }
    }

    const QString joined = QDir::isAbsolutePath(cleaned)
                               ? QDir::cleanPath(cleaned)
                               : QDir(agentCwd()).absoluteFilePath(cleaned);

    const QFileInfo fi(joined);
    if (fi.exists()) {
        const QString canon = fi.canonicalFilePath();
        return canon.isEmpty() ? joined : canon;   // canonical fails on a broken symlink
    }
    if (mustExist) { return QString(); }
    return joined;
}

// Paths a recursive delete must never target: root, any home folder, and
// anything at depth 0/1 under / (/usr, /etc, /System...). Shared with the CLI
// panel's "rm" -- host_delete is the less-trusted caller (a model picks the
// path, not a person typing) so it gets the same guard, not a weaker one.
static bool cliIsProtectedHostPath(const QString &absPath)
{
    const QString p = QDir::cleanPath(absPath);
    if (p.isEmpty() || p == QLatin1String("/")) { return true; }
    if (p == QDir::homePath()) { return true; }
    if (p == QDir::rootPath()) { return true; }
    const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= 1) { return true; }
    // /Users/<name> and /home/<name> are somebody's home even when not ours.
    if (parts.size() == 2
        && (parts.first() == QLatin1String("Users") || parts.first() == QLatin1String("home"))) {
        return true;
    }
    return false;
}

// ---- macOS protected folders (TCC) ---------------------------------------
//
// Desktop, Documents, Downloads, iCloud Drive and removable volumes are not
// ordinary directories on macOS. They sit behind TCC ("Privacy & Security ->
// Files and Folders"), and this app is signed with a Developer ID AND the
// hardened runtime, which is the combination the system enforces strictly:
// with no usage-description string in Info.plist macOS does not even prompt,
// it denies -- and the refusal arrives as EPERM, "Operation not permitted", on
// a folder whose Unix permissions are entirely normal. A folder the user once
// declined behaves the same way forever afterwards.
//
// That string on its own is what produced "Desktop is read-only. Try
// Documents?": handed a bare errno the model reaches for the only explanation
// it knows, invents a property the folder does not have, and sends the user
// chasing it. Everything below exists so the tool result names the real cause
// and the real fix instead.
//
// The Info.plist keys are the actual repair (application/Info.plist.app); this
// is what happens on the machines where the grant is still missing or was
// refused once.

// Empty unless the path is inside a folder macOS gates. The name it returns is
// the one written on the switch in System Settings, so the instruction we hand
// back matches what the user is looking at.
static QString nikitaProtectedFolderName(const QString &absPath)
{
#ifdef Q_OS_MACOS
    const QString p = QDir::cleanPath(absPath);
    struct Guarded { QStandardPaths::StandardLocation loc; const char *label; };
    static const Guarded kGuarded[] = {
        { QStandardPaths::DesktopLocation,   "Desktop"   },
        { QStandardPaths::DocumentsLocation, "Documents" },
        { QStandardPaths::DownloadLocation,  "Downloads" },
    };
    for (const Guarded &g : kGuarded) {
        const QString root = QDir::cleanPath(QStandardPaths::writableLocation(g.loc));
        if (root.isEmpty()) { continue; }
        if (p == root || p.startsWith(root + QLatin1Char('/'))) {
            return QString::fromLatin1(g.label);
        }
    }
    if (p.startsWith(QLatin1String("/Volumes/")))  { return QStringLiteral("Removable Volumes"); }
    if (p.contains(QLatin1String("/Library/Mobile Documents"))) { return QStringLiteral("iCloud Drive"); }
    return QString();
#else
    Q_UNUSED(absPath);
    return QString();
#endif
}

// Qt hands back the C library's wording, and which of these two strings you get
// depends on the call, the filesystem and the locale of the process -- so match
// on both rather than on one error enum.
static bool nikitaLooksLikePermissionError(const QString &errorText)
{
    const QString e = errorText.toLower();
    return e.contains(QLatin1String("not permitted"))
        || e.contains(QLatin1String("permission denied"))
        || e.contains(QLatin1String("access is denied"));
}

// Can this process actually read the folder's contents? opendir() is the call
// Qt makes underneath entryInfoList(), so asking it directly is the same
// question with the errno still attached -- which is the whole difference
// between "the folder is empty" and "macOS said no".
static QString g_lastDirError;

static bool nikitaCanListDirectory(const QString &absPath)
{
    g_lastDirError.clear();
#ifdef Q_OS_WIN
    Q_UNUSED(absPath);
    return true;
#else
    DIR *d = ::opendir(QFile::encodeName(absPath).constData());
    if (!d) {
        g_lastDirError = QString::fromLocal8Bit(::strerror(errno));
        return false;
    }
    ::closedir(d);
    return true;
#endif
}

static QString nikitaLastDirectoryError()
{
    return g_lastDirError.isEmpty() ? QStringLiteral("couldn't read the folder") : g_lastDirError;
}

static void nikitaAddPermissionHint(QJsonObject &o, const QString &absPath,
                                    const QString &errorText)
{
    const QString folder = nikitaProtectedFolderName(absPath);
    if (!folder.isEmpty() && nikitaLooksLikePermissionError(errorText)) {
        o.insert(QStringLiteral("cause"),
                 QStringLiteral("macOS blocked this app from the %1 folder. This is a privacy "
                                "permission, NOT a property of the folder or the file.").arg(folder));
        o.insert(QStringLiteral("fix"),
                 QStringLiteral("Open System Settings > Privacy & Security > Files and Folders, "
                                "find qFlipper and switch on %1 -- or grant Full Disk Access -- "
                                "then ask again.").arg(folder));
        o.insert(QStringLiteral("tell_the_user"),
                 QStringLiteral("Say exactly this: macOS is blocking access to %1, and it needs to "
                                "be enabled for qFlipper in System Settings. Do NOT say the folder "
                                "is read-only, locked, or full. Do NOT quietly write somewhere else "
                                "and report that as done.").arg(folder));
        // Into the LOGS panel as well. The model's reply is one sentence in a
        // chat window and scrolls away; this is the line the user can still
        // find afterwards when they go looking for why it failed.
        nikitaLog(QStringLiteral(
            "PERMISSION DENIED by macOS: %1 -- qFlipper is not allowed into the %2 folder. "
            "Fix: System Settings > Privacy & Security > Files and Folders > qFlipper > %2. "
            "If the switch is not listed there, the app was refused once and macOS cached it: "
            "run  tccutil reset All com.yourcompany.qFlipper  and relaunch.")
            .arg(absPath, folder));
    }
}

// The one place a failed host file operation turns into a tool result. Built
// with QJsonObject rather than string concatenation because a path can contain
// a quote or a backslash, and a hand-assembled JSON string turns that into a
// parse error the model reads as a crash.
static QString nikitaHostErrorJson(const QString &what, const QString &absPath,
                                   const QString &errorText)
{
    QJsonObject o;
    o.insert(QStringLiteral("error"),
             QStringLiteral("%1 %2: %3").arg(what, absPath, errorText));
    nikitaAddPermissionHint(o, absPath, errorText);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Replace the {{LAST_RESULT}} token with the exact output of the most recent
// command this turn. This is the deterministic half of "collect the result and
// treat it in code, do not leave it to the model": when a script must carry a
// value the model just computed, the model writes the token and the app pastes
// the real bytes in, so a mis-transcribed digit cannot reach the saved file.
// A no-op when the token is absent (the model wrote the value directly) or when
// nothing has run yet, so it never harms an ordinary save.
QString NikitaBackend::substituteRunResult(const QString &content) const
{
    if (!content.contains(QLatin1String("{{LAST_RESULT}}"))
        && !content.contains(QLatin1String("{{RESULT}}"))) {
        return content;
    }
    QString out = content;
    out.replace(QLatin1String("{{LAST_RESULT}}"), m_lastRunOutput);
    out.replace(QLatin1String("{{RESULT}}"), m_lastRunOutput);
    nikitaLogAs(assistantName(),
        QStringLiteral("filled {{LAST_RESULT}} with the command output (%1 chars) -- "
                       "value set by code, not retyped by the model")
            .arg(m_lastRunOutput.size()));
    return out;
}

void NikitaBackend::runHostTool(const QString &name, const QJsonObject &args,
                               std::function<void(const QString &)> done)
{
    if (!agentReady()) {
        done(QStringLiteral("{\"error\":\"Computer tools are off. Turn on Agent mode in setup.\"}"));
        return;
    }
    // One shared refusal, so every tool below reports a missing path the same way.
    auto badPath = [](const QString &p) {
        return QStringLiteral("{\"error\":\"no such path: %1\"}").arg(p);
    };

    if (name == QLatin1String("host_list")) {
        const QString abs = resolveAgentPath(args.value("path").toString(), true);
        if (abs.isEmpty()) { done(badPath(args.value("path").toString())); return; }
        QDir dir(abs);
        if (!dir.exists()) { done(QStringLiteral("{\"error\":\"not a folder\"}")); return; }
        // A folder macOS has gated still stat()s fine -- it is only reading its
        // CONTENTS that is refused, and Qt answers that with an empty list.
        // "Your Desktop is empty" is a confident, wrong answer to a permission
        // problem, so ask the OS directly and let it say no out loud.
        if (!nikitaCanListDirectory(abs)) {
            done(nikitaHostErrorJson(QStringLiteral("can't list"), abs,
                                     nikitaLastDirectoryError()));
            return;
        }
        QJsonArray arr;
        const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                        QDir::DirsFirst | QDir::Name);
        int shown = 0;
        for (const QFileInfo &fi : entries) {
            if (shown++ >= NIKITA_HOST_LIST_CAP) { break; }
            arr.append(QJsonObject{
                {"name", fi.fileName()},
                {"type", fi.isDir() ? "dir" : "file"},
                {"size", static_cast<double>(fi.size())}
            });
        }
        // The absolute path comes back with the listing. A bare array of names
        // is unmoored -- the model asked for "src", and three turns later has no
        // way to tell WHICH src it was looking at.
        const QJsonObject res{{"path", abs}, {"entries", arr}};
        done(QString::fromUtf8(QJsonDocument(res).toJson(QJsonDocument::Compact)));

    } else if (name == QLatin1String("host_read")) {
        const QString abs = resolveAgentPath(args.value("path").toString(), true);
        if (abs.isEmpty()) { done(badPath(args.value("path").toString())); return; }
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            done(nikitaHostErrorJson(QStringLiteral("can't read"), abs, f.errorString()));
            return;
        }
        QByteArray d = f.read(NIKITA_HOST_READ_CAP + 1);
        f.close();
        const bool truncated = d.size() > NIKITA_HOST_READ_CAP;
        if (truncated) { d = d.left(NIKITA_HOST_READ_CAP); }
        QString out = QString::fromUtf8(d);
        if (truncated) { out += QStringLiteral("\n...(truncated)"); }
        if (out.isEmpty()) { out = QStringLiteral("(empty file)"); }
        done(out);

    } else if (name == QLatin1String("host_write")) {
        const QString abs = resolveAgentPath(args.value("path").toString(), false);
        if (abs.isEmpty()) { done(badPath(args.value("path").toString())); return; }
        const QByteArray bytes = substituteRunResult(args.value("content").toString()).toUtf8();
        auto run = [abs, bytes, done]() {
            QDir().mkpath(QFileInfo(abs).absolutePath());
            QFile f(abs);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                // errorString() carries the OS's own wording, including the
                // "Operation not permitted" that means macOS refused the folder.
                done(nikitaHostErrorJson(QStringLiteral("can't write"), abs, f.errorString()));
                return;
            }
            const qint64 n = f.write(bytes);
            const QString writeError = f.errorString();
            f.close();
            if (n < 0) {
                done(nikitaHostErrorJson(QStringLiteral("can't write"), abs, writeError));
                return;
            }
            // Read back what landed. n is what QFile claims it wrote; the file's
            // own size is what is actually on disk. A write that reports success
            // on bytes that never arrived is the same lie as a delete that
            // reports success on a file that was never there.
            const QFileInfo after(abs);
            const bool sizeOk = after.exists() && after.size() == static_cast<qint64>(bytes.size());
            // The absolute path, not a workspace-relative one: with the whole
            // disk in reach, "wrote: config.json" no longer identifies a file.
            done(QStringLiteral("{\"wrote\":\"%1\",\"bytes\":%2,\"verified\":%3}")
                     .arg(abs)
                     .arg(static_cast<double>(after.exists() ? after.size() : 0))
                     .arg(sizeOk ? QStringLiteral("true") : QStringLiteral("false")));
        };
        const bool overwriting = QFileInfo::exists(abs);
        QString summary = QStringLiteral("%1 %2 (%3 bytes)")
                               .arg(overwriting ? QStringLiteral("Overwrite") : QStringLiteral("Create"))
                               .arg(abs)
                               .arg(bytes.size());
        QString preview = QString::fromUtf8(bytes.left(400));
        if (bytes.size() > 400) { preview += QStringLiteral("\n...(truncated)"); }
        requestHostActionConfirm(QStringLiteral("write"), summary, preview, run, done);

    } else if (name == QLatin1String("host_run")) {
        const QString cmd = args.value("command").toString().trimmed();
        if (cmd.isEmpty()) { done(QStringLiteral("{\"error\":\"no command\"}")); return; }
        // An explicit cwd wins; otherwise the workspace, otherwise home.
        const QString cwd = args.contains(QLatin1String("cwd"))
                                ? resolveAgentPath(args.value("cwd").toString(), true)
                                : QString();
        if (hostRunAlwaysAllowed(cmd)) {
            executeHostRun(cmd, cwd, done);
            return;
        }
        // Held until answerHostRunConfirm() fires: a command picked by a
        // small model -- possibly straight out of a prompt-injected file on
        // the SD card -- does not touch this computer without a person
        // seeing the literal command first. See hostRunConfirmRequested.
        m_pendingHostRunCmd = cmd;
        m_pendingHostRunCwd = cwd;
        m_pendingHostRunDone = done;
        emit hostRunConfirmRequested(cmd, cwd.isEmpty() ? agentCwd() : cwd);

    } else if (name == QLatin1String("host_cd")) {
        // Answers with no argument too, which makes it the pwd as well: one
        // tool for "where am I" and "go there", the way cd and pwd are one idea.
        const QString want = args.value("path").toString();
        if (!want.trimmed().isEmpty()) {
            const QString abs = resolveAgentPath(want, true);
            if (abs.isEmpty()) { done(badPath(want)); return; }
            if (!QFileInfo(abs).isDir()) {
                done(QStringLiteral("{\"error\":\"%1 is a file, not a folder\"}").arg(abs));
                return;
            }
            m_agentCwd = abs;
        }
        // Hand back the neighbours as well. Arriving somewhere and immediately
        // needing a second call to see what is there is how a model ends up
        // guessing a filename.
        QJsonArray around;
        const QFileInfoList near = QDir(agentCwd()).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
        int shown = 0;
        for (const QFileInfo &fi : near) {
            if (shown++ >= 60) { break; }
            around.append(fi.isDir() ? fi.fileName() + QLatin1Char('/') : fi.fileName());
        }
        const QJsonObject res{
            {"cwd", agentCwd()},
            {"parent", QFileInfo(agentCwd()).absolutePath()},
            {"home", QDir::homePath()},
            {"workspace", agentBaseDir()},
            {"contains", around}
        };
        done(QString::fromUtf8(QJsonDocument(res).toJson(QJsonDocument::Compact)));

    } else if (name == QLatin1String("host_mkdir")) {
        const QString abs = resolveAgentPath(args.value("path").toString(), false);
        if (abs.isEmpty()) { done(badPath(args.value("path").toString())); return; }
        const bool already = QFileInfo(abs).isDir();
        auto run = [abs, already, done]() {
            // Checked on disk, like the others: mkpath's return value is not
            // proof the folder is there, and "created" answering with a path
            // told the model nothing about whether it had to be made or was
            // already sitting there.
            if (!QDir().mkpath(abs) || !QFileInfo(abs).isDir()) {
                // mkpath gives no reason, so ask the filesystem for one: if the
                // parent is a folder macOS gates, the refusal is a TCC grant
                // and not a broken path.
                const QString why = nikitaProtectedFolderName(abs).isEmpty()
                                        ? QStringLiteral("couldn't create it")
                                        : QStringLiteral("Operation not permitted");
                done(nikitaHostErrorJson(QStringLiteral("can't create folder"), abs, why));
                return;
            }
            done(QStringLiteral("{\"path\":\"%1\",\"created\":%2,\"exists\":true}")
                     .arg(abs, already ? QStringLiteral("false") : QStringLiteral("true")));
        };
        if (already) {
            // Nothing would actually change -- let it through without a prompt
            // so "make sure this folder exists" doesn't nag over a no-op.
            run();
        } else {
            requestHostActionConfirm(QStringLiteral("mkdir"),
                                     QStringLiteral("Create folder %1").arg(abs),
                                     QString(), run, done);
        }

    } else if (name == QLatin1String("host_delete")) {
        // Resolved WITHOUT requiring existence, so a missing file comes back as
        // missing rather than as a bad path. They are different answers: a
        // delete that found nothing is not a delete, and there is no way for the
        // model to say so honestly unless the tool distinguishes them.
        const QString abs = resolveAgentPath(args.value("path").toString(), false);
        if (abs.isEmpty()) { done(badPath(args.value("path").toString())); return; }
        if (!QFileInfo::exists(abs)) {
            done(QStringLiteral("{\"deleted\":false,\"existed\":false,\"path\":\"%1\","
                                "\"note\":\"nothing was there -- tell the user the file did not exist, "
                                "do NOT say it was deleted\"}").arg(abs));
            return;
        }
        // Deleting the root of the disk, a home directory (ours or someone
        // else's -- /home/bob and /Users/bob are just as off-limits as our
        // own), or a top-level system tree is never what was meant, and is
        // the kind of mistake with no undo. Everything else goes.
        const QString canon = QDir::cleanPath(abs);
        if (cliIsProtectedHostPath(canon)) {
            done(QStringLiteral("{\"error\":\"refusing to delete %1 -- name something inside it instead\"}").arg(canon));
            return;
        }
        const bool isDir = QFileInfo(canon).isDir();
        auto run = [canon, isDir, done]() {
            const bool ok = isDir ? QDir(canon).removeRecursively() : QFile::remove(canon);
            // Checked on disk rather than trusted: removeRecursively can answer
            // false having emptied most of a folder, and true on a path still
            // there.
            const bool gone = !QFileInfo::exists(canon);
            if (!ok || !gone) {
                // The deleted/existed pair is the contract the model reads, so
                // it stays exactly as it was; the permission explanation is
                // added alongside it rather than in place of it.
                const QString why = nikitaProtectedFolderName(canon).isEmpty()
                                        ? QStringLiteral("it is still there")
                                        : QStringLiteral("Operation not permitted");
                QJsonObject o{
                    {QStringLiteral("deleted"), false},
                    {QStringLiteral("existed"), true},
                    {QStringLiteral("path"), canon},
                    {QStringLiteral("error"), why}
                };
                nikitaAddPermissionHint(o, canon, why);
                done(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
                return;
            }
            done(QStringLiteral("{\"deleted\":true,\"existed\":true,\"path\":\"%1\"}").arg(canon));
        };
        requestHostActionConfirm(QStringLiteral("delete"),
                                 QStringLiteral("Delete %1 %2")
                                     .arg(isDir ? QStringLiteral("folder") : QStringLiteral("file"), canon),
                                 QString(), run, done);

    } else if (name == QLatin1String("host_move") || name == QLatin1String("host_copy")) {
        const bool moving = (name == QLatin1String("host_move"));
        const QString from = resolveAgentPath(args.value("from").toString(), true);
        const QString to   = resolveAgentPath(args.value("to").toString(), false);
        if (from.isEmpty()) { done(badPath(args.value("from").toString())); return; }
        if (to.isEmpty())   { done(badPath(args.value("to").toString())); return; }
        auto run = [moving, from, to, done]() {
            QDir().mkpath(QFileInfo(to).absolutePath());
            // A destination that already exists would make both calls fail
            // silently in Qt, which reads as "nothing happened" to the model.
            if (QFileInfo::exists(to) && QFileInfo(to).isFile()) { QFile::remove(to); }
            QFile src(from);
            const bool ok = moving ? src.rename(to) : src.copy(to);
            if (!ok) {
                // Reported against the DESTINATION: a copy into a gated folder
                // fails on the side being written to, and pointing at the source
                // sends the user to check a file that is perfectly fine.
                done(nikitaHostErrorJson(
                         QStringLiteral("couldn't %1 %2 ->")
                             .arg(moving ? QStringLiteral("move") : QStringLiteral("copy"), from),
                         to, src.errorString()));
                return;
            }
            done(QStringLiteral("{\"%1\":\"%2\",\"to\":\"%3\"}")
                     .arg(moving ? QStringLiteral("moved") : QStringLiteral("copied"), from, to));
        };
        requestHostActionConfirm(moving ? QStringLiteral("move") : QStringLiteral("copy"),
                                 QStringLiteral("%1 %2 -> %3")
                                     .arg(moving ? QStringLiteral("Move") : QStringLiteral("Copy"), from, to),
                                 QString(), run, done);

    } else if (name == QLatin1String("host_find")) {
        const QString abs = resolveAgentPath(args.value("path").toString(), true);
        if (abs.isEmpty()) { done(badPath(args.value("path").toString())); return; }
        QString pattern = args.value("pattern").toString().trimmed();
        if (pattern.isEmpty()) { pattern = QStringLiteral("*"); }
        QJsonArray arr;
        QDirIterator it(abs, QStringList{pattern},
                        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        int shown = 0;
        while (it.hasNext()) {
            it.next();
            if (shown++ >= NIKITA_HOST_FIND_CAP) { break; }
            arr.append(it.filePath());
        }
        done(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));

    } else {
        done(QStringLiteral("{\"error\":\"unknown host tool '%1'\"}").arg(name));
    }
}

// Always-allow by design: this fork runs Nikita with the on-screen host_run
// confirmation permanently disabled, so every command executes immediately
// with no prompt. That is a deliberate product choice, not an oversight --
// know that any command a model decides to run (including one steered by
// prompt-injected content, e.g. from an SD card) executes on this computer
// unattended.
bool NikitaBackend::hostRunAlwaysAllowed(const QString &cmd) const
{
    Q_UNUSED(cmd);
    return true;
}

void NikitaBackend::rememberHostRunAllowed(const QString &cmd)
{
    QSettings s;
    QStringList allowed = s.value(QStringLiteral("nikita/hostRunAllowed")).toStringList();
    if (!allowed.contains(cmd)) {
        allowed << cmd;
        s.setValue(QStringLiteral("nikita/hostRunAllowed"), allowed);
    }
}

// A person answered the on-screen dialog host_run raised. Nothing here runs
// twice: the pending state is cleared before the command is ever spawned, so
// a stray second click (or a stale click after the turn moved on) is a no-op
// rather than a second execution.
void NikitaBackend::answerHostRunConfirm(bool allow, bool alwaysAllow)
{
    if (!m_pendingHostRunDone) { return; }
    const QString cmd = m_pendingHostRunCmd;
    const QString cwd = m_pendingHostRunCwd;
    const auto done = m_pendingHostRunDone;
    m_pendingHostRunCmd.clear();
    m_pendingHostRunCwd.clear();
    m_pendingHostRunDone = nullptr;

    if (!allow) {
        done(QStringLiteral("{\"error\":\"the user declined to run this command\"}"));
        return;
    }
    if (alwaysAllow) { rememberHostRunAllowed(cmd); }
    executeHostRun(cmd, cwd, done);
}

// Gate shared by host_write/host_mkdir/host_move/host_copy/host_delete. Unlike
// host_run's always-allow list (one exact command string), approval here is
// remembered by KIND: the arguments (paths, file contents) are different on
// every call, so "always allow" can only sensibly mean "always allow this
// VERB", not "always allow this exact call again". `run` has already computed
// everything it needs (paths resolved, bytes read) -- approving just means
// calling it; declining reports back through `done` instead.
void NikitaBackend::requestHostActionConfirm(const QString &kind, const QString &summary,
                                             const QString &detail,
                                             std::function<void()> run,
                                             std::function<void(const QString &)> done)
{
    if (hostActionAlwaysAllowed(kind)) { run(); return; }
    m_pendingHostActionKind = kind;
    m_pendingHostActionRun = run;
    m_pendingHostActionDone = done;
    emit hostActionConfirmRequested(kind, summary, detail);
}

// Always-allow by design -- see hostRunAlwaysAllowed(). Same story for
// host_write/host_mkdir/host_move/host_copy/host_delete: no prompt, ever.
bool NikitaBackend::hostActionAlwaysAllowed(const QString &kind) const
{
    Q_UNUSED(kind);
    return true;
}

void NikitaBackend::rememberHostActionAllowed(const QString &kind)
{
    QSettings s;
    QStringList allowed = s.value(QStringLiteral("nikita/hostActionAllowed")).toStringList();
    if (!allowed.contains(kind)) {
        allowed << kind;
        s.setValue(QStringLiteral("nikita/hostActionAllowed"), allowed);
    }
}

// Mirrors answerHostRunConfirm(): pending state is cleared before `run` is
// ever called, so a stray or stale click from a dialog the turn has already
// moved past is a no-op rather than a second execution.
void NikitaBackend::answerHostActionConfirm(bool allow, bool alwaysAllow)
{
    if (!m_pendingHostActionRun) { return; }
    const QString kind = m_pendingHostActionKind;
    const auto run = m_pendingHostActionRun;
    const auto done = m_pendingHostActionDone;
    m_pendingHostActionKind.clear();
    m_pendingHostActionRun = nullptr;
    m_pendingHostActionDone = nullptr;

    if (!allow) {
        if (done) { done(QStringLiteral("{\"error\":\"the user declined this action\"}")); }
        return;
    }
    if (alwaysAllow) { rememberHostActionAllowed(kind); }
    run();
}

// Spawns the command and reports through `done` on exit or watchdog kill.
// Signal-driven, never waitForStarted()/waitForFinished() -- those block the
// GUI thread for up to NIKITA_HOST_RUN_TIMEOUT_MS with no way to cancel.
void NikitaBackend::executeHostRun(const QString &cmd, const QString &cwd,
                                   std::function<void(const QString &)> done)
{
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(cwd.isEmpty() ? agentCwd() : cwd);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    auto *guard = new QTimer(this);
    guard->setSingleShot(true);
    guard->setInterval(NIKITA_HOST_RUN_TIMEOUT_MS);

    // A long install or build can run for a minute-plus with
    // nothing to show for it until this tool call resolves, which is
    // indistinguishable on screen from being stuck (see the model-install
    // LOGS fix above). Read incrementally and log complete lines as they
    // arrive so the LOGS panel shows it's actually working, instead of
    // waiting for readAll() at the very end.
    nikitaLog(QStringLiteral("host_run: %1").arg(cmd));
    auto outBuf = std::make_shared<QByteArray>();
    auto lineBuf = std::make_shared<QByteArray>();
    auto lastLogged = std::make_shared<QString>();
    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, outBuf, lineBuf, lastLogged]() {
        const QByteArray chunk = proc->readAllStandardOutput();
        *outBuf += chunk;
        *lineBuf += chunk;
        lineBuf->replace('\r', '\n');
        int idx;
        while ((idx = lineBuf->indexOf('\n')) >= 0) {
            const QByteArray raw = lineBuf->left(idx);
            lineBuf->remove(0, idx + 1);
            const QString line = sanitizeStatusLine(QString::fromUtf8(raw));
            // Same-line redraws (a spinner, a repeating progress line) would
            // otherwise log once per redraw; skip an exact repeat of the
            // last thing shown.
            if (!line.isEmpty() && line != *lastLogged) {
                *lastLogged = line;
                nikitaLog(QStringLiteral("host_run: %1").arg(line));
            }
        }
    });

    // Shared rather than two captures: the watchdog and the finished handler
    // both need to know whether the kill was ours, and they fire at
    // different times.
    auto killed = std::make_shared<bool>(false);
    QPointer<QProcess> procRef(proc);
    connect(guard, &QTimer::timeout, this, [procRef, killed]() {
        // QPointer: the process may already have finished and been queued
        // for deletion, and dereferencing it then is a crash, not a missed
        // kill.
        if (procRef && procRef->state() != QProcess::NotRunning) {
            *killed = true;
            procRef->kill();
        }
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, guard, killed, done, outBuf](int code, QProcess::ExitStatus) {
        guard->stop();
        guard->deleteLater();
        if (*killed) {
            done(QStringLiteral("{\"error\":\"command timed out after %1s\"}")
                     .arg(NIKITA_HOST_RUN_TIMEOUT_MS / 1000));
            proc->deleteLater();
            return;
        }
        // Whatever arrived after the last readyReadStandardOutput is still
        // sitting in the process, unread; outBuf has everything before that.
        *outBuf += proc->readAllStandardOutput();
        QString out = QString::fromLocal8Bit(*outBuf);
        const bool truncated = out.size() > NIKITA_HOST_OUTPUT_CAP;
        if (truncated) { out = out.left(NIKITA_HOST_OUTPUT_CAP) + QStringLiteral("\n...(truncated)"); }
        // Remember the result so a later save can use {{LAST_RESULT}} and get
        // the exact bytes the command printed, not the model's retype of them.
        // Kept only for a single clean scalar-ish result -- a multi-kilobyte or
        // errored output is not something to paste into a script.
        if (code == 0) {
            const QString trimmed = out.trimmed();
            m_lastRunOutput = (trimmed.size() <= 4096) ? trimmed : QString();
        }
        const QJsonObject res{{"exit_code", code}, {"output", out}};
        done(QString::fromUtf8(QJsonDocument(res).toJson(QJsonDocument::Compact)));
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this,
            [proc, guard, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) { return; }   // finished() still fires for the rest
        guard->stop();
        guard->deleteLater();
        done(QStringLiteral("{\"error\":\"couldn't start command\"}"));
        proc->deleteLater();
    });

#if defined(Q_OS_WIN)
    proc->start(QStringLiteral("cmd"), {QStringLiteral("/c"), cmd});
#else
    proc->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
#endif
    // Nothing here can type at the program. Closing stdin makes anything
    // that reads it (sort, cat with no file, an accidental interactive
    // prompt) see EOF and exit at once, instead of hanging until the
    // watchdog kills it 15 minutes later.
    proc->closeWriteChannel();
    guard->start();
}

bool NikitaBackend::agentEnabled() const { return m_agentEnabled; }
QString NikitaBackend::agentDir() const  { return m_agentRoot; }

void NikitaBackend::setAgentEnabled(bool on)
{
    if (on == m_agentEnabled) { return; }
    m_agentEnabled = on;
    QSettings().setValue(QStringLiteral("nikita/agentEnabled"), on);
    emit agentChanged();
}

void NikitaBackend::setAgentDir(const QString &dir)
{
    m_agentCwd.clear();   // a new workspace means starting from its root again

    // Accept a plain path or a file:// URL (QML FolderDialog hands back a URL).
    QString path = dir;
    if (path.startsWith(QLatin1String("file://"))) { path = QUrl(path).toLocalFile(); }
    if (path == m_agentRoot) { return; }
    m_agentRoot = path;
    QSettings().setValue(QStringLiteral("nikita/agentDir"), m_agentRoot);
    emit agentChanged();
}

// Replace the fact list wholesale and report what moved -- used both when the
// user saves memory.txt and when the card's copy is re-read. m_memory is only
// a cache; the user can edit the file by hand, so this re-reads before
// rewriting rather than appending to a possibly-stale copy. Doesn't route
// through rememberFact() -- its quality gates are for model-produced text and
// would drop a short fact the user deliberately typed verbatim.
void NikitaBackend::applyMemoryText(const QString &text, const QString &source)
{
    const QStringList before = nikitaFactList(m_memory);
    m_memory = text.trimmed();
    const QStringList after = nikitaFactList(m_memory);
    writeMemoryCache();

    QStringList added, removed;
    for (const QString &f : after)  { if (!nikitaHasFact(before, f)) { added << f; } }
    for (const QString &f : before) { if (!nikitaHasFact(after, f))  { removed << f; } }

    if (added.isEmpty() && removed.isEmpty()) { return; }

    nikitaLog(QStringLiteral("%1: +%2 remembered, -%3 forgotten")
             .arg(source).arg(added.size()).arg(removed.size()));
    for (const QString &f : added)   { nikitaLog(QStringLiteral("  remembered: %1").arg(f)); }
    for (const QString &f : removed) { nikitaLog(QStringLiteral("  forgot: %1").arg(f)); }

    // The system prompt is rebuilt every turn, so the fact list itself is
    // already current. What is not current is the conversation: the last ~14
    // messages travel with the request, and a small model weights those far
    // more heavily than a block buried in a long system prompt -- so it keeps
    // reciting a fact that was just deleted. Say it plainly, in the history,
    // where it will actually be read.
    QString note = QStringLiteral("MEMORY UPDATED (%1). ").arg(source);
    if (!removed.isEmpty()) {
        note += QStringLiteral("These are NO LONGER TRUE and must never be repeated or listed again: ")
                + removed.join(QStringLiteral("; ")) + QStringLiteral(". ");
    }
    if (!added.isEmpty()) {
        note += QStringLiteral("These are now true: ") + added.join(QStringLiteral("; ")) + QStringLiteral(". ");
    }
    note += QStringLiteral("Anything you said earlier that conflicts with the current memory list is out of date. "
                           "That list is the complete and only set of durable facts about the user.");
    m_history.append(QJsonObject{{"role", "system"}, {"content", note}});
}

// Firmware-owned files excluded from backup: restoring them onto a different
// firmware gives apps built against the wrong ABI and stale update bundles,
// and the firmware itself is a store click away.
// A very large file crashes the Flipper over RPC (a 3GB image took it down
// with "furi_check failed"), so anything above this cap is reported by name
// rather than attempted.
static const qint64 kMaxBackupFileBytes = 64LL * 1024 * 1024;

// Read by FlipperCli::setOpen(). A plain flag rather than walking the object
// tree: NikitaBackend is not a child of ApplicationBackend, so findChild() would
// have returned null and the guard would have done nothing at all.
static bool g_transferRunning = false;

static bool nikitaSkipInBackup(const QString &name)
{
    static const QStringList skip = {
        QStringLiteral("update"),          // firmware bundles waiting to be applied
        QStringLiteral("Manifest"),        // firmware resource manifest
        QStringLiteral("apps"),            // FAPs, compiled against one firmware ABI
        QStringLiteral("apps_assets"),
        QStringLiteral("apps_manifests"),
        QStringLiteral(".int"),            // internal storage mirror
    };
    if (skip.contains(name, Qt::CaseInsensitive)) { return true; }
    // macOS and Windows litter removable media with these.
    return name.startsWith(QLatin1String(".Spotlight"))
        || name.startsWith(QLatin1String(".Trash"))
        || name.startsWith(QLatin1String(".fseventsd"))
        || name.startsWith(QLatin1String(".tmp"))
        || name == QLatin1String("System Volume Information");
}

void NikitaBackend::setTransfer(const QString &title, const QString &note, double frac)
{
    m_transferActive = true;
    m_transferTitle = title;
    m_transferNote = note;
    m_transferProgress = frac;
    emit transferChanged();
}

void NikitaBackend::finishTransfer(bool ok, const QString &text)
{
    m_transferActive = false;
    m_transferNote.clear();
    m_transferProgress = -1.0;
    nikitaLog(QStringLiteral("transfer: %1 -- %2")
             .arg(ok ? QStringLiteral("finished") : QStringLiteral("failed"), text));
    emit transferChanged();

    // Hand the outcome to the backend so it lands on the very same Finished /
    // ErrorOccured screens a firmware install uses. Reusing the real state is
    // what removed the parallel set of visibility conditions this used to need
    // -- and with it the risk of the two drifting apart.
    if (m_appBackend) {
        m_appBackend->reportExternalResult(ok, int(BackendError::OperationError));
    }
}

void NikitaBackend::endTransfer()
{
    if (!m_transferActive) { return; }
    m_transferActive = false;
    m_transferNote.clear();
    m_transferProgress = -1.0;
    emit transferChanged();
}

void NikitaBackend::backupSdCard(const QUrl &target)
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) { emit backupFailed(QStringLiteral("No Flipper connected.")); return; }
    if (m_transferActive) {
        emit backupFailed(QStringLiteral("An operation is already running."));
        return;
    }
    // A run that died between phases used to leave these populated forever,
    // and every later attempt was refused as "already running".
    m_backupDirs.clear();
    m_backupPending.clear();

    if (!target.isValid() || target.toLocalFile().isEmpty()) {
        emit backupFailed(QStringLiteral("No destination chosen."));
        return;
    }
    m_backupArchive = target.toLocalFile();

    // The staging folder lives in the system temp directory, not next to the
    // archive: files have to land somewhere before they can be packed, but that
    // is plumbing. Only the archive belongs where the user pointed.
    m_backupRoot = QDir::tempPath() + QStringLiteral("/nikita-backup");


    // A leftover working folder from an interrupted run would get packed along
    // with the new one.
    QDir(m_backupRoot).removeRecursively();
    if (!QDir().mkpath(m_backupRoot)) {
        emit backupFailed(QStringLiteral("Couldn't create %1").arg(m_backupRoot));
        return;
    }

    m_backupDirs = QStringList{ QStringLiteral("/ext") };
    m_backupPending.clear();
    m_backupTotal = 0;
    m_backupFiles = 0;
    m_backupBytes = 0;
    m_backupSkipped = 0;   // counted during the walk as well as the copy

    g_transferRunning = true;
    nikitaLog(QStringLiteral("backup: walking /ext into %1").arg(m_backupRoot));
    setTransfer(QStringLiteral("Backing Up"), QStringLiteral("Reading the card…"), -1.0);
    emit backupProgress(QStringLiteral("Reading the card…"), 0.0);
    backupEnumerateNext();
}

// Depth-first walk of the card, one storageList at a time. Directories found
// go back on the queue; files are collected for the copy pass that follows.
void NikitaBackend::backupEnumerateNext()
{
    if (m_backupDirs.isEmpty()) {
        m_backupTotal = m_backupPending.size();
        qint64 want = 0;
        for (const auto &e : m_backupPending) { want += e.second; }
        nikitaLog(QStringLiteral("backup: %1 file(s) found, %2 KB to copy%3")
                 .arg(m_backupTotal).arg(want / 1024)
                 .arg(m_backupSkipped ? QStringLiteral(", %1 too large to read")
                                        .arg(m_backupSkipped) : QString()));
        if (m_backupTotal == 0) {
            // Nothing to pack. Say so rather than reporting a success that
            // leaves an empty folder behind and no explanation.
            QDir(m_backupRoot).removeRecursively();
            g_transferRunning = false;
            finishTransfer(false, QStringLiteral("Nothing was found on the card to back up."));
            emit backupFailed(QStringLiteral("Nothing was found on the card to back up."));
            return;
        }
        backupCopyNext();
        return;
    }

    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (!dev) {
        m_backupDirs.clear(); m_backupPending.clear();
        g_transferRunning = false;
        finishTransfer(false, QStringLiteral("The Flipper went away mid-backup."));
        emit backupFailed(QStringLiteral("The Flipper went away mid-backup."));
        return;
    }

    const QString dir = m_backupDirs.takeFirst();
    auto *op = dev->rpc()->storageList(dir.toUtf8());

    connect(op, &AbstractOperation::finished, this, [this, op, dir]() {
        if (op->isError()) {
            nikitaLog(QStringLiteral("backup: can't list %1 -- %2").arg(dir, op->errorString()));
            backupEnumerateNext();      // one unreadable folder must not stop the walk
            return;
        }

        for (const FileInfo &f : op->files()) {
            const QString name = QString::fromUtf8(f.name);
            const QString full = dir + QLatin1Char('/') + name;
            // The skip list is about the top level only: "apps" under /ext is
            // firmware territory, but a folder called "apps" nested inside
            // someone's own directory is their data.
            if (dir == QLatin1String("/ext") && nikitaSkipInBackup(name)) { continue; }

            if (f.type == FileType::Directory) { m_backupDirs.append(full); }
            else if (qint64(f.size) > kMaxBackupFileBytes) {
                ++m_backupSkipped;
                nikitaLog(QStringLiteral("backup: %1 SKIPPED -- %2 MB is past the %3 MB limit "
                                        "(reading it over RPC crashes the Flipper)")
                         .arg(full).arg(qint64(f.size) / 1024 / 1024)
                         .arg(kMaxBackupFileBytes / 1024 / 1024));
            }
            // The size comes free with the listing and decides the read
            // deadline below -- without it a large file dies on the generic 30s.
            else { m_backupPending.append(qMakePair(full, qint64(f.size))); }
        }
        backupEnumerateNext();
    });
}

void NikitaBackend::backupCopyNext()
{
    if (m_backupPending.isEmpty()) {
        nikitaLog(QStringLiteral("backup: copied %1 file(s), %2 KB, %3 skipped -- packing")
                 .arg(m_backupFiles).arg(m_backupBytes / 1024).arg(m_backupSkipped));
        backupPackArchive();
        return;
    }

    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (!dev) {
        m_backupPending.clear();
        g_transferRunning = false;
        finishTransfer(false, QStringLiteral("The Flipper went away mid-backup."));
        emit backupFailed(QStringLiteral("The Flipper went away mid-backup."));
        return;
    }

    const auto entry = m_backupPending.takeFirst();
    const QString remote = entry.first;
    const qint64 size = entry.second;
    const QString rel = remote.mid(QStringLiteral("/ext").size());   // keeps the leading slash
    const QString local = m_backupRoot + rel;

    // The folder has to exist before the operation opens the file in it.
    QDir().mkpath(QFileInfo(local).absolutePath());

    const double frac = m_backupTotal > 0
                        ? double(m_backupTotal - m_backupPending.size() - 1) / double(m_backupTotal)
                        : 0.0;
    setTransfer(QStringLiteral("Backing Up"), rel, frac);
    emit backupProgress(rel, frac);

    // Handed over unopened: storageRead opens it itself, and pre-opening made
    // every file land on disk at zero bytes.
    auto *file = new QFile(local, this);
    auto *op = dev->rpc()->storageRead(remote.toUtf8(), file);

    // Reads run at a few hundred KB/s over RPC, so a mass-storage image or a
    // big capture needs far more than the generic 30s. Budget 20 KB/s -- well
    // under the real rate, so slow is still survivable -- with a 30s floor.
    // Plain arithmetic rather than qBound: mixing int literals with a qint64
    // expression made the overload ambiguous, and the lower bound is redundant
    // anyway -- the sum already starts at 30s.
    const qint64 budget = qint64(30000) + size / 20;
    op->setTimeout(int(budget < 3600000 ? budget : 3600000));

    connect(op, &AbstractOperation::finished, this, [this, op, file, remote, local]() {
        if (op->isError()) {
            // Skipped, not fatal: one unreadable file must not cost the other
            // 1464. The count is reported at the end so nothing goes unnoticed.
            ++m_backupSkipped;
            nikitaLog(QStringLiteral("backup: %1 SKIPPED -- %2").arg(remote, op->errorString()));
            QFile::remove(local);   // don't leave a truncated stub in the archive
        } else {
            const qint64 sz = QFileInfo(local).size();
            ++m_backupFiles;
            m_backupBytes += sz;
            if (m_backupFiles <= 5 || (m_backupFiles % 50) == 0) {
                nikitaLog(QStringLiteral("backup: %1 -> %2 bytes").arg(remote).arg(sz));
            }
        }
        file->deleteLater();
        backupCopyNext();
    });
}

// Writes one 512-byte ustar header. The format is simple enough to emit
// directly, which is why this no longer shells out to /usr/bin/tar: on macOS
// the Desktop is TCC-protected and a spawned child does not inherit the app's
// permission for it, so the archive step failed for a reason that had nothing
// to do with the data.
static QByteArray nikitaTarHeader(const QString &path, qint64 size, bool isDir)
{
    QByteArray h(512, '\0');

    QByteArray name = path.toUtf8();
    if (isDir && !name.endsWith('/')) { name.append('/'); }
    if (name.size() > 100) { return QByteArray(); }   // caller skips these
    memcpy(h.data(), name.constData(), size_t(name.size()));

    auto octal = [&h](int off, int len, qint64 v) {
        const QByteArray s = QByteArray::number(v, 8).rightJustified(len - 1, '0');
        memcpy(h.data() + off, s.constData(), size_t(len - 1));
    };

    octal(100, 8, isDir ? 0755 : 0644);               // mode
    octal(108, 8, 0);                                 // uid
    octal(116, 8, 0);                                 // gid
    octal(124, 12, isDir ? 0 : size);                 // size
    octal(136, 12, QDateTime::currentSecsSinceEpoch());
    h[156] = isDir ? '5' : '0';                       // typeflag
    memcpy(h.data() + 257, "ustar\0" "00", 8);         // magic + version

    // Checksum is computed with the checksum field itself read as spaces.
    memset(h.data() + 148, ' ', 8);
    unsigned sum = 0;
    for (int k = 0; k < 512; ++k) { sum += unsigned(uchar(h.at(k))); }
    const QByteArray cs = QByteArray::number(sum, 8).rightJustified(6, '0');
    memcpy(h.data() + 148, cs.constData(), 6);
    h[154] = '\0';
    h[155] = ' ';
    return h;
}

// The mirror of nikitaTarHeader: walks the gzipped tar and writes it back out.
// In-process for the same reason the writer is -- a spawned tar cannot reach a
// TCC-protected folder on the app's behalf.
static bool nikitaUntar(const QString &archive, const QString &destDir, QString *error)
{
    gzFile gz = gzopen(QFile::encodeName(archive).constData(), "rb");
    if (!gz) { *error = QStringLiteral("Couldn't open the archive."); return false; }

    QByteArray hdr(512, '\0');
    while (true) {
        const int got = gzread(gz, hdr.data(), 512);
        if (got == 0) { break; }                       // clean end of stream
        if (got != 512) { *error = QStringLiteral("The archive is truncated."); gzclose(gz); return false; }
        if (hdr.at(0) == '\0') { break; }               // the empty end-of-archive block

        const QString name = QString::fromUtf8(hdr.constData());   // NUL-terminated
        const qint64 size = QByteArray(hdr.constData() + 124, 11).trimmed().toLongLong(nullptr, 8);
        const char type = hdr.at(156);

        // Never let an archive write outside the destination.
        const QString clean = QDir::cleanPath(name);
        if (clean.startsWith(QLatin1String("..")) || clean.startsWith(QLatin1Char('/'))) {
            *error = QStringLiteral("The archive contains an unsafe path: %1").arg(name);
            gzclose(gz);
            return false;
        }
        const QString out = destDir + QLatin1Char('/') + clean;

        if (type == '5') { QDir().mkpath(out); continue; }

        QDir().mkpath(QFileInfo(out).absolutePath());
        QFile f(out);
        if (!f.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("Couldn't write %1").arg(out);
            gzclose(gz);
            return false;
        }
        qint64 left = size;
        QByteArray buf(64 * 1024, '\0');
        while (left > 0) {
            const int want = int(qMin<qint64>(left, buf.size()));
            const int r = gzread(gz, buf.data(), unsigned(want));
            if (r <= 0) { *error = QStringLiteral("The archive ended early."); f.close(); gzclose(gz); return false; }
            f.write(buf.constData(), r);
            left -= r;
        }
        f.close();

        // Entries are padded to a 512-byte boundary; step over it.
        const int pad = int((512 - (size % 512)) % 512);
        if (pad) { gzread(gz, buf.data(), unsigned(pad)); }
    }

    gzclose(gz);
    return true;
}

bool nikitaWriteTgz(const QString &srcDir, const QString &archive,
                   int *packedOut, int *skippedOut, QString *error);

// The archive writer, deliberately free-standing: NikitaBackend::backupPackArchive()
// and the CLI's "tgz" command both call it, so what the command exercises is
// exactly what a backup runs -- no second implementation to drift.
bool nikitaWriteTgz(const QString &srcDir, const QString &archive,
                   int *packedOut, int *skippedOut, QString *error)
{
    if (!QDir(srcDir).exists()) { *error = QStringLiteral("no such folder: %1").arg(srcDir); return false; }

    QFile::remove(archive);
    gzFile gz = gzopen(QFile::encodeName(archive).constData(), "wb");
    if (!gz) { *error = QStringLiteral("couldn't open %1 for writing").arg(archive); return false; }

    auto put = [gz](const QByteArray &b) {
        return b.isEmpty() || gzwrite(gz, b.constData(), unsigned(b.size())) == b.size();
    };

    int packed = 0, skipped = 0;
    bool ok = true;

    // QDir::Hidden matters here: the card is full of dotfiles
    // (.badusb.settings, .eink.settings, .nested.log) which are copied but
    // would otherwise be left out of the archive.
    QDirIterator it(srcDir, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && ok) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString rel = QDir(srcDir).relativeFilePath(fi.absoluteFilePath());

        const QByteArray hdr = nikitaTarHeader(rel, fi.size(), fi.isDir());
        if (hdr.isEmpty()) { ++skipped; continue; }     // path past ustar's 100 chars
        if (!put(hdr)) { ok = false; break; }
        if (fi.isDir()) { ++packed; continue; }

        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) { ++skipped; continue; }
        qint64 written = 0;
        while (!f.atEnd()) {
            const QByteArray chunk = f.read(64 * 1024);
            if (chunk.isEmpty() || !put(chunk)) { ok = false; break; }
            written += chunk.size();
        }
        f.close();
        if (!ok) { break; }

        const int pad = int((512 - (written % 512)) % 512);
        if (pad && !put(QByteArray(pad, '\0'))) { ok = false; break; }
        ++packed;
    }

    if (ok) { ok = put(QByteArray(1024, '\0')); }       // two empty blocks end it
    const int gzErr = gzclose(gz);
    if (gzErr != Z_OK) { ok = false; *error = QStringLiteral("gzclose returned %1").arg(gzErr); }

    if (packedOut)  { *packedOut = packed; }
    if (skippedOut) { *skippedOut = skipped; }

    if (!ok) {
        if (error->isEmpty()) { *error = QStringLiteral("write failed"); }
        QFile::remove(archive);
        return false;
    }
    if (QFileInfo(archive).size() <= 0) { *error = QStringLiteral("the archive came out empty"); return false; }
    return true;
}

void NikitaBackend::backupPackArchive()
{
    setTransfer(QStringLiteral("Backing Up"), QStringLiteral("Packing bkp.tgz…"), -1.0);
    emit backupProgress(QStringLiteral("Packing bkp.tgz…"), 1.0);
    nikitaLog(QStringLiteral("backup: packing %1 -> %2").arg(m_backupRoot, m_backupArchive));

    int packed = 0, skipped = 0;
    QString err;
    if (!nikitaWriteTgz(m_backupRoot, m_backupArchive, &packed, &skipped, &err)) {
        // The copied files stay put: they are the user's data, and a packing
        // failure is no reason to throw them away.
        g_transferRunning = false;
        finishTransfer(false, QStringLiteral("Couldn't write bkp.tgz (%1).").arg(err));
        nikitaLog(QStringLiteral("backup: packing FAILED -- %1").arg(err));
        // The staging folder is out of sight, so name it: the copied files are
        // still there and still the user's data.
        emit backupFailed(QStringLiteral("Couldn't write bkp.tgz (%1). The copied files are still in %2")
                          .arg(err, m_backupRoot));
        return;
    }

    const qint64 bytes = QFileInfo(m_backupArchive).size();
    nikitaLog(QStringLiteral("backup: archived %1 entr(y/ies)%2 -> %3 KB")
             .arg(packed)
             .arg(skipped ? QStringLiteral(", %1 skipped").arg(skipped) : QString())
             .arg(bytes / 1024));

    g_transferRunning = false;
    finishTransfer(true, QStringLiteral("%1 file(s) saved to %2")
                   .arg(m_backupFiles).arg(m_backupArchive));
    QDir(m_backupRoot).removeRecursively();
    nikitaLog(QStringLiteral("backup: done -- %1 file(s)%2 in %3 (%4 KB)")
             .arg(m_backupFiles)
             .arg(m_backupSkipped ? QStringLiteral(", %1 skipped").arg(m_backupSkipped) : QString())
             .arg(m_backupArchive).arg(bytes / 1024));
    emit backupFinished(m_backupArchive, m_backupFiles);
}

void NikitaBackend::rebootDevice()
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) { return; }
    dev->rpc()->rebootToOS();
}

void NikitaBackend::formatSdCard()
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) { finishTransfer(false, QStringLiteral("No Flipper connected."));
 emit formatFailed(QStringLiteral("No Flipper connected.")); return; }
    // m_transferActive is set synchronously by setTransfer() below, so it is
    // already true by the time a second click can arrive. The old guard tested
    // m_formatQueue, which stays empty until the storageList reply lands --
    // two quick presses started two listings, and the two queues then chewed
    // through the same entries at once. That is what took the app down.
    if (m_transferActive) { emit formatFailed(QStringLiteral("An operation is already running.")); return; }

    g_transferRunning = true;
    nikitaLog(QStringLiteral("format: listing /ext"));
    setTransfer(QStringLiteral("Formatting"), QStringLiteral("Reading the card…"), -1.0);
    emit formatProgress(QStringLiteral("Reading the card…"), 0.0);

    auto *op = dev->rpc()->storageList(QByteArrayLiteral("/ext"));
    connect(op, &AbstractOperation::finished, this, [this, op]() {
        if (op->isError()) {
            nikitaLog(QStringLiteral("format: FAILED to list /ext -- %1").arg(op->errorString()));
            finishTransfer(false, op->errorString());
            emit formatFailed(op->errorString());
            return;
        }
        m_formatQueue.clear();
        for (const FileInfo &f : op->files()) {
            m_formatQueue.append(QString::fromUtf8(f.name));
        }
        m_formatTotal = m_formatQueue.size();
        nikitaLog(QStringLiteral("format: removing %1 entries from /ext").arg(m_formatTotal));

        if (m_formatQueue.isEmpty()) {
            m_sdFormatted = true;
            emit sdFormattedChanged();
            finishTransfer(true, QStringLiteral("The card is empty."));
            if (Flipper::FlipperZero *d = m_appBackend ? m_appBackend->device() : nullptr) {
                auto info = d->deviceState()->deviceInfo();
                info.storage.isAssetsInstalled = false;
                d->deviceState()->setDeviceInfo(info);
            }
            emit formatFinished();
            return;
        }
        runFormatQueue();
    });
}

void NikitaBackend::runFormatQueue()
{
    if (m_formatQueue.isEmpty()) {
        // The card is empty. Everything the firmware needs on it -- resources,
        // Manifest, apps -- is gone, so a plain reinstall has nothing to repair;
        // a full install is what puts the card back together.
        m_sdFormatted = true;
        emit sdFormattedChanged();
        nikitaLog(QStringLiteral("format: /ext is empty"));
        finishTransfer(true, QStringLiteral("The card is empty."));
        // The card just lost its assets, but deviceInfo still holds what was read
        // on connect. Without this the main button says "up to date" until the
        // app is restarted.
        if (Flipper::FlipperZero *d = m_appBackend ? m_appBackend->device() : nullptr) {
            // The card was just emptied, so the assets are gone by definition.
            // Marking it locally beats asking the device: no round trip to race
            // with whatever the user does next, and the UI updates immediately.
            auto info = d->deviceState()->deviceInfo();
            info.storage.isAssetsInstalled = false;
            d->deviceState()->setDeviceInfo(info);
        }
        emit formatFinished();
        return;
    }

    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    if (!dev) {
        m_formatQueue.clear();
        finishTransfer(false, QStringLiteral("The Flipper went away mid-format."));
        emit formatFailed(QStringLiteral("The Flipper went away mid-format."));
        return;
    }

    const QString name = m_formatQueue.takeFirst();
    const double frac = m_formatTotal > 0
                        ? double(m_formatTotal - m_formatQueue.size() - 1) / double(m_formatTotal)
                        : 0.0;
    setTransfer(QStringLiteral("Formatting"), name, frac);
    emit formatProgress(name, frac);

    auto *op = dev->rpc()->storageRemove((QStringLiteral("/ext/") + name).toUtf8(), true);
    connect(op, &AbstractOperation::finished, this, [this, op, name]() {
        if (op->isError()) {
            nikitaLog(QStringLiteral("format: %1 FAILED -- %2").arg(name, op->errorString()));
        } else {
            nikitaLog(QStringLiteral("format: %1 removed").arg(name));
        }
        runFormatQueue();     // one stubborn entry must not stop the rest
    });
}

void NikitaBackend::restoreSdCard(const QString &folderUrl)
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) { finishTransfer(false, QStringLiteral("No Flipper connected."));
 emit restoreFailed(QStringLiteral("No Flipper connected.")); return; }
    if (m_transferActive) {
        emit restoreFailed(QStringLiteral("An operation is already running."));
        return;
    }

    const QString picked = QUrl(folderUrl).isLocalFile() ? QUrl(folderUrl).toLocalFile() : folderUrl;

    // Backups are archives now, but a folder from an older run still works.
    QString local = picked;
    if (QFileInfo(picked).isFile()) {
        const QString tmp = QDir::homePath() + QStringLiteral("/Desktop/Nikita-qflipper/.restore-working");
        QDir(tmp).removeRecursively();
        if (!QDir().mkpath(tmp)) {
            finishTransfer(false, QStringLiteral("Couldn't create %1").arg(tmp));
            emit restoreFailed(QStringLiteral("Couldn't create %1").arg(tmp));
            return;
        }

        nikitaLog(QStringLiteral("restore: unpacking %1").arg(picked));
        QString err;
        if (!nikitaUntar(picked, tmp, &err)) {
            nikitaLog(QStringLiteral("restore: unpacking FAILED -- %1").arg(err));
            finishTransfer(false, QStringLiteral("Couldn't unpack the archive: %1").arg(err));
            emit restoreFailed(QStringLiteral("Couldn't unpack %1: %2")
                               .arg(QFileInfo(picked).fileName(), err));
            return;
        }
        local = tmp;
    }

    QDir dir(local);
    if (!dir.exists()) {
        finishTransfer(false, QStringLiteral("No such folder: %1").arg(local));
        emit restoreFailed(QStringLiteral("No such folder: %1").arg(local));
        return;
    }

    // One operation for the whole set: uploadFiles() walks directories itself,
    // so handing it the top-level entries restores the tree in a single pass
    // instead of a queue that could half-finish.
    QList<QUrl> urls;
    int skipped = 0;
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &fi : entries) {
        // The same exclusions as the backup, in case the folder was hand-edited
        // or came from somewhere else -- restoring apps or an update bundle
        // onto a different firmware is how a working Flipper stops working.
        if (nikitaSkipInBackup(fi.fileName())) { ++skipped; continue; }
        urls.append(QUrl::fromLocalFile(fi.absoluteFilePath()));
    }

    if (urls.isEmpty()) {
        finishTransfer(false, QStringLiteral("Nothing to restore in %1").arg(local));
        emit restoreFailed(QStringLiteral("Nothing to restore in %1").arg(local));
        return;
    }

    g_transferRunning = true;
    nikitaLog(QStringLiteral("restore: sending %1 entries from %2 to /ext (%3 skipped)")
             .arg(urls.size()).arg(local).arg(skipped));
    setTransfer(QStringLiteral("Restoring"), QStringLiteral("Uploading %1 item(s)…").arg(urls.size()), -1.0);
    emit restoreProgress(QStringLiteral("Uploading %1 item(s)…").arg(urls.size()), 0.0);

    auto *op = dev->utility()->uploadFiles(urls, QByteArrayLiteral("/ext"));
    const int count = urls.size();

    connect(op, &AbstractOperation::progressChanged, this, [this, op, count]() {
        setTransfer(QStringLiteral("Restoring"), QStringLiteral("Uploading %1 item(s)…").arg(count),
                    op->progress() / 100.0);
        emit restoreProgress(QStringLiteral("Uploading %1 item(s)…").arg(count), op->progress() / 100.0);
    });

    connect(op, &AbstractOperation::finished, this, [this, op, count]() {
        if (op->isError()) {
            nikitaLog(QStringLiteral("restore: FAILED -- %1").arg(op->errorString()));
            finishTransfer(false, op->errorString());
            emit restoreFailed(op->errorString());
            return;
        }
        nikitaLog(QStringLiteral("restore: %1 entries written to /ext").arg(count));
        finishTransfer(true, QStringLiteral("Restore complete."));
        emit restoreFinished(count);
    });
}

void NikitaBackend::reloadMemory()
{
    // Disabled means the assistant does not touch the card. Without this the
    // startup mirror wrote memory.txt straight back after an erase -- an empty
    // file, so no data survived, but /ext/nikita reappeared on the card and the
    // erase looked like it had failed. It also settles the wider promise: off
    // has to mean no reads and no writes, not just a hidden panel.
    if (!m_assistantEnabled) { return; }
    loadPortableMemory();
}

void NikitaBackend::refreshMemoryFromDisk()
{
    // Disabled means the assistant does not touch the card. Without this the
    // startup mirror wrote memory.txt straight back after an erase -- an empty
    // file, so no data survived, but /ext/nikita reappeared on the card and the
    // erase looked like it had failed. It also settles the wider promise: off
    // has to mean no reads and no writes, not just a hidden panel.
    if (!m_assistantEnabled) { return; }
    // First, and outside the guard below. These are two different files, and
    // putting this after the early return meant a missing or unreadable
    // memory.txt silently skipped loading the proven moves as well --
    // leaving m_skills empty. The next sync then wrote that empty list over the
    // card, which is why actions-memory.txt kept being wiped.
    loadProvenMoves();

    QFile f(nikitaMemoryPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { return; }
    m_memory = QString::fromUtf8(f.readAll()).trimmed();
    f.close();
}

void NikitaBackend::writeMemoryCache() const
{
    QFile f(nikitaMemoryPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write(m_memory.toUtf8());
        f.close();
    }
}

// A manual save of memory.txt is authoritative: adopt it as-is and do NOT push
// anything back, or the copy we were holding would overwrite what was just
// written.
bool NikitaBackend::adoptMemoryIfMemoryFile(const QString &path, const QString &content)
{
    if (QDir::cleanPath(path).compare(QLatin1String("/ext/nikita/memory.txt"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    applyMemoryText(content, QStringLiteral("memory.txt edited by hand"));
    return true;
}

// The same contract for actions-memory.txt. What the user leaves in the file IS
// the proven-moves list -- edits stick, and deleting everything really clears
// it. Before this, m_skills was the source of truth and the card only a mirror
// it wrote TO, so a hand-erase on the card was overwritten by the stale list on
// the very next turn: "I deleted it and it came back."
bool NikitaBackend::adoptSkillsIfSkillsFile(const QString &path, const QString &content)
{
    if (QDir::cleanPath(path).compare(QLatin1String("/ext/nikita/actions-memory.txt"),
                                      Qt::CaseInsensitive) != 0) {
        return false;
    }
    m_skills = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    while (m_skills.size() > 24) { m_skills.removeFirst(); }

    // Persist the local copy directly, allowing empty -- saveProvenMoves()
    // refuses to truncate to nothing (a guard against an unloaded list wiping a
    // real one), but here empty is a deliberate choice the user just made.
    QFile lf(nikitaSkillsPath());
    if (lf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        lf.write(m_skills.join(QLatin1Char('\n')).toUtf8());
        lf.close();
    }
    // Mark this exact content as already synced so the card-mirror sees no
    // change and does not write the list back over what the user just saved.
    m_syncedSkills = m_skills.join(QLatin1Char('\n'));
    nikitaLog(QStringLiteral("actions-memory.txt edited by hand: %1 proven move(s)")
                  .arg(m_skills.size()));
    return true;
}

void NikitaBackend::rememberFact(const QString &fact)
{
    refreshMemoryFromDisk();
    QString clean = fact.trimmed();
    // Strip a leading bullet the model sometimes includes.
    while (clean.startsWith(QLatin1String("- ")) || clean.startsWith(QLatin1String("* "))) {
        clean = clean.mid(2).trimmed();
    }
    if (clean.isEmpty()) { return; }

    // --- Quality gate: reject junk so memory stays trustworthy, not a dump. ---
    // 1. Too short or too long to be a real, useful fact.
    if (clean.size() < 6 || clean.size() > 200) { return; }
    // 2. Must contain letters (not just symbols/numbers/emoji).
    if (!clean.contains(QRegularExpression(QStringLiteral("[A-Za-zÀ-ÿ]")))) { return; }
    // 3. Reject obvious conversational filler the model might try to store as a "fact".
    static const QRegularExpression filler(
        QStringLiteral("^(ok|okay|sure|yes|no|thanks|hello|hi|hey|done|"
                       "got it|hmm+|lol|kk+|test(ing)?)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (filler.match(clean).hasMatch()) { return; }
    // 4. A fact is a statement, not a question or a command back to the user.
    if (clean.endsWith(QLatin1Char('?'))) { return; }

    // De-dupe (case-insensitive), and replace a near-identical prior fact instead
    // of stacking a second copy.
    QStringList lines = m_memory.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        QString l = line.trimmed();
        while (l.startsWith(QLatin1String("- "))) { l = l.mid(2).trimmed(); }
        if (l.compare(clean, Qt::CaseInsensitive) == 0) { return; }   // exact dupe
    }

    // No cap. It used to keep only the most recent 40 facts, which meant the
    // assistant quietly FORGOT the oldest thing it knew about a person every
    // time it learned a new one -- the opposite of what a memory is for. Facts
    // are one short line each; a thousand of them is a few tens of kilobytes.
    //
    // What this does cost is prompt weight: every fact is sent on every turn.
    // If that ever becomes the bottleneck, the fix is selecting which facts are
    // relevant to the current message -- not throwing the oldest ones away.
    lines << (QStringLiteral("- ") + clean);
    m_memory = lines.join(QLatin1Char('\n'));

    // Persist locally (authoritative, always available).
    QFile mf(nikitaMemoryPath());
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        mf.write(m_memory.toUtf8());
        mf.close();
    }

    // Best-effort mirror onto the SD so memory travels with the Flipper.
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (ready) {
        const QByteArray memPath = "/ext/nikita/memory.txt";
        const QString memBody = m_memory;
        // ensureFlipperDir is async: the device can go away before this runs.
        QPointer<Flipper::FlipperZero> devRef(dev);
        ensureFlipperDir("/ext/nikita", [this, devRef, memPath, memBody]() {
            Flipper::FlipperZero *dev = devRef.data();
            if (!dev) { return; }
            QBuffer *buf = new QBuffer(this);
            buf->setData(memBody.toUtf8());
            buf->open(QIODevice::ReadOnly);
            auto *op = dev->rpc()->storageWrite(memPath, buf);
            connect(op, &AbstractOperation::finished, this, [buf]() { buf->deleteLater(); });
        });
    }
}

// Remove facts from memory: those containing `match`, or ALL if match is "all"
// (or empty). Returns how many were removed. Persists locally + mirrors to SD.
int NikitaBackend::forgetFacts(const QString &match)
{
    refreshMemoryFromDisk();
    if (m_memory.trimmed().isEmpty()) { return 0; }
    QStringList lines = m_memory.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const int before = lines.size();

    const QString m = match.trimmed();
    if (m.isEmpty() || m.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) {
        lines.clear();
    } else {
        QStringList kept;
        for (const QString &line : lines) {
            if (!line.contains(m, Qt::CaseInsensitive)) { kept << line; }
        }
        lines = kept;
    }
    const int removed = before - lines.size();
    if (removed == 0) { return 0; }

    m_memory = lines.join(QLatin1Char('\n'));

    // Persist locally.
    QFile mf(nikitaMemoryPath());
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        mf.write(m_memory.toUtf8());
        mf.close();
    }
    // Mirror to SD (writes the whole current memory, even if now empty).
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (ready) {
        const QByteArray memPath = "/ext/nikita/memory.txt";
        const QString memBody = m_memory;
        // ensureFlipperDir is async: the device can go away before this runs.
        QPointer<Flipper::FlipperZero> devRef(dev);
        ensureFlipperDir("/ext/nikita", [this, devRef, memPath, memBody]() {
            Flipper::FlipperZero *dev = devRef.data();
            if (!dev) { return; }
            QBuffer *buf = new QBuffer(this);
            buf->setData(memBody.toUtf8());
            buf->open(QIODevice::ReadOnly);
            auto *op = dev->rpc()->storageWrite(memPath, buf);
            connect(op, &AbstractOperation::finished, this, [buf]() { buf->deleteLater(); });
        });
    }
    return removed;
}

// Pull the script out of a chat message: the first fenced ``` code block if
// present, otherwise the whole text. Used by the manual "save to Flipper" panel.
QString NikitaBackend::extractScript(const QString &text) const
{
    const int a = text.indexOf(QStringLiteral("```"));
    if (a < 0) { return text.trimmed(); }
    const int nl = text.indexOf(QLatin1Char('\n'), a);
    if (nl < 0) { return text.trimmed(); }
    const int b = text.indexOf(QStringLiteral("```"), nl + 1);
    if (b < 0) { return text.mid(nl + 1).trimmed(); }
    return text.mid(nl + 1, b - nl - 1).trimmed();
}

// Manual, deterministic save: the USER picks the folder + filename and we write
// straight to the SD -- the model is never involved, so it can't fumble it. This
// is the reliable path: the 3b is great at drafting a script, bad at saving it,
// so we take the saving out of its hands entirely.
void NikitaBackend::saveScriptToFlipper(const QString &folder, const QString &filename, const QString &content)
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) { emit scriptSaveError(QStringLiteral("No Flipper connected or ready.")); return; }

    // Normalise the folder into an /ext/<folder> path.
    QString fld = folder.trimmed();
    while (fld.startsWith(QLatin1Char('/'))) { fld = fld.mid(1); }
    if (fld.startsWith(QLatin1String("ext/"))) { fld = fld.mid(4); }
    if (fld.isEmpty()) { fld = QStringLiteral("badusb"); }

    // Sanitise the filename (no path separators or illegal chars).
    QString fn = filename.trimmed();
    fn.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (fn.isEmpty()) { fn = QStringLiteral("script.txt"); }

    QString path = QStringLiteral("/ext/") + fld + QLatin1Char('/') + fn;
    QString body = content;

    // BadUSB must be .txt and gets the DuckyScript cleaner.
    if (path.startsWith(QLatin1String("/ext/badusb/"), Qt::CaseInsensitive)) {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const int dot = path.lastIndexOf(QLatin1Char('.'));
        if (dot > slash) { path = path.left(dot) + QStringLiteral(".txt"); }
        else            { path += QStringLiteral(".txt"); }
        body = sanitizeDuckyScript(body);
    }

    const QByteArray p = path.toUtf8();
    const QString finalBody = body;
    const QByteArray parent = path.section('/', 0, -2).toUtf8();
    QPointer<Flipper::FlipperZero> devRef(dev);
    ensureFlipperDir(parent, [this, devRef, p, finalBody, path]() {
        Flipper::FlipperZero *dev = devRef.data();
        if (!dev) { return; }
        QBuffer *buf = new QBuffer(this);
        buf->setData(finalBody.toUtf8());
        buf->open(QIODevice::ReadOnly);
        auto *op = dev->rpc()->storageWrite(p, buf);
        connect(op, &AbstractOperation::finished, this, [this, op, buf, path]() {
            if (op->isError()) {
                nikitaLog(QStringLiteral("save FAILED %1: %2").arg(path, op->errorString()));
                emit scriptSaveError(op->errorString());
            } else {
                nikitaLog(QStringLiteral("saved %1").arg(path));
                emit scriptSaved(path);
            }
            buf->deleteLater();
        });
    });
}

// ---- In-app file editor (read/write any Flipper text file: .txt/.nfc/.ir/.sub) ----
// Read a file off the Flipper and hand its text to the QML editor.
void NikitaBackend::openFileForEdit(const QString &path)
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) { emit fileEditError(QStringLiteral("No Flipper connected.")); return; }
    if (path.isEmpty()) { emit fileEditError(QStringLiteral("No path.")); return; }

    const QByteArray p = path.toUtf8();
    QBuffer *buf = new QBuffer(this);
    buf->open(QIODevice::ReadWrite);
    auto *op = dev->rpc()->storageRead(p, buf);
    connect(op, &AbstractOperation::finished, this, [this, op, buf, path]() {
        if (op->isError()) {
            emit fileEditError(op->errorString());
        } else {
            emit fileOpened(path, QString::fromUtf8(buf->data()));
        }
        buf->deleteLater();
    });
}

// Write edited text straight back to the Flipper at the exact path (no extension
// forcing -- the editor keeps the file's real name/type).
void NikitaBackend::writeFile(const QString &path, const QString &content)
{
    Flipper::FlipperZero *dev = m_appBackend ? m_appBackend->device() : nullptr;
    const bool ready = m_appBackend && dev &&
                       m_appBackend->backendState() == ApplicationBackend::BackendState::Ready;
    if (!ready) { emit fileEditError(QStringLiteral("No Flipper connected.")); return; }
    if (path.isEmpty()) { emit fileEditError(QStringLiteral("No path.")); return; }

    const QByteArray p = path.toUtf8();
    const QByteArray body = content.toUtf8();
    QBuffer *buf = new QBuffer(this);
    buf->setData(body);
    buf->open(QIODevice::ReadOnly);
    auto *op = dev->rpc()->storageWrite(p, buf);
    connect(op, &AbstractOperation::finished, this, [this, op, buf, path, content]() {
        if (op->isError()) {
            nikitaLog(QStringLiteral("write %1 -- FAILED: %2").arg(path, op->errorString()));
            emit fileEditError(op->errorString());
        } else {
            nikitaLog(QStringLiteral("write %1 -- done").arg(path));
            adoptMemoryIfMemoryFile(path, content);
            adoptSkillsIfSkillsFile(path, content);
            emit fileSaved(path);
        }
        buf->deleteLater();
    });
}



// ============================ FirmwareStore ============================

// Short display label for a channel id.
static QString fwChannelLabel(const QString &id)
{
    if (id == QLatin1String("development"))       { return QStringLiteral("dev"); }
    if (id == QLatin1String("release-candidate"))  { return QStringLiteral("rc"); }
    return id;   // "release", "dev"
}

FirmwareStore::FirmwareStore(QObject *parent)
    : QObject(parent)
{
    m_net.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    const QString rel = QStringLiteral("release");
    const QStringList git = { QStringLiteral("release"), QStringLiteral("dev") };

    // fields: name, kind, locator, blurb, channels, wantChannel, latest, tgzUrl, status, raw
    m_sources = {
        { QStringLiteral("Official"),    Kind::DirJson,
          QStringLiteral("https://update.flipperzero.one/firmware/directory.json"),
          QStringLiteral("The original Flipper Devices firmware."),      {},  rel, {}, {}, {}, {}, {} },
        { QStringLiteral("Momentum"),    Kind::DirJson,
          QStringLiteral("https://up.momentum-fw.dev/firmware/directory.json"),
          QStringLiteral("A feature rich community firmware."),     {},  rel, {}, {}, {}, {}, {} },
        { QStringLiteral("Unleashed"),   Kind::GitHub,
          QStringLiteral("DarkFlippers/unleashed-firmware"),
          QStringLiteral("A popular community firmware with expanded features."), git, rel, {}, {}, {}, {}, {} },
        { QStringLiteral("RogueMaster"), Kind::GitHub,
          QStringLiteral("RogueMaster/flipperzero-firmware-wPlugins"),
          QStringLiteral("A feature packed community firmware."),   git, rel, {}, {}, {}, {}, {} },
        // ARF ships only dev-tagged releases, so give it a single "dev" channel.
        { QStringLiteral("ARF"),         Kind::GitHub,
          QStringLiteral("D4C1-Labs/Flipper-ARF"),
          QStringLiteral("A firmware for automotive and Sub GHz research."),
          { QStringLiteral("dev") }, QStringLiteral("dev"), {}, {}, {}, {}, {} },
        // Xero publishes versioned releases (flipper-z-f7-update-local.tgz).
        { QStringLiteral("Xero"),        Kind::GitHub,
          QStringLiteral("noproto/xero-firmware"),
          QStringLiteral("A lightweight firmware based on the official one."),
          { QStringLiteral("release") }, QStringLiteral("release"), {}, {}, {}, {}, {} },
    };

    // Restore each firmware's remembered channel choice.
    QSettings st;
    for (Source &s : m_sources) {
        const QString saved = st.value(QStringLiteral("firmware/ch/") + s.name).toString();
        if (!saved.isEmpty()) { s.wantChannel = saved; }
    }
    // Start from what was learned last run, so the UI is correct before -- and
    // regardless of -- what the network says this time.
    for (int i = 0; i < m_sources.size(); ++i) { loadDerived(i); }
}

void FirmwareStore::setOpen(bool value)
{
    if (value == m_open) { return; }
    m_open = value;
    emit openChanged();
    if (m_open) { refreshIfStale(); }   // freshen only if the cache aged out
}

void FirmwareStore::setBusy(bool value)
{
    if (value == m_busy) { return; }
    m_busy = value;
    emit busyChanged();
}

void FirmwareStore::setDeviceVersion(const QString &v)
{
    if (m_deviceVersion == v) { return; }
    m_deviceVersion = v;
    emit changed();

    // The main screen needs to know whether an update exists before the user
    // ever opens the store panel, so the first time a device reports in, go
    // fetch. Without this the button would have nothing to compare against.
    if (!m_deviceVersion.trimmed().isEmpty()) {
        const int i = installedIndex();
        nikitaLog(QStringLiteral("firmware: device reports %1 -> %2")
                 .arg(m_deviceVersion.trimmed(),
                      i >= 0 ? m_sources.at(i).name : QStringLiteral("no matching source")));
    }

    if (!m_deviceVersion.trimmed().isEmpty() && !m_fetchedOnce) {
        m_fetchedOnce = true;
        refreshIfStale();
    }
}

void FirmwareStore::setDeviceCommit(const QString &c)
{
    if (m_deviceCommit == c) { return; }
    m_deviceCommit = c;
    emit changed();
}

void FirmwareStore::setDeviceChannel(const QString &c)
{
    if (m_deviceChannel == c) { return; }
    m_deviceChannel = c;
    // Follow the device. Opening on a channel the Flipper is not running offers
    // an install nobody asked for, and the honest starting point is whatever is
    // actually on it. Changing the picker by hand still works; it just does not
    // survive the next connect.
    if (!c.trimmed().isEmpty()) { setInstalledChannel(c.trimmed()); }
    emit changed();
}

void FirmwareStore::setDeviceDate(const QDate &d)
{
    if (m_deviceDate == d) { return; }
    m_deviceDate = d;
    emit changed();
}

// True when the source offers something built after what is running. Going back
// to an earlier build is a perfectly good thing to want, but it is an install,
// not an update, and the button should say so.
bool FirmwareStore::sourceIsNewer() const
{
    if (!m_deviceDate.isValid()) { return true; }
    const QDate srcDate = QDate::fromString(installedDate(), Qt::ISODate);
    if (!srcDate.isValid()) { return true; }
    return srcDate > m_deviceDate;
}

// Counts a lookup in and, on the last one, works out what to tell the user.
// A source only counts as an update if it is a build the device is not already
// running -- otherwise "updates found" would fire for every source in the list.
void FirmwareStore::noteLookupDone(int index)
{
    if (m_pending <= 0) { return; }
    --m_pending;
    if (m_pending > 0) { return; }

    int reachable = 0;
    for (const Source &src : m_sources) {
        if (src.status == QLatin1String("ready")) { ++reachable; }
    }

    // An "update" means a newer build of the firmware THIS Flipper is running.
    // Counting every source whose version differs was meaningless: with six
    // firmwares listed, five always differ, so it reported "5 new releases
    // found" while there was nothing to update -- they were other people's
    // firmwares, not this one's.
    m_foundUpdates = updateAvailable() ? 1 : 0;

    if (reachable == 0) {
        m_checkSummary = QStringLiteral("Couldn't reach any source");
    } else if (m_foundUpdates > 0) {
        // Naming the build is the point: the same version then shows up in the
        // store and the main screen offers the update, so the message and the
        // rest of the app agree.
        m_checkSummary = QStringLiteral("%1 %2 available")
                         .arg(installedName(), installedLatest());
    } else if (!installedReady()) {
        m_checkSummary = QStringLiteral("Couldn't check your firmware");
    } else {
        // Nothing newer than what is installed. Say it plainly -- a count of
        // other firmwares' releases read as "it found things and did nothing".
        m_checkSummary = QStringLiteral("Everything is up to date");
    }
    nikitaLog(QStringLiteral("firmware: %1").arg(m_checkSummary));
    Q_UNUSED(index)
}

QStringList FirmwareStore::installedChannels() const
{
    const int i = installedIndex();
    if (i < 0) { return QStringList(); }
    const Source &s = m_sources.at(i);
    // Same rule as the store panel: no picker when every channel resolves to
    // the same build.
    if (distinctChannelCount(s) <= 1) { return QStringList{ currentChannelId(s) }; }
    return s.channels;
}

QVariantList FirmwareStore::installedChannelModel() const
{
    QVariantList out;
    for (const QString &id : installedChannels()) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), id);
        out.append(m);
    }
    return out;
}

QString FirmwareStore::installedChannel() const
{
    const int i = installedIndex();
    return (i >= 0) ? currentChannelId(m_sources.at(i)) : QString();
}

void FirmwareStore::setInstalledChannel(const QString &id)
{
    const int i = installedIndex();
    if (i < 0 || !m_sources.at(i).channels.contains(id)) { return; }
    if (m_sources.at(i).wantChannel == id) { return; }
    m_sources[i].wantChannel = id;
    QSettings().setValue(QStringLiteral("firmware/ch/") + m_sources.at(i).name, id);
    if (!m_sources.at(i).raw.isEmpty() || loadDerived(i)) { deriveFromCache(i); }
    else                                                  { fetchOne(i); }

    // Worth stating outright: for a GitHub source "dev" is the newest release
    // and "release" is the newest non-prerelease. When the newest release is
    // not flagged as a prerelease those are the same object, so switching
    // channel legitimately lands on the same build and the main button stays
    // on "Up to date". This line is what makes that visible instead of looking
    // like the switch did nothing.
    nikitaLog(QStringLiteral("firmware: %1 channel -> %2, resolves to %3")
             .arg(m_sources.at(i).name, id,
                  m_sources.at(i).latest.isEmpty() ? QStringLiteral("nothing yet")
                                                   : m_sources.at(i).latest));
    emit changed();
}

// Reinstall means "the build already on the device, from the source it came
// from" -- not the official channel, which is what the stock action would do
// and which on a fork would silently replace it with a different firmware.
// The device only reports a version string, never which channel it came from.
// Without remembering it, "you are on release, dev is available" is unanswerable
// -- so the channel is recorded whenever this app is the one doing the flashing.
void FirmwareStore::rememberFlashedChannel(int index)
{
    if (index < 0 || index >= m_sources.size()) { return; }
    const Source &s = m_sources.at(index);
    QSettings().setValue(QStringLiteral("firmware/flashedCh/") + s.name, currentChannelId(s));
    QSettings().setValue(QStringLiteral("firmware/flashedVer/") + s.name, s.latest);
}

QString FirmwareStore::installedFromChannel() const
{
    const int i = installedIndex();
    if (i < 0) { return QString(); }
    QSettings st;
    const QString ver = st.value(QStringLiteral("firmware/flashedVer/") + m_sources.at(i).name).toString();
    // Only trust the record if it describes the build actually running; the
    // user may have flashed from somewhere else since.
    if (ver.isEmpty() || ver.compare(m_deviceVersion.trimmed(), Qt::CaseInsensitive) != 0) { return QString(); }
    return st.value(QStringLiteral("firmware/flashedCh/") + m_sources.at(i).name).toString();
}

bool FirmwareStore::channelSwitchPending() const
{
    const QString from = installedFromChannel();
    if (from.isEmpty() || !installedReady()) { return false; }
    return from.compare(installedChannel(), Qt::CaseInsensitive) != 0;
}

void FirmwareStore::reinstallInstalled()
{
    const int i = installedIndex();
    if (i < 0 || !installedReady()) { return; }
    install(i);
}

// The download URL points at an asset inside a release; the release page is the
// same URL with /download/<tag>/<file> swapped for /tag/<tag>. That page is where
// a fork publishes its changelog, and it is the only one this app can honestly
// show for a firmware it does not host.
QString FirmwareStore::installedReleaseUrl() const
{
    const int i = installedIndex();
    if (i < 0) { return QString(); }
    const QString u = m_sources.at(i).tgzUrl;
    const int cut = u.indexOf(QLatin1String("/releases/download/"));
    if (cut < 0) {
        // Momentum publishes on its own site rather than GitHub Releases, and
        // its per-version page is /releases/<version>: the same idea as the tag
        // page the other forks get.
        const QString loc = m_sources.at(i).locator;
        const QString ver = m_sources.at(i).latest;
        if (loc.contains(QLatin1String("momentum-fw.dev")) && !ver.isEmpty()) {
            return QStringLiteral("https://momentum-fw.dev/releases/") + ver;
        }
        return QString();
    }
    const QString tail = u.mid(cut + 19);
    const int slash = tail.indexOf(QLatin1Char('/'));
    if (slash < 0) { return QString(); }
    return u.left(cut) + QStringLiteral("/releases/tag/") + tail.left(slash);
}

QString FirmwareStore::installedDate() const
{
    const int i = installedIndex();
    return (i >= 0) ? m_sources.at(i).date : QString();
}

QString FirmwareStore::selectedName() const
{
    return hasSelection() ? m_sources.at(m_selected).name : QString();
}

QString FirmwareStore::selectedVersion() const
{
    return hasSelection() ? m_sources.at(m_selected).latest : QString();
}

QString FirmwareStore::selectedDate() const
{
    return hasSelection() ? m_sources.at(m_selected).date : QString();
}

void FirmwareStore::select(int index)
{
    if (index < 0 || index >= m_sources.size()) { return; }
    // Picking the staged row again unstages it, so there is a way back without
    // hunting for a separate cancel control.
    m_selected = (m_selected == index) ? -1 : index;
    emit changed();
}

void FirmwareStore::clearSelection()
{
    if (m_selected < 0) { return; }
    m_selected = -1;
    // Importing from another channel moves the picker there, and clearing the
    // pick without moving it back left the main button offering that channel's
    // build as an install. Only Momentum publishes two channels, so this was
    // invisible on every other row.
    const QString on = m_deviceChannel.trimmed();
    nikitaLog(QStringLiteral("DIAG clear devCh=\"%1\" curCh=\"%2\" latest=\"%3\"")
             .arg(m_deviceChannel, installedChannel(), installedLatest()));
    if (!on.isEmpty()) { setInstalledChannel(on); }
    emit changed();
}

void FirmwareStore::installSelected()
{
    if (!hasSelection()) { return; }
    install(m_selected);
}

QString FirmwareStore::installedName() const
{
    const int i = installedIndex();
    return (i >= 0) ? m_sources.at(i).name : QString();
}

QString FirmwareStore::installedLatest() const
{
    const int i = installedIndex();
    return (i >= 0) ? m_sources.at(i).latest : QString();
}

bool FirmwareStore::installedReady() const
{
    const int i = installedIndex();
    return (i >= 0) && m_sources.at(i).status == QLatin1String("ready")
           && !m_sources.at(i).tgzUrl.isEmpty();
}

// Same equality-not-ordering reasoning as in sources(): the store only ever
// lists the newest build, so "differs from what is running" is what can be
// claimed honestly.
bool FirmwareStore::updateAvailable() const
{
    if (!installedReady()) { return false; }
    const QString want = installedLatest();
    // Every fork names a rolling build differently: Momentum publishes the bare
    // commit hash, RogueMaster wraps it as RM<date>-<hash>, and the device calls
    // itself something else again ("mntm-dev", "rm-420"). What they all share is
    // that the running commit appears somewhere inside the offered name, so that
    // is what can be checked. Seven characters is enough not to match by accident.
    const QString commit = m_deviceCommit.trimmed();
    if (commit.length() >= 7 && want.contains(commit, Qt::CaseInsensitive)) {
        return channelSwitchPending();
    }
    // A different build, or the same build from a different channel: both are
    // something to offer. The second case only fires when this app flashed the
    // current build and therefore knows which channel it came from.
    if (want.compare(m_deviceVersion.trimmed(), Qt::CaseInsensitive) != 0) { return true; }
    return channelSwitchPending();
}

// Each firmware stamps its version with a recognisable shape, so the running
// build can be traced back to the source it came from without asking the device
// anything extra. Unknown shapes fall through to Official, which is where a
// plain "1.4.3" or a bare commit hash comes from.
int FirmwareStore::installedIndex() const
{
    const QString v = m_deviceVersion.trimmed();
    if (v.isEmpty()) { return -1; }

    QString want;
    if (v.startsWith(QLatin1String("mntm"), Qt::CaseInsensitive))        { want = QStringLiteral("Momentum"); }
    else if (v.startsWith(QLatin1String("unlshd"), Qt::CaseInsensitive)) { want = QStringLiteral("Unleashed"); }
    // The feed publishes "RM0722-..." but the device answers "rm-420", so this
    // has to be case-insensitive or a RogueMaster Flipper is read as Official.
    else if (v.startsWith(QLatin1String("RM"), Qt::CaseInsensitive))     { want = QStringLiteral("RogueMaster"); }
    else if (v.contains(QLatin1String("arf"), Qt::CaseInsensitive))      { want = QStringLiteral("ARF"); }
    // Xero versions look like "1.4.2-xero.2". Note the device reports
    // firmware.origin.fork as "Official" for it, so the version string is the
    // only thing that tells them apart.
    else if (v.contains(QLatin1String("xero"), Qt::CaseInsensitive))     { want = QStringLiteral("Xero"); }
    else                                                                 { want = QStringLiteral("Official"); }

    for (int i = 0; i < m_sources.size(); ++i) {
        if (m_sources.at(i).name.compare(want, Qt::CaseInsensitive) == 0) { return i; }
    }
    return -1;
}

// What a given channel of this source would resolve to, without touching the
// source's own state. Mirrors the picking rules in deriveFromCache().
QString FirmwareStore::channelVersion(const Source &s, const QString &ch) const
{
    if (s.raw.isEmpty()) { return QString(); }

    if (s.kind == Kind::DirJson) {
        const QJsonArray channels = QJsonDocument::fromJson(s.raw).object()
                                    .value(QStringLiteral("channels")).toArray();
        for (const QJsonValue &cv : channels) {
            const QJsonObject c = cv.toObject();
            if (c.value(QStringLiteral("id")).toString() != ch) { continue; }
            const QJsonArray versions = c.value(QStringLiteral("versions")).toArray();
            if (versions.isEmpty()) { return QString(); }
            return versions.first().toObject().value(QStringLiteral("version")).toString();
        }
        return QString();
    }

    const QJsonArray rels = QJsonDocument::fromJson(s.raw).array();
    if (rels.isEmpty()) { return QString(); }
    if (ch == QLatin1String("dev")) {
        for (const QJsonValue &rv : rels) {
            const QJsonObject r = rv.toObject();
            if (r.value(QStringLiteral("prerelease")).toBool()) {
                return r.value(QStringLiteral("tag_name")).toString();
            }
        }
        return rels.first().toObject().value(QStringLiteral("tag_name")).toString();
    }
    for (const QJsonValue &rv : rels) {
        const QJsonObject r = rv.toObject();
        if (!r.value(QStringLiteral("prerelease")).toBool()) {
            return r.value(QStringLiteral("tag_name")).toString();
        }
    }
    return rels.first().toObject().value(QStringLiteral("tag_name")).toString();
}

// A channel picker is only worth showing when the channels actually lead
// somewhere different. Unleashed and RogueMaster list release and dev but
// publish no prereleases, so both land on the same build -- a dropdown there
// invites a choice that changes nothing. Decided from the data rather than a
// hand-kept list, so it follows whatever those repos do next.
int FirmwareStore::distinctChannelCount(const Source &s) const
{
    if (s.channels.size() <= 1) { return s.channels.size(); }
    // Nothing fetched this session: keep the picker so a switch can go fetch.
    if (s.raw.isEmpty()) { return s.channels.size(); }

    QStringList seen;
    for (const QString &ch : s.channels) {
        const QString v = channelVersion(s, ch);
        if (!v.isEmpty() && !seen.contains(v, Qt::CaseInsensitive)) { seen << v; }
    }
    return (seen.size() > 1) ? s.channels.size() : 1;
}

QVariantList FirmwareStore::sources() const
{
    const int running = installedIndex();
    QVariantList out;
    for (int i = 0; i < m_sources.size(); ++i) {
        const Source &s = m_sources.at(i);
        QVariantMap m;
        m.insert(QStringLiteral("name"), s.name);
        m.insert(QStringLiteral("blurb"), s.blurb);
        m.insert(QStringLiteral("latest"), s.latest);
        m.insert(QStringLiteral("status"), s.status);
        m.insert(QStringLiteral("ready"), s.status == QLatin1String("ready") && !s.tgzUrl.isEmpty());
        m.insert(QStringLiteral("channel"), fwChannelLabel(currentChannelId(s)));
        m.insert(QStringLiteral("channelCount"), distinctChannelCount(s));

        // Same firmware family as the one running?
        const bool installed = (i == running);
        // Deliberately an equality test, not an ordering one. Version strings
        // across these forks have no comparable format ("unlshd-080e" vs
        // "unlshd-089", "RM0722-1811-ff9f4feb"), so claiming which is newer
        // would be guesswork. The store only ever lists the latest build, so
        // "differs from what is running" is the honest reading of "update".
        // Same reasoning as updateAvailable(): the feed and the device name the
        // same build differently ("dev-56701a81" against "Flipper-ARF"), so the
        // commit is what actually identifies it. The version string still works
        // for the forks that do agree on one.
        const QString devCommit = m_deviceCommit.trimmed();
        const bool sameBuild = installed && !s.latest.isEmpty()
                               && ((devCommit.length() >= 7 && s.latest.contains(devCommit, Qt::CaseInsensitive))
                                   || s.latest.compare(m_deviceVersion.trimmed(), Qt::CaseInsensitive) == 0);
        m.insert(QStringLiteral("date"), s.date);
        m.insert(QStringLiteral("installed"), installed);
        m.insert(QStringLiteral("upToDate"), sameBuild);
        m.insert(QStringLiteral("selected"), i == m_selected);
        // This panel picks; it never flashes. The only button that starts an
        // install is the one on the main screen, so a row can be staged
        // ("IMPORT"), already staged ("IMPORTED"), or the build already running.
        m.insert(QStringLiteral("action"), sameBuild      ? QStringLiteral("INSTALLED")
                                         : (i == m_selected) ? QStringLiteral("IMPORTED")
                                                             : QStringLiteral("IMPORT"));
        out.append(m);
    }
    return out;
}

void FirmwareStore::refresh()
{
    nikitaLog(QStringLiteral("firmware: checking %1 sources").arg(m_sources.size()));
    m_lastFetch = QDateTime::currentDateTime();
    m_pending = m_sources.size();
    m_foundUpdates = 0;
    m_checkSummary.clear();
    for (int i = 0; i < m_sources.size(); ++i) {
        // Only a source with nothing cached shows "checking". Clearing the
        // version of a source we already know turns a slow or rate-limited
        // reply into a row that reads "unavailable" for no reason.
        if (m_sources[i].raw.isEmpty()) {
            m_sources[i].status = QStringLiteral("checking");
            m_sources[i].latest.clear();
            m_sources[i].tgzUrl.clear();
            m_sources[i].date.clear();
        }
    }
    emit changed();
    for (int i = 0; i < m_sources.size(); ++i) { fetchOne(i); }
}

// Opening the panel shouldn't cost network requests when the answer is minutes
// old. "check for updates" still calls refresh() directly and always goes out.
void FirmwareStore::refreshIfStale()
{
    const bool haveAll = [this]() {
        for (const Source &s : m_sources) { if (s.raw.isEmpty()) { return false; } }
        return true;
    }();
    if (haveAll && m_lastFetch.isValid() && m_lastFetch.secsTo(QDateTime::currentDateTime()) < 600) {
        nikitaLog(QStringLiteral("firmware: versions are %1s old, not re-checking")
                 .arg(m_lastFetch.secsTo(QDateTime::currentDateTime())));
        return;
    }
    refresh();
}

QString FirmwareStore::currentChannelId(const Source &s) const
{
    if (s.channels.contains(s.wantChannel)) { return s.wantChannel; }
    return s.channels.isEmpty() ? s.wantChannel : s.channels.first();
}

void FirmwareStore::fetchOne(int index)
{
    if (index < 0 || index >= m_sources.size()) { return; }
    const Source src = m_sources.at(index);

    // GitHub: pull the whole release list (newest-first) so release/dev come from one fetch.
    QUrl url = (src.kind == Kind::DirJson)
             ? QUrl(src.locator)
             : QUrl(QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(src.locator));

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "Hyper-Zero-UI");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_net.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, index]() {
        reply->deleteLater();
        if (index < 0 || index >= m_sources.size()) { return; }
        Source &s = m_sources[index];

        if (reply->error() != QNetworkReply::NoError) {
            // Say what actually went wrong. GitHub answers 403 once the
            // unauthenticated 60-per-hour budget is spent, and four of these
            // sources share it -- without this line "unavailable" looks like a
            // broken source rather than a spent quota.
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString why = (http == 403 || http == 429)
                                ? QStringLiteral("rate limited by GitHub (HTTP %1) -- the hourly quota is shared by every firmware source").arg(http)
                                : (http > 0 ? QStringLiteral("HTTP %1").arg(http) : reply->errorString());
            nikitaLog(QStringLiteral("firmware: %1 lookup failed -- %2").arg(s.name, why));

            // Whatever was fetched last time is still perfectly good, so fall
            // back to it instead of blanking the row to "unavailable".
            if (!s.raw.isEmpty()) {
                deriveFromCache(index);
                nikitaLog(QStringLiteral("firmware: %1 kept this session's cached %2").arg(s.name, s.latest));
            } else if (loadDerived(index)) {
                nikitaLog(QStringLiteral("firmware: %1 restored saved %2").arg(s.name, s.latest));
            } else {
                s.status = QStringLiteral("error");
            }
            noteLookupDone(index);   // after the fallback, for the same reason
            emit changed();
            return;
        }
        s.raw = reply->readAll();

        // Discover the channel list from the directory.json, but keep only the
        // canonical channels -- Momentum also lists dozens of per-PR preview
        // channels (long ids like "pr294:feat/...") that we don't want to cycle.
        if (s.kind == Kind::DirJson) {
            const QJsonArray channels = QJsonDocument::fromJson(s.raw).object()
                                        .value(QStringLiteral("channels")).toArray();
            auto hasChannel = [&channels](const QString &id) {
                for (const QJsonValue &cv : channels) {
                    if (cv.toObject().value(QStringLiteral("id")).toString() == id) { return true; }
                }
                return false;
            };
            static const QStringList canonical = {
                QStringLiteral("release"), QStringLiteral("release-candidate"), QStringLiteral("development") };
            QStringList ids;
            for (const QString &c : canonical) { if (hasChannel(c)) { ids << c; } }
            s.channels = ids;
        }

        deriveFromCache(index);
        if (s.status == QLatin1String("ready")) {
            nikitaLog(QStringLiteral("firmware: %1 [%2] -> %3%4")
                     .arg(s.name, currentChannelId(s), s.latest,
                          s.date.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(s.date)));
        } else {
            nikitaLog(QStringLiteral("firmware: %1 [%2] replied, but no usable build was found in it")
                     .arg(s.name, currentChannelId(s)));
        }
        // Counted last, on purpose. This is what closes the verdict when the
        // final lookup lands, and it reads every source's resolved state -- so
        // running it before deriveFromCache() judged the last source on data it
        // had not parsed yet, and reported "couldn't check your firmware" one
        // line before printing that same firmware's version.
        noteLookupDone(index);
        emit changed();
    });
}

void FirmwareStore::deriveFromCache(int index)
{
    if (index < 0 || index >= m_sources.size()) { return; }
    Source &s = m_sources[index];
    const QString ch = currentChannelId(s);
    s.latest.clear();
    s.tgzUrl.clear();
    s.date.clear();
    if (s.raw.isEmpty()) {
        // No payload this session (fresh start, or the fetch was refused).
        // Whatever was saved for this channel still describes a real build.
        if (!loadDerived(index)) { s.status = QStringLiteral("error"); }
        return;
    }

    if (s.kind == Kind::DirJson) {
        const QJsonArray channels = QJsonDocument::fromJson(s.raw).object()
                                    .value(QStringLiteral("channels")).toArray();
        for (const QJsonValue &cv : channels) {
            const QJsonObject c = cv.toObject();
            if (c.value(QStringLiteral("id")).toString() != ch) { continue; }
            const QJsonArray versions = c.value(QStringLiteral("versions")).toArray();
            if (versions.isEmpty()) { break; }
            const QJsonObject v0 = versions.first().toObject();
            s.latest = v0.value(QStringLiteral("version")).toString();
            // directory.json carries a unix timestamp; some mirrors write an
            // ISO string instead, so accept either rather than showing nothing.
            const QJsonValue ts = v0.value(QStringLiteral("timestamp"));
            if (ts.isDouble()) {
                s.date = QDateTime::fromSecsSinceEpoch(qint64(ts.toDouble())).toString(QStringLiteral("yyyy-MM-dd"));
            } else {
                const QDateTime dt = QDateTime::fromString(v0.value(QStringLiteral("date")).toString(), Qt::ISODate);
                if (dt.isValid()) { s.date = dt.toString(QStringLiteral("yyyy-MM-dd")); }
            }
            for (const QJsonValue &fv : v0.value(QStringLiteral("files")).toArray()) {
                const QJsonObject f = fv.toObject();
                if (f.value(QStringLiteral("target")).toString() == QLatin1String("f7") &&
                    f.value(QStringLiteral("type")).toString() == QLatin1String("update_tgz")) {
                    s.tgzUrl = f.value(QStringLiteral("url")).toString();
                    break;
                }
            }
            break;
        }
    } else {   // GitHub: "dev" = newest prerelease, "release" = newest stable
        const QJsonArray rels = QJsonDocument::fromJson(s.raw).array();
        QJsonObject chosen;
        if (ch == QLatin1String("dev")) {
            // Was "the newest release, whatever it is" -- which on any repo that
            // doesn't publish prereleases is the exact same object the release
            // channel picks. The two channels then resolve to one build and
            // switching between them looks like it does nothing. Prefer a real
            // prerelease; fall back only when the repo has none.
            for (const QJsonValue &rv : rels) {
                const QJsonObject r = rv.toObject();
                if (r.value(QStringLiteral("prerelease")).toBool()) { chosen = r; break; }
            }
            if (chosen.isEmpty() && !rels.isEmpty()) { chosen = rels.first().toObject(); }
        } else {
            for (const QJsonValue &rv : rels) {
                const QJsonObject r = rv.toObject();
                if (!r.value(QStringLiteral("prerelease")).toBool()) { chosen = r; break; }
            }
            if (chosen.isEmpty() && !rels.isEmpty()) { chosen = rels.first().toObject(); }
        }
        if (!chosen.isEmpty()) {
            s.latest = chosen.value(QStringLiteral("tag_name")).toString();
            const QDateTime dt = QDateTime::fromString(
                chosen.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
            if (dt.isValid()) { s.date = dt.toString(QStringLiteral("yyyy-MM-dd")); }
            QString bestUrl, bestName, anyUrl, anyName;
            for (const QJsonValue &av : chosen.value(QStringLiteral("assets")).toArray()) {
                const QJsonObject a = av.toObject();
                const QString name = a.value(QStringLiteral("name")).toString();
                if (!name.endsWith(QLatin1String(".tgz"))) { continue; }
                const QString dl = a.value(QStringLiteral("browser_download_url")).toString();
                if (anyName.isEmpty() || name.size() < anyName.size()) { anyName = name; anyUrl = dl; }
                if (name.contains(QLatin1String("f7")) && name.contains(QLatin1String("update"))) {
                    if (bestName.isEmpty() || name.size() < bestName.size()) { bestName = name; bestUrl = dl; }
                }
            }
            s.tgzUrl = bestUrl.isEmpty() ? anyUrl : bestUrl;
        }
    }

    s.status = (!s.latest.isEmpty() && !s.tgzUrl.isEmpty()) ? QStringLiteral("ready")
                                                            : QStringLiteral("error");
    if (s.status == QLatin1String("ready")) { saveDerived(index); }
}

// The raw payloads only ever lived in memory, so every restart had to hit the
// network again -- and four of the five sources share GitHub's 60-per-hour
// unauthenticated budget. One rate-limited start was enough to leave the whole
// panel reading "unavailable" and the main button "No data". Persisting the
// handful of derived fields (not the payload, which runs to hundreds of KB)
// means a restart shows the right thing immediately and the network is only
// ever an improvement.
void FirmwareStore::saveDerived(int index) const
{
    if (index < 0 || index >= m_sources.size()) { return; }
    const Source &s = m_sources.at(index);
    const QString key = QStringLiteral("firmware/cache/%1/%2/").arg(s.name, currentChannelId(s));
    QSettings st;
    st.setValue(key + QStringLiteral("latest"), s.latest);
    st.setValue(key + QStringLiteral("tgz"), s.tgzUrl);
    st.setValue(key + QStringLiteral("date"), s.date);
}

bool FirmwareStore::loadDerived(int index)
{
    if (index < 0 || index >= m_sources.size()) { return false; }
    Source &s = m_sources[index];
    const QString key = QStringLiteral("firmware/cache/%1/%2/").arg(s.name, currentChannelId(s));
    QSettings st;
    const QString latest = st.value(key + QStringLiteral("latest")).toString();
    const QString tgz    = st.value(key + QStringLiteral("tgz")).toString();
    if (latest.isEmpty() || tgz.isEmpty()) { return false; }
    s.latest = latest;
    s.tgzUrl = tgz;
    s.date   = st.value(key + QStringLiteral("date")).toString();
    s.status = QStringLiteral("ready");
    return true;
}

void FirmwareStore::cycleChannel(int index)
{
    if (index < 0 || index >= m_sources.size()) { return; }
    Source &s = m_sources[index];
    if (s.channels.size() < 2) { return; }
    int i = s.channels.indexOf(currentChannelId(s));
    i = (i + 1) % s.channels.size();
    s.wantChannel = s.channels.at(i);
    QSettings().setValue(QStringLiteral("firmware/ch/") + s.name, s.wantChannel);
    deriveFromCache(index);   // every channel is already cached -> instant, no re-fetch
    emit changed();
}

void FirmwareStore::install(int index)
{
    if (index < 0 || index >= m_sources.size()) { return; }
    const Source src = m_sources.at(index);

    // Everything below reports through failed()/progress(), which are wired to
    // the store panel -- and that panel is closed when this runs from Reinstall
    // in the tools tab. Without these lines the whole download is invisible:
    // no progress, and a failure that never reaches the user.
    if (src.status != QLatin1String("ready") || src.tgzUrl.isEmpty()) {
        nikitaLog(QStringLiteral("firmware: install %1 refused -- no downloadable build").arg(src.name));
        emit failed(index, QStringLiteral("No downloadable build found -- try re-checking."));
        return;
    }
    if (m_busy) {
        nikitaLog(QStringLiteral("firmware: install %1 refused -- a download is already running").arg(src.name));
        emit failed(index, QStringLiteral("A download is already in progress."));
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty()) { dir = QDir::tempPath(); }
    dir += QStringLiteral("/firmware");
    QDir().mkpath(dir);

    const QUrl url(src.tgzUrl);
    QString fileName = url.fileName();
    if (fileName.isEmpty() || !fileName.endsWith(QLatin1String(".tgz"))) {
        fileName = QStringLiteral("flipper-z-f7-update.tgz");
    }
    const QString outPath = dir + QStringLiteral("/") + fileName;

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "nikita-qflipper");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    rememberFlashedChannel(index);
    setBusy(true);
    nikitaLog(QStringLiteral("firmware: downloading %1 %2 from %3")
             .arg(src.name, src.latest, src.tgzUrl));
    emit progress(index, 0.0, QStringLiteral("Downloading %1…").arg(src.latest));

    QNetworkReply *reply = m_net.get(req);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, index](qint64 rec, qint64 total) {
        const qreal frac = (total > 0) ? (qreal)rec / (qreal)total : 0.0;
        emit progress(index, frac, QStringLiteral("Downloading…"));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, index, outPath]() {
        reply->deleteLater();
        setBusy(false);
        if (reply->error() != QNetworkReply::NoError) {
            nikitaLog(QStringLiteral("firmware: download FAILED -- %1").arg(reply->errorString()));
            emit failed(index, QStringLiteral("Download failed: %1").arg(reply->errorString()));
            return;
        }
        QFile f(outPath);
        if (!f.open(QIODevice::WriteOnly)) {
            nikitaLog(QStringLiteral("firmware: couldn't write %1").arg(outPath));
            emit failed(index, QStringLiteral("Couldn't save the download to disk."));
            return;
        }
        const QByteArray body = reply->readAll();
        f.write(body);
        f.close();
        nikitaLog(QStringLiteral("firmware: downloaded %1 bytes -> %2").arg(body.size()).arg(outPath));
        emit progress(index, 1.0, QStringLiteral("Ready -- flashing…"));
        emit readyToInstall(QUrl::fromLocalFile(outPath).toString());
    });
}

// ===================== FlipperCli: in-app Flipper text CLI =====================

// ---- shared helpers (command tables, path routing, help layout) -------------
namespace {

// ---- command model ---------------------------------------------------------
// One table, one identity per command. Everything the CLI knows about a
// command -- what it is called, what else people type instead, where it runs,
// how to use it, what "help" prints -- lives in a single row, and dispatch
// switches on the row's id. Adding a row without handling its id is a compiler
// warning (the dispatch switch has no default), so the table and the behaviour
// cannot drift apart the way a parallel if/else chain does.

enum class CliVerb {
    None,
    // panel-local: never touch the wire
    Clear, Colors, Verbose, History, Help, Tgz, HostRun,
    // Flipper filesystem and device
    Cd, Pwd, Ls, Tree, Cat, Rm, Mkdir, Cp, Mv, Stat, Md5, Df, Du, Touch,
    Find, Grep, Head, Tail, Wc, Sed, Diff, FileType, Locate, Echo, Wget,
    Edit, Reboot, Shutdown, Open, Close, Vibro, Name,
    // this computer
    Chmod, Kill, Ps, Pkg, Mount, Umount, Fdisk, Lsblk, Ifconfig, Ping,
    Uname, Pm3, Sudo, SuLike, Whoami, Hostname, FWhoami, Python3,
    FlipperInstall
};

// Where a command does its work. Only used for the one-line "runs on" hint in
// help and for the banner, but keeping it in the table means a new command has
// to declare it.
// Which of help's three sections a row belongs to. Device is this computer and
// anything else on the USB bus; Flipper is the f-prefixed half; Panel is the
// terminal itself. The firmware's own commands are a fourth list entirely and
// are never in this table.
enum class CliWhere { Panel, Device, Flipper, Either };

// A command has up to two spellings, one per machine, and they are two columns
// of the same row so a command can never exist on one side and be forgotten on
// the other.
//
//   name   the bare spelling -- acts on THIS computer, like any terminal
//   fname  the f-prefixed spelling -- acts on the Flipper
//
// Either may be empty: "sudo" has no meaning on a Flipper, "fvibro" has none
// here. Panel commands (help, clear, colors) use name only.
struct CliCommandDef {
    CliVerb      id;
    const char  *name;
    const char  *fname;
    const char  *aliases;   // space-separated, may be empty
    CliWhere     where;
    const char  *usage;     // empty when the bare name is the whole usage
    const char  *help;
};

const CliCommandDef kCliCommands[] = {
{ CliVerb::Cat,      "cat",         "fcat",        "read type",                  CliWhere::Either,  "cat <file>",
  "Prints a file's contents to the screen." },
{ CliVerb::Cd,       "cd",          "fcd",         "chdir lcd",                      CliWhere::Either,  "cd [path]",
  "Changes the current folder on the Flipper. 'cd -' goes back, 'cd ~' goes to /ext." },
{ CliVerb::Chmod,    "chmod",       "",            "chown chgrp",                CliWhere::Device,  "chmod <mode> <path>",
  "Changes permissions on a file on THIS computer. The Flipper's FatFS has no owners or permission bits." },
{ CliVerb::Clear,    "clear",       "",            "cls",                        CliWhere::Panel,   "",
  "Clears the terminal view." },
{ CliVerb::Close,    "",            "fclose",      "",                           CliWhere::Flipper, "fclose",
  "Closes the app currently running on the Flipper (loader close)." },
{ CliVerb::Colors,   "colors",      "",            "color colours",              CliWhere::Panel,   "colors on | off",
  "Colours folders, prompt and log lines the way ls --color does." },
{ CliVerb::Cp,       "cp",          "fcp",         "copy pull push",             CliWhere::Either,  "cp [-r] <source> <destination>",
  "Copies a file or folder (-r), including between this computer and the Flipper. Wildcards (*.sub) work as a source. Every transfer is MD5-verified." },
{ CliVerb::Df,       "df",          "fdf",         "diskfree",                   CliWhere::Either,  "df [path]",
  "Shows free and used space on the Flipper's storage." },
{ CliVerb::Diff,     "diff",        "fdiff",       "",                           CliWhere::Either,  "diff <fileA> <fileB>",
  "Shows what differs between two files, line by line." },
{ CliVerb::Du,       "du",          "fdu",         "",                           CliWhere::Either,  "du [path]",
  "Adds up how much space a folder uses, broken down by what is inside it." },
{ CliVerb::Echo,     "echo",        "fecho",       "",                           CliWhere::Either,  "echo <text> [> file | >> file]",
  "Echoes text back; with a redirect it writes to a file on the Flipper instead." },
{ CliVerb::Edit,     "edit",        "fedit",       "nano vi vim emacs pico micro",  CliWhere::Either,  "edit <file>",
  "Opens a file in the editor panel. Works on Flipper files and on files on this computer." },
{ CliVerb::Fdisk,    "fdisk",       "",            "diskutil",                   CliWhere::Device,  "fdisk [args]",
  "Runs fdisk on THIS computer. The Flipper's volumes are fixed; 'df' shows their space." },
{ CliVerb::FileType, "file",        "ffile",       "",                           CliWhere::Either,  "file <path>",
  "Identifies what a file is from its contents." },
{ CliVerb::Find,     "find",        "ffind",       "",                           CliWhere::Either,  "find <pattern> [path]",
  "Finds files under a folder by name, e.g. find *.sub /ext/subghz." },
{ CliVerb::FlipperInstall, "flipper", "",          "",                           CliWhere::Device,  "flipper install <url to a .fap> [name.fap]",
  "Downloads a .fap app from a URL and installs it straight to /ext/apps -- a package-manager-style install for the Flipper. Same 1 MB size cap as wget. Check it landed with 'loader list', launch it with fopen \"App Name\"." },
{ CliVerb::Grep,     "grep",        "fgrep",       "",                           CliWhere::Either,  "grep <text> <file>",
  "Prints the lines of a file that contain some text." },
{ CliVerb::Head,     "head",        "fhead",       "",                           CliWhere::Either,  "head [-n lines] <file>",
  "Prints the first lines of a file." },
{ CliVerb::Help,     "help",        "",            "?",                          CliWhere::Panel,   "help [command]",
  "Lists the available commands, or explains one of them." },
{ CliVerb::History,  "history",     "",            "",                           CliWhere::Panel,   "history [-c]",
  "Lists the commands typed here. '!12' re-runs one, '!!' repeats the last." },
{ CliVerb::HostRun,  "host",        "",            "local run",                  CliWhere::Device,  "host <command> [args]",
  "Runs any command on THIS computer: host whoami, host git status, host bash myscript.sh. The escape hatch for anything the Flipper also has a name for." },
{ CliVerb::Ifconfig, "ifconfig",    "",            "ip",                         CliWhere::Device,  "ifconfig",
  "Shows THIS COMPUTER's network interfaces. The Flipper has none." },
{ CliVerb::Kill,     "kill",        "",            "killall pkill",              CliWhere::Device,  "kill <pid>",
  "Kills a process on THIS computer. To stop the Flipper's running app use 'close', to stop a command use Ctrl-C." },
{ CliVerb::Locate,   "",            "flocate",     "",                           CliWhere::Flipper, "flocate <text>",
  "Searches the whole SD card for a name (find rooted at /ext)." },
{ CliVerb::Ls,       "ls",          "fls",         "dir ll la",                  CliWhere::Either,  "ls [path]",
  "Lists the files and folders in a directory." },
{ CliVerb::Lsblk,    "lsblk",       "",            "",                           CliWhere::Device,  "lsblk",
  "Lists THIS COMPUTER's block devices." },
{ CliVerb::Md5,      "md5",         "fmd5",        "md5sum",                     CliWhere::Either,  "md5 <file>",
  "Prints a file's MD5 hash." },
{ CliVerb::Mkdir,    "mkdir",       "fmkdir",      "md",                         CliWhere::Either,  "mkdir [-p] <path>",
  "Creates a folder. With -p it creates every missing parent too." },
{ CliVerb::Mount,    "mount",       "",            "",                           CliWhere::Device,  "mount",
  "Shows THIS COMPUTER's mounts. The Flipper's /int and /ext are always mounted; 'df' shows their space." },
{ CliVerb::Mv,       "mv",          "fmv",         "move ren",                   CliWhere::Either,  "mv <source> <destination>",
  "Moves or renames a file or folder on the Flipper." },
{ CliVerb::Name,     "name",        "fname",       "rename",                     CliWhere::Flipper, "name <2-8 letters/numbers> | name reset",
  "Sets the Flipper's custom device name. ONLY WORKS ON CUSTOM FIRMWARE (Momentum/Unleashed/RogueMaster): official firmware takes the name from the factory OTP block and cannot be renamed -- the command detects that and refuses instead of rebooting for nothing. On custom firmware it writes dolphin/name.settings and reboots. 'name reset' removes the name files (and skips the reboot on official firmware)." },
{ CliVerb::Open,     "open",        "fopen",       "browser website",            CliWhere::Either,  "open <app|file|folder|url>",
  "open <file|folder|url> opens it on THIS computer with its default app -- a URL opens in the browser. fopen <app> launches an app ON THE FLIPPER instead, e.g. fopen NFC." },
{ CliVerb::Pkg,      "apt",         "",            "apt-get yum dnf brew pacman apk zypper",  CliWhere::Device,  "apt [args]",
  "Runs the package manager on THIS computer. The Flipper has none: its apps are .fap files you copy into /ext/apps." },
{ CliVerb::Ping,     "ping",        "",            "",                           CliWhere::Device,  "ping <host>",
  "Pings a host FROM THIS COMPUTER. The Flipper has no network." },
{ CliVerb::Pm3,      "pm3",         "",            "proxmark3 proxmark",         CliWhere::Device,  "pm3 <proxmark command>",
  "Runs a Proxmark3 command via the pm3 client on THIS computer, e.g. pm3 hf search." },
{ CliVerb::Ps,       "ps",          "",            "",                           CliWhere::Device,  "ps",
  "Lists processes on THIS COMPUTER. For the Flipper's own threads use 'top'." },
{ CliVerb::Python3,  "python3",     "",            "python",                     CliWhere::Device,  "python3 [/ext/path/to/script.py | script.py] [args...]",
  "Runs Python on THIS computer -- there's no Python on the Flipper itself. Given a device path (/ext/... or /int/...), the script is fetched off the SD card first and run from there, so a script saved on the card runs the same way no matter which computer the Flipper is plugged into." },
{ CliVerb::Pwd,      "pwd",         "fpwd",        "lpwd",                           CliWhere::Either,  "",
  "Prints the current folder on the Flipper." },
{ CliVerb::Reboot,   "",            "freboot",     "restart",                    CliWhere::Flipper, "",
  "Restarts the Flipper." },
{ CliVerb::Rm,       "rm",          "frm",         "del erase",                  CliWhere::Either,  "rm [-r] [-f] <path>",
  "Deletes a file, or a folder and everything in it (-r). Wildcards work. Deleting a whole tree needs -f." },
{ CliVerb::Sed,      "sed",         "fsed",        "",                           CliWhere::Either,  "sed s/old/new/[g] <file>",
  "Find-and-replace inside a file. Overwrites the file with the result." },
{ CliVerb::Shutdown, "",            "fshutdown",   "poweroff halt",              CliWhere::Flipper, "",
  "Powers the Flipper off." },
{ CliVerb::Stat,     "stat",        "fstat",       "",                           CliWhere::Either,  "stat <path>",
  "Shows the size and type of a file or folder." },
{ CliVerb::SuLike,   "su",          "",            "login passwd useradd",       CliWhere::Device,  "su [user]",
  "Needs a real terminal to ask for a password, which this panel is not. Use 'sudo' instead, or a terminal window." },
{ CliVerb::Sudo,     "sudo",        "",            "doas",                       CliWhere::Device,  "sudo <command>",
  "Runs a command as root on THIS computer. There is no terminal here to type a password into, so run 'sudo -v' in a terminal first to cache your credentials." },
{ CliVerb::Tail,     "tail",        "ftail",       "",                           CliWhere::Either,  "tail [-n lines] <file>",
  "Prints the last lines of a file." },
{ CliVerb::Tgz,      "tgz",         "",            "",                           CliWhere::Panel,   "tgz <folder> [archive.tgz]",
  "Packs a folder on this computer into a .tgz, the same way Backup does." },
{ CliVerb::Touch,    "touch",       "ftouch",      "",                           CliWhere::Either,  "touch <file>",
  "Creates an empty file on the Flipper." },
{ CliVerb::Tree,     "tree",        "ftree",       "",                           CliWhere::Either,  "tree [path]",
  "Lists everything under a folder, recursively." },
{ CliVerb::Umount,   "umount",      "",            "unmount",                    CliWhere::Device,  "umount <target>",
  "Unmounts a volume on THIS computer. The Flipper's /int and /ext cannot be unmounted from here." },
{ CliVerb::Uname,    "uname",       "",            "",                           CliWhere::Device,  "uname",
  "Shows THIS COMPUTER's system info. For the Flipper's own, use device_info." },
{ CliVerb::Verbose,  "verbose",     "",            "",                           CliWhere::Panel,   "verbose on | off",
  "Shows or hides the wire-level log of everything a command runs." },
{ CliVerb::Vibro,    "",            "fvibro",      "buzz vibrate",               CliWhere::Flipper, "fvibro [0|1]",
  "Turns the Flipper's vibration motor on or off." },
{ CliVerb::Whoami,   "whoami",      "fwhoami",     "",                           CliWhere::Either,  "whoami",
  "Prints your user name on THIS computer. For the Flipper's own identity use fwhoami." },
{ CliVerb::Hostname, "hostname",    "",            "",                           CliWhere::Device,  "hostname",
  "Prints THIS COMPUTER's host name. For the Flipper's own name use fwhoami." },
{ CliVerb::Wc,       "wc",          "fwc",         "",                           CliWhere::Either,  "wc <file>",
  "Counts the lines, words and bytes in a file." },
{ CliVerb::Wget,     "wget",        "fwget",       "curl fetch",                 CliWhere::Either,  "wget <url> [destination]",
  "Downloads a URL on this computer and saves it straight onto the Flipper. The Flipper has no network of its own." },

};

// Commands that are simply passed through to a program of the same name on this
// computer. They have no Flipper meaning at all, so there is nothing to
// disambiguate and no dedicated row above -- but they still have to be known,
// or Tab completion and "help" go quiet on them.
const char *const kHostPassthrough[] = {
    "awk", "base64", "dig", "docker", "env", "git", "gzip", "hexdump", "hostname",
    "id", "ifdown", "ifup", "lsof", "man", "netstat", "nmap", "nslookup", "openssl",
    "sha256sum", "ssh", "tar", "traceroute", "unzip", "which", "xxd", "zip",
};

// Which of the pass-through commands actually exist on this computer. They are
// forwarded to a program of the same name, so "listed" and "works" are two
// different things: nmap, docker or dig are only there if you installed them.
// `help` says which ones are missing rather than letting you find out by
// running one and reading a spawn error.
QString cliMissingPassthrough()
{
    QStringList missing;
    for (const char *name : kHostPassthrough) {
        const QString n = QString::fromLatin1(name);
        if (QStandardPaths::findExecutable(n).isEmpty()) { missing += n; }
    }
    missing.sort();
    return missing.join(QLatin1Char(' '));
}

// Your user name on this computer. $USER is unset in a GUI process launched
// from Finder or a .desktop file, so fall back to the home folder's name, which
// is right on every platform this app ships to.
QString cliHostUser()
{
    QString u = qEnvironmentVariable("USER");
    if (u.isEmpty()) { u = qEnvironmentVariable("USERNAME"); }
    if (u.isEmpty()) { u = QFileInfo(QDir::homePath()).fileName(); }
    return u;
}

// Programs that insist on a controlling terminal to ask for a password. A
// QProcess has none, so they would sit there until the watchdog killed them
// with nothing on screen to explain why.
bool cliNeedsTty(const QString &program)
{
    static const QSet<QString> tty = {
        QStringLiteral("su"), QStringLiteral("login"), QStringLiteral("passwd"),
        QStringLiteral("useradd"), QStringLiteral("adduser"), QStringLiteral("chpasswd"),
    };
    return tty.contains(program);
}

// Commands that exist on both machines under the same name, so a host path in
// the arguments can safely hand the whole line to that program here. cp,
// wget and edit are absent on purpose -- they already understand both
// machines themselves.

bool cliRoutesByPath(CliVerb id)
{
    switch (id) {
    case CliVerb::Ls:   case CliVerb::Cat:  case CliVerb::Stat: case CliVerb::Du:
    case CliVerb::Wc:   case CliVerb::Grep: case CliVerb::Head: case CliVerb::Tail:
    case CliVerb::Find: case CliVerb::Diff: case CliVerb::Mkdir:
    case CliVerb::Touch: case CliVerb::Rm:  case CliVerb::Mv:   case CliVerb::FileType:
    // md5 and sed were missing, so both always went to the Flipper: "md5 a.txt"
    // in a host folder answered "Storage error: file/dir not exist", and
    // "sed s/x/y/ a.txt" tried to read /ext/a.txt. They have f-aliases like
    // every other entry here, which is exactly what makes the bare spelling
    // mean this computer.
    case CliVerb::Md5:  case CliVerb::Sed:
        return true;
    default:
        return false;
    }
}

// The real program name on THIS operating system. The table stores the name the
// user types, and for almost everything that is also the binary -- but md5 is
// Apple's spelling and Linux ships the same job as md5sum. Routing md5 to the
// host without this turns a command that used to work on Linux (by going to the
// Flipper) into "command not found".
static QString cliHostProgramFor(const CliCommandDef *def)
{
#ifndef Q_OS_MACOS
    if (def->id == CliVerb::Md5) { return QStringLiteral("md5sum"); }
#endif
    return QString::fromLatin1(def->name);
}

// What a typed word resolves to: the row, and which machine the spelling asked
// for. The two travel together because every caller needs both -- the row says
// what to do, the flag says where.
struct CliMatch {
    const CliCommandDef *def = nullptr;
    bool flipper = false;                     // the f-prefixed spelling was used
    explicit operator bool() const { return def != nullptr; }
};

CliMatch cliMatch(const QString &name)
{
    const QString n = name.toLower();
    // Exact spellings first, both columns, before any alias: a row's own name
    // must never lose to another row's alias.
    for (const CliCommandDef &c : kCliCommands) {
        if (*c.name  && n == QLatin1String(c.name))  { return { &c, false }; }
        if (*c.fname && n == QLatin1String(c.fname)) { return { &c, true  }; }
    }
    for (const CliCommandDef &c : kCliCommands) {
        if (!*c.aliases) { continue; }
        const QStringList al = QString::fromLatin1(c.aliases).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (al.contains(n)) { return { &c, false }; }
        // An alias picks up the f-prefix too, so "fdir" is "fls" without having
        // to spell every variant out in the table.
        if (n.startsWith(QLatin1Char('f')) && *c.fname && al.contains(n.mid(1))) {
            return { &c, true };
        }
    }
    return {};
}

const CliCommandDef *cliLookup(const QString &name)
{
    return cliMatch(name).def;
}

// Every spelling in one section of the table, sorted. Drives both the help
// listing and Tab completion, so the two can never disagree.
QStringList cliNamesIn(CliWhere section)
{
    QStringList out;
    for (const CliCommandDef &c : kCliCommands) {
        const bool both = (c.where == CliWhere::Either);
        if (section == CliWhere::Flipper && *c.fname && (both || c.where == CliWhere::Flipper)) {
            out += QLatin1String(c.fname);
        }
        if (section == CliWhere::Device && *c.name && (both || c.where == CliWhere::Device)) {
            out += QLatin1String(c.name);
        }
        if (section == CliWhere::Panel && c.where == CliWhere::Panel) {
            out += QLatin1String(c.name);
        }
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QStringList cliOurNames()
{
    QStringList out;
    for (const CliCommandDef &c : kCliCommands) {
        if (*c.name)  { out += QLatin1String(c.name); }
        if (*c.fname) { out += QLatin1String(c.fname); }
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString cliHelpFor(const QString &name)
{
    const CliMatch m = cliMatch(name);
    if (!m) { return QString(); }
    const CliCommandDef *d = m.def;
    const QString canon = QLatin1String(m.flipper ? d->fname : d->name);

    QString out;
    if (*d->usage) {
        // The usage line is written with the bare spelling; show it with the
        // one the user actually typed.
        QString u = QLatin1String(d->usage);
        if (m.flipper && *d->name) { u.replace(0, int(qstrlen(d->name)), QLatin1String(d->fname)); }
        out += QStringLiteral("usage: %1\n").arg(u);
    }
    out += QLatin1String(d->help);
    out += m.flipper ? QStringLiteral("\n(acts on the Flipper)")
                     : (d->where == CliWhere::Panel ? QStringLiteral("\n(handled by this panel)")
                                                    : QStringLiteral("\n(acts on this computer)"));
    if (d->where == CliWhere::Either && *d->name && *d->fname) {
        out += QStringLiteral("\n%1 is the other half: %2")
               .arg(QLatin1String(m.flipper ? d->name : d->fname),
                    QLatin1String(m.flipper ? "this computer" : "the Flipper"));
    }
    if (canon != name.toLower()) { out += QStringLiteral("\n(alias for %1)").arg(canon); }
    return out;
}

// One-line descriptions for the firmware's own commands, from the official CLI
// reference (docs.flipper.net/zero/development/cli), so "help <command>" answers
// for everything on the list instead of going quiet.
struct CliFwCmd { const char *name; const char *help; };
const CliFwCmd kFwCommands[] = {
    { "!",                 "Alias for <info device>." },
    { "bt",                "Bluetooth test app -- reads the BLE HCI version." },
    { "buzzer",            "Plays a frequency or a musical note on the piezo speaker." },
    { "crypto",            "Encrypts and decrypts text using keys in the secure enclave." },
    { "date",              "Shows or sets the date and time." },
    { "exit",              "Leaves the CLI shell -- useful inside a secondary shell." },
    { "factory_reset",     "Resets the device to factory settings; the microSD is kept." },
    { "free",              "Shows heap memory allocator information." },
    { "free_blocks",       "Shows free heap blocks and their sizes, for fragmentation." },
    { "gpio",              "Sets pin mode and reads or writes GPIO pin state." },
    { "i2c",               "Scans the I2C bus for devices." },
    { "ikey",              "Reads, emulates and writes iButton keys." },
    { "info",              "Shows detailed device and power system information." },
    { "input",             "Shows button presses and injects input events." },
    { "ir",                "Reads and sends infrared signals." },
    { "js",                "Runs a JavaScript file and prints its console output." },
    { "led",               "Sets the status LED colour and the display backlight." },
    { "loader",            "Lists, opens and closes applications." },
    { "log",               "Streams the system log; Ctrl-C stops it." },
    { "neofetch",          "Prints system info, neofetch style." },
    { "nfc",               "Opens the NFC shell to read and emulate cards." },
    { "onewire",           "Scans the 1-Wire bus for devices." },
    { "power",             "Powers off, reboots, and switches GPIO power rails." },
    { "reload_ext_cmds",   "Reloads the external commands stored on the microSD." },
    { "rfid",              "Reads, writes and emulates low-frequency RFID cards." },
    { "start_rpc_session", "Switches the CLI into protobuf RPC mode." },
    { "storage",           "Filesystem commands under /int and /ext." },
    { "subghz",            "Sub-GHz tools: transmit, receive, decode and chat." },
    { "sysctl",            "Configures system settings such as debug and heap tracking." },
    { "top",               "Lists running threads in real time; Ctrl-C quits." },
    { "update",            "Installs updates and backs up or restores internal storage." },
    { "uptime",            "Shows the time since the last reboot." },
};

QString cliFwHelpFor(const QString &name)
{
    for (const CliFwCmd &c : kFwCommands) {
        if (name == QLatin1String(c.name)) { return QLatin1String(c.help); }
    }
    return QString();
}

// Every name the CLI answers to -- ours, every alias of ours, the host
// passthroughs and the firmware's own -- for Tab completion of the first word.
// Built from the same tables the dispatcher reads, so a command can never be
// runnable but uncompletable.
QStringList cliAllCommandNames()
{
    QStringList out;
    for (const CliCommandDef &c : kCliCommands) {
        if (*c.name)  { out += QLatin1String(c.name); }
        if (*c.fname) { out += QLatin1String(c.fname); }
        if (*c.aliases) {
            const QStringList al = QString::fromLatin1(c.aliases)
                                   .split(QLatin1Char(' '), Qt::SkipEmptyParts);
            out += al;
            if (*c.fname) { for (const QString &a : al) { out += QLatin1Char('f') + a; } }
        }
    }
    for (const char *const n : kHostPassthrough) {
        QString s = QLatin1String(n);
        if (s.endsWith(QLatin1String("-host"))) { continue; }   // reachable only via "host <name>"
        out += s;
    }
    for (const CliFwCmd &c : kFwCommands) { out += QLatin1String(c.name); }
    out.removeAll(QStringLiteral("!"));
    out.removeAll(QStringLiteral("?"));
    out.removeDuplicates();
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

// A passthrough only applies to a name the table does NOT claim: the table
// always wins, so listing a name in both places would silently make the
// passthrough entry unreachable rather than ambiguous.
bool cliIsHostPassthrough(const QString &name)
{
    if (cliLookup(name)) { return false; }
    for (const char *const n : kHostPassthrough) {
        if (name == QLatin1String(n)) { return true; }
    }
    return false;
}

// ---- tokenising ------------------------------------------------------------
// Splitting on whitespace loses every path with a space in it, and the Flipper
// is full of them ("/ext/badusb/Bad USB Demo.txt", app names like "Sub-GHz").
// This is a real shell-style tokeniser: double quotes, single quotes and
// backslash escapes, with the quotes removed from the result.
QStringList cliTokenize(const QString &line, bool *unterminated = nullptr)
{
    QStringList out;
    QString cur;
    bool have = false;
    QChar quote;
    bool esc = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (esc) { cur += ch; have = true; esc = false; continue; }
        if (ch == QLatin1Char('\\') && quote != QLatin1Char('\'')) { esc = true; have = true; continue; }
        if (!quote.isNull() && ch == quote) { quote = QChar(); continue; }
        if (quote.isNull() && (ch == QLatin1Char('"') || ch == QLatin1Char('\''))) {
            quote = ch; have = true; continue;
        }
        if (quote.isNull() && ch.isSpace()) {
            if (have) { out += cur; cur.clear(); have = false; }
            continue;
        }
        cur += ch;
        have = true;
    }
    if (have) { out += cur; }
    if (unterminated) { *unterminated = !quote.isNull() || esc; }
    return out;
}

// The inverse, for anything the CLI puts back on the input line (Tab
// completion) or sends to the firmware, whose own parser splits on spaces.
QString cliQuoteArg(const QString &arg)
{
    if (arg.isEmpty()) { return QStringLiteral("\"\""); }
    static const QRegularExpression needs(QStringLiteral("[\\s\"'\\\\]"));
    if (!needs.match(arg).hasMatch()) { return arg; }
    QString s = arg;
    s.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    s.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

// Where the token under the caret begins, honouring quotes, so Tab completion
// of "/ext/My Ca|" completes the whole path and not just "Ca".
int cliTokenStart(const QString &line)
{
    int start = 0;
    QChar quote;
    bool esc = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (esc) { esc = false; continue; }
        if (ch == QLatin1Char('\\') && quote != QLatin1Char('\'')) { esc = true; continue; }
        if (!quote.isNull()) { if (ch == quote) { quote = QChar(); } continue; }
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) { quote = ch; continue; }
        if (ch.isSpace()) { start = i + 1; }
    }
    return start;
}

// ---- flag parsing ----------------------------------------------------------
// One parser instead of removeAll() sprinkled through the dispatcher. It knows
// about clustered short flags (-rf), long flags (--recursive, --lines=20),
// flags that take a value (-n 20), and "--" ending the flags -- so a file
// genuinely named "-n" is still reachable, and "head file -n 5" works as well
// as "head -n 5 file".
struct CliArgs {
    QString     verb;         // canonical bare spelling, lower-cased
    QString     typedVerb;    // exactly what was typed, for messages
    bool        flipper = false;   // the f-prefixed spelling was used
    QStringList positional;
    QStringList rawTokens;    // everything after the verb, untouched
    QSet<QString> flags;      // "r", "f", "p", "recursive", ...
    QHash<QString, QString> values;   // "n" -> "20"

    bool has(const char *f) const { return flags.contains(QLatin1String(f)); }
    QString at(int i) const { return positional.value(i); }
    int count() const { return positional.size(); }
    QString value(const char *f, const QString &fallback = QString()) const {
        return values.value(QLatin1String(f), fallback);
    }
};

// Flags that consume the next token as their value, per command. Small enough
// to be a flat list; anything not here is a plain boolean flag.
bool cliFlagTakesValue(const QString &verb, const QString &flag)
{
    if (flag == QLatin1String("n") || flag == QLatin1String("lines")) {
        return verb == QLatin1String("head") || verb == QLatin1String("tail");
    }
    if (flag == QLatin1String("c") || flag == QLatin1String("count")) {
        return verb == QLatin1String("ping");
    }
    return false;
}

CliArgs cliParseArgs(const QStringList &tokens)
{
    CliArgs r;
    if (tokens.isEmpty()) { return r; }
    r.typedVerb = tokens.first();
    r.verb = tokens.first().toLower();
    const CliMatch m = cliMatch(r.verb);
    if (m) {
        r.flipper = m.flipper;
        // Flag parsing is keyed on the bare spelling, so "fhead -n 5" takes a
        // value for -n exactly like "head -n 5" does.
        r.verb = QLatin1String(*m.def->name ? m.def->name : m.def->fname);
    }
    r.rawTokens = tokens.mid(1);

    bool noMoreFlags = false;
    for (int i = 0; i < r.rawTokens.size(); ++i) {
        const QString t = r.rawTokens.at(i);
        if (noMoreFlags || t.size() < 2 || !t.startsWith(QLatin1Char('-'))) {
            r.positional += t;
            continue;
        }
        if (t == QLatin1String("--")) { noMoreFlags = true; continue; }

        if (t.startsWith(QLatin1String("--"))) {
            QString name = t.mid(2);
            const int eq = name.indexOf(QLatin1Char('='));
            if (eq >= 0) {
                r.values.insert(name.left(eq), name.mid(eq + 1));
                r.flags.insert(name.left(eq));
                continue;
            }
            r.flags.insert(name);
            if (cliFlagTakesValue(r.verb, name) && i + 1 < r.rawTokens.size()) {
                r.values.insert(name, r.rawTokens.at(++i));
            }
            continue;
        }
        // A clustered short group: -rf is -r and -f. The last letter of the
        // group is the one allowed to take a value, exactly like getopt.
        const QString group = t.mid(1);
        for (int c = 0; c < group.size(); ++c) {
            const QString f = QString(group.at(c));
            r.flags.insert(f);
            if (c == group.size() - 1 && cliFlagTakesValue(r.verb, f)) {
                if (c + 1 < group.size()) { continue; }
                if (i + 1 < r.rawTokens.size()) { r.values.insert(f, r.rawTokens.at(++i)); }
            }
        }
    }
    // Long spellings fold onto the short ones the dispatcher checks.
    if (r.flags.contains(QStringLiteral("recursive"))) { r.flags.insert(QStringLiteral("r")); }
    if (r.flags.contains(QStringLiteral("force")))     { r.flags.insert(QStringLiteral("f")); }
    if (r.flags.contains(QStringLiteral("parents")))   { r.flags.insert(QStringLiteral("p")); }
    if (r.flags.contains(QStringLiteral("R")))         { r.flags.insert(QStringLiteral("r")); }
    if (r.values.contains(QStringLiteral("lines")))    { r.values.insert(QStringLiteral("n"), r.values.value(QStringLiteral("lines"))); }
    return r;
}

// The Flipper's own filesystem is /ext (SD), /int (internal) and /any.
bool cliIsDevicePath(const QString &p)
{
    return p.startsWith(QLatin1String("/ext"), Qt::CaseInsensitive)
        || p.startsWith(QLatin1String("/int"), Qt::CaseInsensitive)
        || p.startsWith(QLatin1String("/any"), Qt::CaseInsensitive);
}

// A storage root, not a folder inside one. "storage stat /ext" answers with the
// volume ("Storage, label: ...") instead of "Directory", which is why a plain
// "cd .." out of /ext/nfc used to be rejected as a missing folder.
bool cliIsStorageRoot(const QString &p)
{
    QString s = p;
    while (s.size() > 1 && s.endsWith(QLatin1Char('/'))) { s.chop(1); }
    return s.compare(QLatin1String("/ext"), Qt::CaseInsensitive) == 0
        || s.compare(QLatin1String("/int"), Qt::CaseInsensitive) == 0
        || s.compare(QLatin1String("/any"), Qt::CaseInsensitive) == 0;
}

// "storage info" answers for a VOLUME, not for a folder inside one. Given any
// path, hand back the volume it belongs to. Without this, "df" run from
// /ext/clitest asked the firmware about /ext/clitest, which is not a volume, and
// got the whole generic "storage" usage text back instead of an answer.
QString cliVolumeOf(const QString &p)
{
    const QString s = QDir::cleanPath(p.isEmpty() ? QStringLiteral("/ext") : p);
    if (s.startsWith(QLatin1String("/int"), Qt::CaseInsensitive)) { return QStringLiteral("/int"); }
    if (s.startsWith(QLatin1String("/any"), Qt::CaseInsensitive)) { return QStringLiteral("/any"); }
    return QStringLiteral("/ext");
}

// One rule, so a single "cp" can serve both machines: "~..." and absolute paths
// outside /ext, /int, /any live on this computer. Everything else -- including
// every bare relative name -- belongs to the Flipper, because that's whose shell
// the prompt is.
bool cliIsHostPath(const QString &p)
{
    if (p.startsWith(QLatin1Char('~')))  { return true; }
    if (p.startsWith(QLatin1Char('/')))  { return !cliIsDevicePath(p); }
    return false;
}

// Resolve a Flipper-side argument against the current folder, so "cd nfc",
// "ls ..", "cat card.nfc" all behave the way a Linux shell would.
QString cliResolvePath(const QString &cwd, const QString &arg)
{
    if (arg.isEmpty() || arg == QLatin1String(".")) { return cwd; }
    if (arg == QLatin1String("~"))                  { return QStringLiteral("/ext"); }
    QString s = arg;
    if (s.startsWith(QLatin1String("~/")))  { s = QStringLiteral("/ext") + s.mid(1); }
    if (!s.startsWith(QLatin1Char('/')))    { s = cwd + QLatin1Char('/') + s; }
    s = QDir::cleanPath(s);
    return s.isEmpty() ? QStringLiteral("/") : s;
}

// The same Linux-style shortcuts the panel offers, for the one-shot path the
// assistant uses. send() does this inline, but that version is bound to panel
// state -- current folder, capture buffers, transfer chains -- and the assistant
// has no panel. So this is the subset that is genuinely one command in, one
// command out, with relative paths resolved against /ext. Multi-step commands
// (cp to/from this computer, find, rm -r, edit) stay panel-only, and anything
// unrecognised passes through untouched so raw firmware commands still work.
QString cliOneShotTranslate(const QString &cmd)
{
    const QStringList tokens = cliTokenize(cmd);
    if (tokens.isEmpty()) { return cmd; }

    const CliArgs args = cliParseArgs(tokens);
    const CliCommandDef *def = cliLookup(args.verb);
    if (!def) { return cmd.trimmed(); }

    const QString root = QStringLiteral("/ext");
    auto here = [&root](const QString &arg) { return cliResolvePath(root, arg); };
    const QString p0 = args.at(0);
    const QString p1 = args.at(1);

    switch (def->id) {
    case CliVerb::Ls:    return QStringLiteral("storage list ")  + here(p0);
    case CliVerb::Tree:  return QStringLiteral("storage tree ")  + here(p0);
    case CliVerb::Df:    return QStringLiteral("storage info ")  + cliVolumeOf(here(p0));
    case CliVerb::Cat:   return p0.isEmpty() ? cmd : QStringLiteral("storage read ")   + here(p0);
    case CliVerb::Stat:  return p0.isEmpty() ? cmd : QStringLiteral("storage stat ")   + here(p0);
    case CliVerb::Md5:   return p0.isEmpty() ? cmd : QStringLiteral("storage md5 ")    + here(p0);
    case CliVerb::Mkdir: return p0.isEmpty() ? cmd : QStringLiteral("storage mkdir ")  + here(p0);
    case CliVerb::Rm:    return p0.isEmpty() ? cmd : QStringLiteral("storage remove ") + here(p0);
    case CliVerb::Mv:
        return p1.isEmpty() ? cmd
                            : QStringLiteral("storage rename ") + here(p0) + QLatin1Char(' ') + here(p1);
    case CliVerb::Pwd:      return QStringLiteral("storage info /ext");
    case CliVerb::Whoami:
    case CliVerb::FWhoami:  return QStringLiteral("device_info");
    case CliVerb::Reboot:   return QStringLiteral("power reboot");
    case CliVerb::Shutdown: return QStringLiteral("power off");
    case CliVerb::Close:    return QStringLiteral("loader close");
    case CliVerb::Vibro:    return args.count() ? (QStringLiteral("vibro ") + p0)
                                                : QStringLiteral("vibro 1");
    case CliVerb::Open:
        return args.count() ? (QStringLiteral("loader open ") + args.positional.join(QLatin1Char(' ')))
                            : cmd.trimmed();
    default:
        break;
    }

    // Panel-only and host-only commands have no one-shot form: they either need
    // the panel's state (current folder, transfer chains, the editor) or they
    // run a program on this computer, which is not something to smuggle through
    // a wire translation. Everything else -- raw firmware commands -- passes
    // through untouched.
    return cmd.trimmed();
}

// Resolve a this-computer path. Relative names resolve against the panel's own
// host working folder (set with "lcd"), not blindly against $HOME -- otherwise
// "lcd ~/Desktop" followed by "cp shot.png /ext/" would still look in $HOME.
QString cliExpandHostPath(const QString &p, const QString &base = QString())
{
    const QString root = base.isEmpty() ? QDir::homePath() : base;
    QString s = p;
    if (s == QLatin1String("~"))           { return QDir::homePath(); }
    if (s.startsWith(QLatin1String("~/"))) { return QDir::cleanPath(QDir::homePath() + s.mid(1)); }
    if (!s.startsWith(QLatin1Char('/')))   { s = root + QLatin1Char('/') + s; }
    return QDir::cleanPath(s);
}

bool cliLooksLikeDir(const QString &dst)
{
    if (dst.endsWith(QLatin1Char('/'))) { return true; }
    return !dst.section(QLatin1Char('/'), -1).contains(QLatin1Char('.'));
}

QString cliJoinDest(const QString &dst, const QString &srcName, bool dstIsDir)
{
    QString d = dst;
    while (d.size() > 1 && d.endsWith(QLatin1Char('/'))) { d.chop(1); }
    return dstIsDir ? (d + QLatin1Char('/') + srcName) : d;
}

// One line of "storage tree": possibly tab-indented, then "[D] /abs/path" or
// "[F] /abs/path 1234b". Same row shape as "storage list" (see
// cliFormatListing below) except tree's path column is already absolute.
struct CliTreeEntry { QString path; bool isDir; qint64 size; };

QList<CliTreeEntry> cliParseTree(const QString &raw)
{
    static const QRegularExpression rowRe(QStringLiteral("\\[([DF])\\]\\s+(\\S+)(?:\\s+(\\d+)b)?"));
    QList<CliTreeEntry> out;
    const QStringList lines = raw.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const auto m = rowRe.match(line);
        if (!m.hasMatch()) { continue; }
        CliTreeEntry e;
        e.isDir = (m.captured(1) == QLatin1String("D"));
        e.path  = m.captured(2);
        e.size  = m.captured(3).isEmpty() ? -1 : m.captured(3).toLongLong();
        out += e;
    }
    return out;
}

// Pulls the first 32-hex-char token out of a "storage md5" reply.
QString cliExtractMd5(const QString &raw)
{
    static const QRegularExpression re(QStringLiteral("\\b[0-9a-fA-F]{32}\\b"));
    const auto m = re.match(raw);
    return m.hasMatch() ? m.captured(0).toLower() : QString();
}

// Two labelled sections, one shared column width. The firmware's own layout is
// discarded entirely so neither half can drift out of line with the other.
QString cliFormatHelp(const QString &raw, const QString &promptText)
{
    // Stock firmware prints "Commands available:", Unleashed/Momentum print
    // "Available commands:". Only the first was matched, so on a fork this
    // whole function bailed out on line one and the listing was passed through
    // untouched -- which is why cd, ls, grep and the rest were missing from it.
    static const QRegularExpression header(
        QStringLiteral("(?:commands available|available commands)\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch hm = header.match(raw);
    if (!hm.hasMatch()) { return raw; }
    const int hdr = hm.capturedStart();

    int listStart = raw.indexOf(QLatin1Char('\n'), hdr);
    listStart = (listStart < 0) ? hm.capturedEnd() : (listStart + 1);

    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QStringList stock;
    int listEnd = listStart;
    int pos = listStart;
    while (pos < raw.size()) {
        const int eol = raw.indexOf(QLatin1Char('\n'), pos);
        const int lineEnd = (eol < 0) ? raw.size() : eol;
        const QString line = raw.mid(pos, lineEnd - pos).trimmed();
        // Stop at the prompt. It is no longer ">: " -- we rewrite it -- and if
        // the parser runs past it, "Nikita@qflipper", "~" and "%" get sorted
        // into the command list and the prompt itself is eaten.
        if (line.isEmpty() || line.startsWith(QLatin1String(">:"))) { break; }
        if (!promptText.isEmpty() && line.startsWith(promptText)) { break; }
        // The firmware follows the grid with two sentences of prose ("If you
        // added a new external command...", "Find out more: <url>"). Without
        // this they were split on whitespace and sorted into the command list.
        // A command row is nothing but bare names.
        static const QRegularExpression nameRe(QStringLiteral("^[A-Za-z0-9_!?.-]{1,24}$"));
        const QStringList tokens = line.split(ws, Qt::SkipEmptyParts);
        bool allNames = !tokens.isEmpty();
        for (const QString &t : tokens) {
            if (!nameRe.match(t).hasMatch()) { allNames = false; break; }
        }
        if (!allNames) { break; }
        stock += tokens;
        if (eol < 0) { listEnd = raw.size(); break; }
        pos = eol + 1;
        listEnd = pos;
    }
    if (stock.isEmpty()) { return raw; }

    // Three lists, three meanings. Anything this CLI defines is removed from the
    // firmware's own listing first, so a name appears in exactly one section and
    // the reader is never left guessing which one is in charge.
    const QStringList ours    = cliOurNames();
    QStringList flipperCmds   = cliNamesIn(CliWhere::Flipper);
    QStringList deviceCmds    = cliNamesIn(CliWhere::Device) + cliNamesIn(CliWhere::Panel);
    for (const char *const n : kHostPassthrough) { deviceCmds += QLatin1String(n); }
    deviceCmds.removeDuplicates();
    std::sort(deviceCmds.begin(), deviceCmds.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    for (const QString &n : ours) { stock.removeAll(n); }
    stock.removeDuplicates();
    std::sort(stock.begin(), stock.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    int colW = 0;
    for (const QString &n : flipperCmds) { colW = qMax(colW, n.size()); }
    for (const QString &n : deviceCmds)  { colW = qMax(colW, n.size()); }
    for (const QString &n : stock)       { colW = qMax(colW, n.size()); }
    colW += 4;

    auto grid = [colW](const QStringList &names) {
        QString out;
        const int rows = (names.size() + 1) / 2;
        for (int r = 0; r < rows; ++r) {
            QString row = names.at(r);
            const int right = r + rows;
            if (right < names.size()) {
                while (row.size() < colW) { row += QLatin1Char(' '); }
                row += names.at(right);
            }
            out += row + QLatin1Char('\n');
        }
        return out;
    };

    QString block;
    block += QStringLiteral("  (help <command> for details)\n");
    block += QStringLiteral("  (f-prefixed acts on the Flipper, bare acts on this computer)\n");
    block += QStringLiteral("------ Flipper ------\n");
    block += grid(flipperCmds);
    block += QStringLiteral("------ Computer ------\n");
    block += grid(deviceCmds);
    // Listed above, but only usable if the program is installed here.
    {
        const QString missing = cliMissingPassthrough();
        if (!missing.isEmpty()) {
            block += QStringLiteral("  (not installed on this computer: ") + missing
                   + QStringLiteral(")\n");
        }
    }
    block += QStringLiteral("------ Firmware ------\n");
    block += grid(stock);

    QString out = raw;
    out.replace(listStart, listEnd - listStart, block);

    // Drop the firmware's two trailing sentences. "reload_ext_cmds" is still in
    // the listing above for anyone who needs it, and the docs link is already a
    // line of the banner on connect -- repeating both after every `help` is
    // noise in a panel this size.
    static const QRegularExpression trailer(
        QStringLiteral("(?m)^[ \\t]*(?:If you added a new external command.*|Find out more:.*)$\\n?"));
    out.remove(trailer);
    // A colour reset that arrived split across two reads leaves its tail behind
    // as literal text.
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\[[0-9;]*m$\\n?")));
    out.replace(QRegularExpression(QStringLiteral("\\[[0-9;]*m")), QString());
    return out;
}

// Reformat "storage list" into something closer to ls: folders first with a
// trailing slash, files after them, sizes right-aligned and human readable. No
// type column -- the slash already says which is which, and a bare "-" is only
// half of the "ls -l" convention it borrows from.
QString cliFormatListing(const QString &raw)
{
    static const QRegularExpression rowRe(
        QStringLiteral("^\\[([DF])\\]\\s+(.*?)(?:\\s+(\\d+)b)?$"));

    const QStringList lines = raw.split(QLatin1Char('\n'));
    QStringList dirs, files, sizes, passthrough;
    QString head, tail;
    bool sawRow = false;

    for (const QString &line : lines) {
        const QString t = line.trimmed();
        const auto m = rowRe.match(t);
        if (m.hasMatch()) {
            sawRow = true;
            if (m.captured(1) == QLatin1String("D")) {
                dirs += m.captured(2);
            } else {
                files += m.captured(2);
                sizes += m.captured(3);
            }
        } else if (!sawRow) {
            head += line + QLatin1Char('\n');
        } else {
            tail += line + QLatin1Char('\n');
        }
    }
    if (!sawRow) { return raw; }

    // Keep each file glued to its size while sorting.
    QList<QPair<QString, QString>> fileRows;
    for (int i = 0; i < files.size(); ++i) { fileRows.append({ files.at(i), sizes.value(i) }); }
    auto byName = [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(fileRows.begin(), fileRows.end(),
              [&byName](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
                  return byName(a.first, b.first);
              });

    auto human = [](const QString &bytes) {
        if (bytes.isEmpty()) { return QString(); }
        const qint64 n = bytes.toLongLong();
        if (n < 1024)             { return QStringLiteral("%1 B").arg(n); }
        if (n < 1024LL * 1024)    { return QStringLiteral("%1 KB").arg(n / 1024.0, 0, 'f', 1); }
        if (n < 1024LL * 1024 * 1024) { return QStringLiteral("%1 MB").arg(n / (1024.0 * 1024), 0, 'f', 1); }
        return QStringLiteral("%1 GB").arg(n / (1024.0 * 1024 * 1024), 0, 'f', 1);
    };

    int nameW = 0;
    for (const QString &d : dirs) { nameW = qMax(nameW, d.size() + 1); }   // +1 for the trailing slash
    for (const auto &f : fileRows) { nameW = qMax(nameW, f.first.size()); }
    nameW += 2;

    int sizeW = 0;
    for (const auto &f : fileRows) { sizeW = qMax(sizeW, human(f.second).size()); }

    QString out = head;
    for (const QString &d : dirs) {
        out += d + QLatin1Char('/') + QLatin1Char('\n');
    }
    for (const auto &f : fileRows) {
        QString row = f.first;
        const QString sz = human(f.second);
        if (!sz.isEmpty()) {
            while (row.size() < nameW) { row += QLatin1Char(' '); }
            QString pad = sz;
            while (pad.size() < sizeW) { pad.prepend(QLatin1Char(' ')); }
            row += pad;
        }
        out += row + QLatin1Char('\n');
    }
    out += tail;
    // split() handed us the trailing prompt as its own element and the loop
    // above put a newline after it, which the raw text never had. That stray
    // newline is what dropped the caret onto the line below the prompt.
    if (!raw.endsWith(QLatin1Char('\n')) && out.endsWith(QLatin1Char('\n'))) { out.chop(1); }
    return out;
}

// ---- prompt ----------------------------------------------------------------
// "Nikita@qflipper ~/nfc % " -- the folder shows in the prompt, the way a shell
// does, so cd doesn't need to announce itself.
// The prompt has to answer "where does a bare command land", because that is
// now a real question with two possible answers. The path shown is the one
// bare commands use -- this computer's -- and the Flipper's folder is shown in
// brackets only when it has moved off /ext, so the common case stays short.
QString cliPromptFor(const QString &devName, const QString &cwd, const QString &hostCwd)
{
    QString fwhere = cwd;
    if (fwhere == QLatin1String("/ext"))                { fwhere = QStringLiteral("~"); }
    else if (fwhere.startsWith(QLatin1String("/ext/"))) { fwhere = QStringLiteral("~") + fwhere.mid(4); }

    QString hwhere = hostCwd;
    const QString home = QDir::homePath();
    if (hwhere == home)                 { hwhere = QStringLiteral("~"); }
    else if (hwhere.startsWith(home + QLatin1Char('/'))) { hwhere = QStringLiteral("~") + hwhere.mid(home.size()); }
    if (hwhere.isEmpty())               { hwhere = QStringLiteral("~"); }

    const QString who = devName.isEmpty() ? QStringLiteral("flipper") : devName;
    const QString flip = (cwd == QLatin1String("/ext")) ? QString()
                                                        : QStringLiteral("[f:%1]").arg(fwhere);
    // A bare "%" is safe here: QString::arg only consumes % followed by a digit.
    return QStringLiteral("%1@qflipper %2%3 % ").arg(who, hwhere, flip);
}

// ---- file transfer, as pure steps so the protocol can be exercised offline ---
struct CliXferStep {
    bool done = false;
    bool failed = false;
    QByteArray toWrite;   // raw bytes to push at the port
    QByteArray body;      // downloaded contents (download only)
    QString message;      // line to show the user
};

// storage write_chunk <path> <n> answers "Ready", then swallows exactly n bytes.
CliXferStep cliUploadFeed(QByteArray &raw, const QByteArray &chunk,
                          const QByteArray &payload, const QString &label)
{
    CliXferStep r;
    raw += chunk;
    if (raw.contains("Ready")) {
        r.done = true;
        r.toWrite = payload;
        r.message = QStringLiteral("[ sent %1 bytes -> %2 ]").arg(payload.size()).arg(label);
        raw.clear();
    } else if (raw.contains("Storage error") || raw.contains("Usage:") || raw.size() > 4096) {
        // Deliberately NOT a bare contains("error"): that also matched the
        // firmware's echo of a path like /ext/logs/error.txt, and -- worse --
        // any stray reply from a previous command that leaked into this
        // buffer, which aborted a perfectly good upload while the firmware was
        // already sitting in write_chunk's Ready state.
        r.done = true;
        r.failed = true;
        const QString reply = QString::fromUtf8(raw).trimmed();
        // "file/dir not exist" on a write_chunk means the PARENT folder is
        // gone -- write_chunk creates the file itself. Echoing the raw reply
        // (command line and all) buried that behind text that read like the
        // file was expected to already be there.
        if (reply.contains(QLatin1String("file/dir not exist"))) {
            const QString dir = label.section(QLatin1Char('/'), 0, -2);
            r.message = QStringLiteral("[ can't write %1: the folder %2 doesn't exist -- mkdir -p %2 first ]")
                            .arg(label, dir.isEmpty() ? QStringLiteral("/ext") : dir);
        } else {
            r.message = QStringLiteral("[ upload refused: %1 ]").arg(reply.section(QLatin1Char('\n'), -1).trimmed());
        }
        raw.clear();
    }
    return r;
}

// storage read <path> prints "Size: n" and then the raw contents.
CliXferStep cliDownloadFeed(QByteArray &raw, qint64 &size, const QByteArray &chunk,
                            const QString &hostDst)
{
    CliXferStep r;
    raw += chunk;
    if (size < 0) {
        const int c = raw.indexOf("Size:");
        if (c < 0) {
            if (raw.contains("Storage error") || raw.size() > 4096) {
                r.done = true;
                r.failed = true;
                // Just the error, not the command echo and the prompt around
                // it: "[ download failed: storage read /ext/a.txt \n Storage
                // error: file/dir not exist \n\n >: ]" made a one-line problem
                // look like a malfunction.
                QString why = QString::fromUtf8(raw);
                const int e = why.indexOf(QLatin1String("Storage error"));
                why = (e >= 0) ? why.mid(e).section(QLatin1Char('\n'), 0, 0).trimmed()
                               : why.trimmed();
                r.message = QStringLiteral("[ can't read %1: %2 ]").arg(hostDst, why);
                raw.clear();
            }
            return r;
        }
        const int nl = raw.indexOf('\n', c);
        if (nl < 0) { return r; }
        size = raw.mid(c + 5, nl - c - 5).trimmed().toLongLong();
        raw = raw.mid(nl + 1);
    }
    // The text console sends CRLF for every newline the file contains, so the
    // bytes on the wire always outnumber the bytes in the file. Measuring the
    // raw stream meant the read was declared finished while the tail of the
    // body -- and the prompt after it -- were still coming, and left(size) then
    // returned a truncated body. Every single download failed its md5 that way.
    //
    // Worse than the wrong file: the leftovers stayed in the port. The md5
    // check issued next read THEM instead of its own reply, and the real md5
    // answer arrived with nothing waiting for it and was printed as raw text in
    // the middle of whatever came after. That is the whole cascade of spliced
    // half-commands, from one off-by-CRLF.
    QByteArray norm = raw;
    norm.replace("\r\n", "\n");
    if (norm.size() < size) { return r; }

    // Wait for the prompt too, and only look for it past the body -- a file may
    // legitimately contain ">:" of its own. Nothing may be left in the port when
    // this returns.
    const int pmt = norm.indexOf(">:", int(size));
    if (pmt < 0) { return r; }

    r.done = true;
    r.body = norm.left(int(size));
    r.message = QStringLiteral("[ saved %1 bytes -> %2 ]").arg(r.body.size()).arg(hostDst);
    raw.clear();
    return r;
}

}   // namespace

// Words that mean "this message is about the device or the shell", built from
// the command table rather than typed out. A hand-written copy is exactly the
// kind of thing that silently stops matching the real command set.
static QStringList nikitaCliIntentWords()
{
    QStringList w{
        QStringLiteral("storage"), QStringLiteral("terminal"), QStringLiteral("shell"),
        QStringLiteral("command line"), QStringLiteral("run_cli"), QStringLiteral("cli"),
    };
    for (const CliCommandDef &c : kCliCommands) {
        // Panel-only commands (help, clear, colors) are not device work, and
        // matching them would drag ordinary conversation into tool mode.
        if (c.where == CliWhere::Panel) { continue; }
        if (*c.name)  { w += QLatin1String(c.name); }
        if (*c.fname) { w += QLatin1String(c.fname); }
    }
    w.removeDuplicates();
    return w;
}


// The CLI's command reference, written out from the command table itself rather
// than typed into the prompt by hand. A prompt that lists commands in prose
// goes stale the first time one is added, and a model taught a command that no
// longer exists produces confident nonsense -- so this is generated at runtime
// from the same rows the dispatcher switches on. Add a command, the assistant
// knows it; there is no second place to update.
QString NikitaBackend::cliReferenceForPrompt()
{
    QStringList flipper, device;
    for (const CliCommandDef &c : kCliCommands) {
        const bool both = (c.where == CliWhere::Either);
        if (*c.fname && (both || c.where == CliWhere::Flipper)) { flipper += QLatin1String(c.fname); }
        if (*c.name  && (both || c.where == CliWhere::Device))  { device  += QLatin1String(c.name);  }
    }
    std::sort(flipper.begin(), flipper.end());
    std::sort(device.begin(), device.end());

    QString out;
    out += QStringLiteral(
        "\n\nTHE CLI PANEL -- what the user sees when they open the terminal in this app.\n"
        "It is a two-machine shell, and the command NAME says which machine:\n"
        "- f-prefixed commands act on the FLIPPER: %1\n"
        "- bare commands act on THIS COMPUTER, and are the real Unix programs: %2\n"
        "- everything else is the firmware's own command set (device_info, info, gpio, "
        "subghz, nfc, rfid, ir, led, loader, storage, power, top, log, js, ...), unchanged.\n"
        "- cp, wget and edit take a path on either machine and work out the direction "
        "themselves, so they have no prefix.\n"
        "- 'help' lists all three groups; 'help <command>' explains one.\n"
        "When you TELL the user which command to type, use these exact names. Telling a "
        "user to type 'ls /ext/nfc' is wrong -- bare ls is their computer; the Flipper's "
        "is 'fls /ext/nfc'.\n")
        .arg(flipper.join(QStringLiteral(", ")), device.join(QStringLiteral(", ")));
    return out;
}


FlipperCli::FlipperCli(QObject *parent)
    : QObject(parent)
{
    // History outlives the panel, so "!12" is still useful for exactly the long
    // commands worth repeating.
    loadHistory();
}

void FlipperCli::setOpen(bool value)
{
    if (m_open == value) { return; }

    // The CLI and the RPC session share one serial port, so connecting here
    // tears down whatever RPC work is in flight. A backup is minutes of
    // transfers; letting a click destroy it -- and only finding out from a
    // "session was stopped" line in the log -- is not a fair trade.
    if (value && g_transferRunning) {
        setStatus(QStringLiteral("A backup is running -- the CLI would cut its connection."));
        emit openChanged();      // let the UI snap the toggle back
        return;
    }

    m_open = value;
    emit openChanged();

    if (m_open) { connectCli(); }
    else        { disconnectCli(); }
}

void FlipperCli::connectCli()
{
    clearOutput();

    if (!m_appBackend) { setStatus(QStringLiteral("Backend unavailable.")); return; }

    auto *reg = m_appBackend->deviceRegistry();
    auto *dev = reg ? reg->currentDevice() : nullptr;
    if (!dev) {
        setStatus(QStringLiteral("Connect a Flipper over USB first."));
        return;
    }

    const auto &info = dev->deviceState()->deviceInfo();
    if (info.isBle || info.portInfo.isNull()) {
        setStatus(QStringLiteral("CLI is USB only."));
        return;
    }
    const QSerialPortInfo portInfo = info.portInfo;
    // Name for the prompt: the device's own name, or the one baked into the
    // serial port (…usbmodemflip_Nikita1) if the field comes back empty.
    QString devName = info.name;
    if (devName.isEmpty()) {
        const QString pn = portInfo.portName();
        const int f = pn.indexOf(QLatin1String("flip_"));
        if (f >= 0) {
            devName = pn.mid(f + 5);
            while (!devName.isEmpty() && devName.back().isDigit()) { devName.chop(1); }
        }
    }

    // Hand the serial line off from RPC to us: releasePort() stops the RPC
    // session, which closes the COM port and drops the Flipper back to its CLI.
    // Deliberately no status here. This placeholder only ever showed in the
    // gap before the Flipper's own banner arrived, so all it did was put a
    // sentence on screen and take it away again a moment later. An empty
    // terminal for that moment reads better than narration.
    setStatus(QString());
    // (device session pauses silently while the CLI is open)
    m_appBackend->releasePort();

    // Give the RPC teardown a moment to actually free the port, then take it over.
    QTimer::singleShot(700, this, [this, portInfo, devName]() {
        if (!m_open) { return; }   // user closed the CLI again before we got here
        openPort(portInfo, devName);
    });
}

void FlipperCli::openPort(const QSerialPortInfo &portInfo, const QString &devName)
{
    m_port = new QSerialPort(portInfo, this);
    m_port->setBaudRate(230400);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        appendOutput(QStringLiteral("[ couldn't open %1: %2 ]\n").arg(portInfo.portName(), m_port->errorString()));
        setStatus(QStringLiteral("Couldn't open the port -- close and retry."));
        m_port->deleteLater();
        m_port = nullptr;
        return;
    }

    connect(m_port, &QSerialPort::readyRead, this, &FlipperCli::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred, this, &FlipperCli::onPortError);
    m_cwd = QStringLiteral("/ext");
    m_devName = devName;
    emit promptChanged();
    m_cdPrev = m_cwd;
    m_cdPending.clear();
    m_cdRaw.clear();
    m_xfer = Xfer::None;
    m_rawCb = nullptr;
    m_xferChain = nullptr;
    m_capture = Capture::None;
    m_captureBuf.clear();
    m_echoPending.clear();
    m_escTail.clear();
    setActive(true);
    // Same reason: the firmware's own banner already says "Run `help` or `?` to
    // list available commands", so this said it first and worse.
    setStatus(QString());

    // The firmware prints its banner and a prompt by itself the moment the
    // CDC port is opened. Writing "\r\n" here unconditionally asked it for a
    // second one, which is the duplicate "name@qflipper ~ %" line sitting
    // above the caret on every connect. Nudge only if it stayed silent --
    // which does happen when the device was already parked at a prompt and
    // has nothing new to announce.
    m_sawDeviceBytes = false;
    QTimer::singleShot(700, this, [this]() {
        if (m_port && m_active && !m_sawDeviceBytes) { m_port->write("\r\n"); }
    });
}

void FlipperCli::onPortError(QSerialPort::SerialPortError err)
{
    // ResourceError/DeviceNotFoundError is Qt's way of saying the device just
    // went away underneath us -- exactly what a reboot or a rename (which
    // re-enumerates under a new serial path) does. Other codes fire for
    // things that aren't a disconnect (e.g. a stray write timeout) and would
    // false-trigger a reconnect the link doesn't need.
    if (err != QSerialPort::ResourceError && err != QSerialPort::DeviceNotFoundError) { return; }
    if (!m_open || m_reconnecting) { return; }
    beginReconnect();
}

void FlipperCli::beginReconnect()
{
    m_reconnecting = true;
    setActive(false);
    setStatus(QStringLiteral("Flipper disconnected -- waiting for it to come back…"));
    appendOutput(QStringLiteral("\n[ link dropped -- probably a reboot or rename, reconnecting… ]\n"));

    if (m_port) {
        m_port->disconnect(this);
        m_port->deleteLater();
        m_port = nullptr;
    }
    resetTransientState();

    if (!m_reconnectPoll) {
        m_reconnectPoll = new QTimer(this);
        m_reconnectPoll->setInterval(800);
        connect(m_reconnectPoll, &QTimer::timeout, this, &FlipperCli::pollReconnect);
    }
    m_reconnectElapsedMs = 0;
    m_reconnectPoll->start();
}

void FlipperCli::pollReconnect()
{
    if (!m_open) {   // the user closed the panel while it was reconnecting
        m_reconnectPoll->stop();
        m_reconnecting = false;
        return;
    }

    m_reconnectElapsedMs += m_reconnectPoll->interval();
    // Same ballpark as RestartOperation's own post-reboot reconnect budget
    // (see restartoperation.cpp) -- long enough for macOS to release the old
    // port and the firmware to re-enumerate, short enough to eventually admit
    // the device isn't coming back on its own.
    static const int kReconnectTimeoutMs = 45000;
    if (m_reconnectElapsedMs > kReconnectTimeoutMs) {
        m_reconnectPoll->stop();
        m_reconnecting = false;
        setStatus(QStringLiteral("Flipper didn't come back -- close and reopen the CLI to retry."));
        appendOutput(QStringLiteral("[ gave up waiting -- close and reopen the CLI panel ]\n"));
        return;
    }

    auto *reg = m_appBackend ? m_appBackend->deviceRegistry() : nullptr;
    auto *dev = reg ? reg->currentDevice() : nullptr;
    if (!dev) { return; }   // still gone

    const auto &info = dev->deviceState()->deviceInfo();
    if (info.isBle || info.portInfo.isNull()) { return; }   // not fully back yet

    m_reconnectPoll->stop();
    m_reconnecting = false;

    QString devName = info.name;
    if (devName.isEmpty()) {
        const QString pn = info.portInfo.portName();
        const int f = pn.indexOf(QLatin1String("flip_"));
        if (f >= 0) {
            devName = pn.mid(f + 5);
            while (!devName.isEmpty() && devName.back().isDigit()) { devName.chop(1); }
        }
    }

    appendOutput(QStringLiteral("[ %1 is back -- reconnecting the CLI… ]\n")
                 .arg(devName.isEmpty() ? info.portInfo.portName() : devName));

    // Same hand-off connectCli() does: make sure RPC isn't sitting on the new
    // port before the panel grabs it.
    if (m_appBackend) { m_appBackend->releasePort(); }
    const QSerialPortInfo portInfo = info.portInfo;
    QTimer::singleShot(700, this, [this, portInfo, devName]() {
        if (!m_open) { return; }
        openPort(portInfo, devName);
    });
}

void FlipperCli::disconnectCli()
{
    // Whatever is still queued was queued for a link that no longer exists.
    // Leaving it there means it fires at whatever gets connected next.
    m_pending.clear();
    if (m_queueStall) { m_queueStall->stop(); }
    if (m_reconnectPoll) { m_reconnectPoll->stop(); }
    m_reconnecting = false;
    resetTransientState();
    if (m_port) {
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
    }
    setActive(false);
    setStatus(QString());

    // Hand the line back to qFlipper's normal RPC session.
    if (m_appBackend) { m_appBackend->reacquirePort(); }
}

// ---- one-shot CLI run for the assistant (isolated from the interactive panel) --
void FlipperCli::runOneShot(const QString &cmd, std::function<void(bool, QString)> done)
{
    if (m_open || m_active) {
        // Actionable, because the model cannot close this panel itself and was
        // left retrying a command that could never run.
        done(false, QStringLiteral("The in-app CLI panel is open and owns the session, so run_cli "
                                   "cannot be used right now. Either ask the user to close the CLI "
                                   "panel, or do this with press_button navigation instead."));
        return;
    }
    if (m_runBusy)          { done(false, QStringLiteral("A CLI command is already running.")); return; }
    if (!m_appBackend)      { done(false, QStringLiteral("Backend unavailable.")); return; }

    auto *reg = m_appBackend->deviceRegistry();
    auto *dev = reg ? reg->currentDevice() : nullptr;
    if (!dev) { done(false, QStringLiteral("Connect a Flipper over USB first.")); return; }
    const auto &info = dev->deviceState()->deviceInfo();
    if (info.isBle || info.portInfo.isNull()) { done(false, QStringLiteral("CLI is USB-only.")); return; }
    const QSerialPortInfo portInfo = info.portInfo;

    // The assistant is told it can type "ls /ext/nfc"; make that true. Raw
    // firmware commands are returned unchanged, so both spellings work.
    const QString wire = cliOneShotTranslate(cmd);

    m_runBusy = true;
    m_runBuf.clear();
    m_runDone = std::move(done);
    // A reboot/power/shutdown drops the USB link on purpose; anything else that
    // makes the link vanish mid-command is the firmware crashing.
    {
        const QString low = cmd.toLower();
        m_runRebootExpected = low.contains(QLatin1String("reboot"))
                           || low.contains(QLatin1String("shutdown"))
                           || low.contains(QLatin1String("power off"))
                           || low.contains(QLatin1String("power reboot"));
    }

    // Idle timer: once output stops arriving for a beat, the command is done.
    if (!m_runIdle) {
        m_runIdle = new QTimer(this);
        m_runIdle->setSingleShot(true);
        m_runIdle->setInterval(700);
        connect(m_runIdle, &QTimer::timeout, this, [this]() { finishOneShot(true, m_runBuf); });
    }
    // Hard guard: never hang forever.
    if (!m_runGuard) {
        m_runGuard = new QTimer(this);
        m_runGuard->setSingleShot(true);
        m_runGuard->setInterval(6000);
        connect(m_runGuard, &QTimer::timeout, this, [this]() { finishOneShot(true, m_runBuf); });
    }

    // Release RPC, wait for the port to free, then take it over briefly.
    m_appBackend->releasePort();
    QTimer::singleShot(700, this, [this, portInfo, wire]() {
        if (!m_runBusy) { return; }
        m_runPort = new QSerialPort(portInfo, this);
        m_runPort->setBaudRate(230400);
        m_runPort->setDataBits(QSerialPort::Data8);
        m_runPort->setParity(QSerialPort::NoParity);
        m_runPort->setStopBits(QSerialPort::OneStop);
        m_runPort->setFlowControl(QSerialPort::NoFlowControl);
        if (!m_runPort->open(QIODevice::ReadWrite)) {
            const QString err = m_runPort->errorString();
            m_runPort->deleteLater(); m_runPort = nullptr;
            finishOneShot(false, QStringLiteral("Couldn't open the port: %1").arg(err));
            return;
        }
        // The link dropping mid-command (device rebooted) is a crash unless we
        // asked it to reboot. QSerialPort reports it as ResourceError.
        connect(m_runPort, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError && m_runBusy && !m_runRebootExpected) {
                finishOneShot(false, QStringLiteral("The Flipper crashed running this command and "
                    "rebooted. The command did not complete -- do not report success. Try "
                    "navigating by button or a safer command instead."));
            }
        });
        connect(m_runPort, &QSerialPort::readyRead, this, [this]() {
            if (!m_runPort) { return; }
            QString chunk = QString::fromUtf8(m_runPort->readAll());
            static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]"));
            chunk.remove(ansi);
            chunk.remove(QLatin1Char('\r'));
            static const QRegularExpression ctrl(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]"));
            chunk.remove(ctrl);
            m_runBuf += chunk;
            if (m_runIdle) { m_runIdle->start(); }   // reset idle countdown
        });
        m_runGuard->start();
        m_runPort->write(wire.toUtf8());
        m_runPort->write("\r\n");
        m_runIdle->start();
    });
}

void FlipperCli::finishOneShot(bool ok, const QString &out)
{
    if (!m_runBusy) { return; }
    if (m_runIdle)  { m_runIdle->stop(); }
    if (m_runGuard) { m_runGuard->stop(); }
    if (m_runPort) {
        m_runPort->close();
        m_runPort->deleteLater();
        m_runPort = nullptr;
    }
    m_runBusy = false;

    // Tidy the captured text: drop the echoed command line and the trailing prompt.
    QString text = out;
    text.remove(QRegularExpression(QStringLiteral("(^|\\n)>: *")));   // prompt lines
    text = text.trimmed();

    // A firmware fault printed to the console before the reboot. If any of these
    // are in the output, the command crashed the Flipper -- report failure even
    // though the CLI "returned", so the assistant does not claim success on a
    // command that just rebooted the device. "Rebooting" alone is NOT here: a
    // deliberate `reboot` prints it and is not a crash.
    static const QRegularExpression faultRe(QStringLiteral(
        "furi_crash|NULL pointer dereference|HardFault|MemManage|BusFault|"
        "UsageFault|assert(ion)? failed|\\[CRASH\\]|stack overflow|fault_handler"),
        QRegularExpression::CaseInsensitiveOption);
    if (ok && faultRe.match(text).hasMatch()) {
        ok = false;
        // Clean, single line -- the captured framebuffer/banner is noise here,
        // and the user watching should read a plain "it crashed", not a wall of
        // ASCII art. The model needs the fact, not the dump.
        text = QStringLiteral("The Flipper crashed running this command and rebooted. The command "
                              "did not complete -- do not report success. Try navigating by button "
                              "or a safer command instead.");
    }

    auto cb = m_runDone;
    m_runDone = nullptr;
    if (m_appBackend) { m_appBackend->reacquirePort(); }   // hand the line back to RPC
    if (cb) { cb(ok, text); }
}

// ---- entry point -----------------------------------------------------------
// send() does three things and nothing else: it makes sure exactly one line at
// a time reaches the dispatcher, it records and expands history, and it hands
// the parsed line over. Every decision about what a command *means* lives in
// dispatch() below, driven by the command table.
void FlipperCli::send(const QString &cmd)
{
    // A live interactive session (python3 with no script, started by
    // startInteractiveSession()) owns the panel: every line is relayed
    // straight to its stdin, verbatim, instead of reaching the dispatcher
    // below at all -- exactly what a real terminal does once a program has
    // taken over the foreground. A multi-line paste arrives as one string
    // with embedded newlines; writing it in one call is fine, the child
    // reads it as separate lines the same as if they'd been typed one at a
    // time. The session ends on its own (the program's own exit()/quit()/EOF,
    // or a crash) -- see the finished handler in startInteractiveSession().
    if (m_interactiveProc) {
        appendOutput(cmd + QLatin1Char('\n'));
        m_interactiveProc->write(cmd.toUtf8() + "\n");
        return;
    }

    // Depending on how the panel forwards a paste, the whole block can arrive
    // as a single string with newlines in it. Sent as-is that becomes one
    // enormous "command" on the wire. Split here so everything below is always
    // dealing with exactly one line, whatever feeds it.
    if (cmd.contains(QLatin1Char('\n')) || cmd.contains(QLatin1Char('\r'))) {
        static const QRegularExpression lineSep(QStringLiteral("[\r\n]+"));
        const QStringList lines = cmd.split(lineSep, Qt::SkipEmptyParts);
        for (const QString &l : lines) {
            if (!l.trimmed().isEmpty()) { queuePending(l.trimmed()); }
        }
        drainPending();
        return;
    }

    // One conversation at a time. Every mode below owns the serial line
    // exclusively, so a command that arrives mid-operation would interleave its
    // reply with the one already in flight.
    //
    // Held, not bounced. A pasted block arrives far faster than the firmware
    // can answer, and rejecting the overflow meant most of the block silently
    // never ran. Queuing here also means correctness no longer depends on the
    // panel pacing itself.
    //
    // This sits above the panel-local commands on purpose. They never touch the
    // port, but they do print into the same view, and a local echo landing
    // mid-capture is what spliced a typed command into the middle of the
    // firmware's help listing.
    if (busy()) {
        queuePending(cmd);
        return;
    }

    QString line = cmd.trimmed();
    if (line.isEmpty()) { appendOutput(prompt()); return; }

    // "!12" re-runs entry 12, "!!" repeats the last command, "!ls" repeats the
    // most recent line starting with "ls". Resolved before anything is recorded
    // so history never fills up with the shorthand instead of the command.
    if (line.startsWith(QLatin1Char('!')) && line != QLatin1String("!")) {
        QString expanded;
        if (!expandHistory(line, &expanded)) {
            appendOutput(line + QStringLiteral("\n[ %1: no such entry in history ]\n").arg(line) + prompt());
            return;
        }
        appendOutput(line + QStringLiteral("\n") + expanded + QLatin1Char('\n'));
        line = expanded;
    }

    m_lastTyped = line;
    if (m_history.value(m_history.size() - 1) != line) {
        m_history += line;
        if (m_history.size() > kCliHistoryMax) { m_history.removeFirst(); }
        saveHistory();
    }

    bool unterminated = false;
    const QStringList tokens = cliTokenize(line, &unterminated);
    if (unterminated) {
        appendOutput(line + QStringLiteral(
            "\n[ unbalanced quote -- close it, or escape the quote with \\ ]\n") + prompt());
        return;
    }
    if (tokens.isEmpty()) { appendOutput(prompt()); return; }

    dispatch(line, tokens);
}

// "!12", "!!", "!ls" -> the command they stand for.
bool FlipperCli::expandHistory(const QString &token, QString *out) const
{
    if (!out) { return false; }
    const QString body = token.mid(1);
    if (body == QLatin1String("!")) {
        if (m_history.isEmpty()) { return false; }
        *out = m_history.last();
        return true;
    }
    bool isNum = false;
    const int n = body.toInt(&isNum);
    if (isNum) {
        if (n < 1 || n > m_history.size()) { return false; }
        *out = m_history.at(n - 1);
        return true;
    }
    for (int i = m_history.size() - 1; i >= 0; --i) {
        if (m_history.at(i).startsWith(body)) { *out = m_history.at(i); return true; }
    }
    return false;
}

// ---- history on disk -------------------------------------------------------
// A session's history used to die with the panel, which made "!12" useless for
// exactly the long commands worth repeating.
QString FlipperCli::historyPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/cli-history.txt");
}

void FlipperCli::loadHistory()
{
    QFile f(historyPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { return; }
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    m_history = lines.mid(qMax(0, lines.size() - kCliHistoryMax));
}

void FlipperCli::saveHistory() const
{
    QFile f(historyPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { return; }
    f.write(m_history.join(QLatin1Char('\n')).toUtf8());
    f.write("\n");
}

// ---- dispatch --------------------------------------------------------------
// A single switch over the command table's ids. There is deliberately no
// `default:` label: adding a row to kCliCommands without handling its id is a
// -Wswitch warning at compile time, which is the whole reason the table and the
// behaviour cannot drift apart.
void FlipperCli::dispatch(const QString &line, const QStringList &tokens)
{
    const CliArgs args = cliParseArgs(tokens);
    const CliCommandDef *def = cliLookup(args.verb);

    auto echo    = [](const QString &c) { return c.trimmed() + QLatin1Char('\n'); };
    auto usage   = [this, line](const CliCommandDef *d) {
        appendOutput(line + QStringLiteral("\n[ usage: %1 ]\n")
                     .arg(QLatin1String(d && *d->usage ? d->usage : "")) + prompt());
    };
    auto here    = [this](const QString &arg) { return cliResolvePath(m_cwd, arg); };
    auto lhere   = [this](const QString &arg) { return cliExpandHostPath(arg, m_hostCwd); };
    auto needs   = [&args, &usage, def](int n) {
        if (args.count() >= n) { return true; }
        usage(def);
        return false;
    };

    // ---- panel-local commands ---------------------------------------------
    // Handled before the connection check: they are answers this panel gives,
    // and refusing them because a Flipper is unplugged makes no sense.
    if (def) {
        switch (def->id) {
        case CliVerb::Clear:
            clearOutput();
            appendOutput(prompt());
            return;

        case CliVerb::Colors: {
            const QString arg = args.at(0).toLower();
            if (arg.isEmpty())                                                 { setColored(!m_colored); }
            else if (arg == QLatin1String("on")  || arg == QLatin1String("1")) { setColored(true); }
            else if (arg == QLatin1String("off") || arg == QLatin1String("0")) { setColored(false); }
            else { usage(def); return; }
            appendOutput(echo(line) + (m_colored ? QStringLiteral("[ colors on ]\n")
                                                 : QStringLiteral("[ colors off ]\n")) + prompt());
            return;
        }

        case CliVerb::Verbose: {
            const QString arg = args.at(0).toLower();
            if (arg.isEmpty())                                                 { setVerbose(!m_verbose); }
            else if (arg == QLatin1String("on")  || arg == QLatin1String("1")) { setVerbose(true); }
            else if (arg == QLatin1String("off") || arg == QLatin1String("0")) { setVerbose(false); }
            else { usage(def); return; }
            appendOutput(echo(line)
                         + (m_verbose ? QStringLiteral("[ verbose on -- logging every step ]\n")
                                      : QStringLiteral("[ verbose off -- results only ]\n")) + prompt());
            return;
        }

        case CliVerb::History: {
            appendOutput(echo(line));
            if (args.has("c")) {
                m_history.clear();
                saveHistory();
                appendOutput(QStringLiteral("[ history cleared ]\n") + prompt());
                return;
            }
            if (m_history.isEmpty()) {
                appendOutput(QStringLiteral("[ nothing yet ]\n") + prompt());
                return;
            }
            QStringList rows;
            for (int i = 0; i < m_history.size(); ++i) {
                rows += QStringLiteral("%1  %2").arg(i + 1, 4).arg(m_history.at(i));
            }
            appendOutput(rows.join(QLatin1Char('\n')) + QLatin1Char('\n') + prompt());
            return;
        }

        case CliVerb::Pwd:
            // fpwd is the Flipper's and lives with the device commands below.
            if (!args.flipper) {
                appendOutput(echo(line) + m_hostCwd + QLatin1Char('\n') + prompt());
                return;
            }
            break;

        case CliVerb::Cd: {
            if (args.flipper) { break; }   // fcd moves the Flipper's folder, below
            const QString target = args.count() ? lhere(args.at(0)) : QDir::homePath();
            const QFileInfo fi(target);
            if (!fi.isDir()) {
                appendOutput(echo(line)
                             + QStringLiteral("[ no such folder on this computer: %1 ]\n").arg(target)
                             + prompt());
                return;
            }
            m_hostCwd = fi.absoluteFilePath();
            emit promptChanged();
            appendOutput(echo(line) + prompt());
            return;
        }

        case CliVerb::Tgz: {
            if (!needs(1)) { return; }
            // tgz packs a folder on THIS computer. Handed an SD-card path it
            // used to fail with a bare "no such folder", which reads like the
            // folder is missing rather than like it is on the wrong machine.
            if (cliIsDevicePath(args.at(0))) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ %1 is on the Flipper; tgz packs a folder on this computer. "
                    "Copy it over first: cp -r %1 ~/somewhere ]\n").arg(args.at(0)) + prompt());
                return;
            }
            const QString src = lhere(args.at(0));
            const QString out = args.count() > 1 ? lhere(args.at(1))
                                                 : QFileInfo(src).absolutePath() + QStringLiteral("/bkp.tgz");
            int packed = 0, skipped = 0;
            QString err;
            const bool ok = nikitaWriteTgz(src, out, &packed, &skipped, &err);
            QString msg = echo(line);
            if (ok) {
                const qint64 bytes = QFileInfo(out).size();
                const QString human = bytes >= 1024 * 1024
                        ? QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1)
                        : (bytes >= 1024 ? QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1)
                                         : QStringLiteral("%1 B").arg(bytes));
                msg += QStringLiteral("[ %1 -> %2 ]\n[ %3 file%4%5, %6 ]\n")
                       .arg(src, out).arg(packed)
                       .arg(packed == 1 ? QString() : QStringLiteral("s"))
                       .arg(skipped ? QStringLiteral(", %1 skipped").arg(skipped) : QString(), human);
            } else {
                msg += QStringLiteral("Storage error: %1\n").arg(err);
            }
            appendOutput(msg + prompt());
            return;
        }

        case CliVerb::Name: {
            if (!m_port || !m_active) {
                appendOutput(echo(line) + QStringLiteral("[ the CLI link is not open ]\n") + prompt());
                return;
            }
            if (!needs(1)) { return; }

            const QString arg = args.at(0);
            const bool isReset = arg.compare(QLatin1String("reset"), Qt::CaseInsensitive) == 0
                               || arg.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0;

            // 2-8 letters/digits: the rule custom firmware applies when it reads
            // name.settings at boot. Checking here makes a bad name fail right
            // where it was typed, instead of being silently ignored after a reboot.
            static const QRegularExpression validName(QStringLiteral("^[A-Za-z0-9]{2,8}$"));
            if (!isReset && !validName.match(arg).hasMatch()) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ name must be 2-8 letters/numbers -- the firmware silently ignores anything else ]\n")
                    + prompt());
                return;
            }

            appendOutput(echo(line));

            // Ask the firmware BEFORE writing anything.
            //
            // Measured on a real Flipper (Official firmware 1.4.3): the name comes
            // from the factory OTP block -- the same block as hardware_uid/ver/
            // color, burned once in production. Official firmware never reads
            // name.settings from anywhere, and has no "name" command of its own
            // (checked against its CLI `help`: 31 commands, none for naming).
            // The file was written to /ext, then to /int, rebooting both times:
            // the name stayed "Ut4me" on both.
            //
            // So on Official this CANNOT work. What could be fixed was the
            // behaviour -- instead of writing a file nobody reads and rebooting
            // the user's device for nothing (the disconnect that looked like the
            // bug), say plainly what is going on.
            sendRaw(QStringLiteral("device_info"), [this, arg, isReset](const QString &info) {
                QString fork, hwName;
                const QStringList linhas = info.split(QLatin1Char('\n'));
                for (const QString &l : linhas) {
                    const QString t = l.trimmed();
                    if (t.startsWith(QLatin1String("firmware_origin_fork"))) {
                        fork = t.section(QLatin1Char(':'), 1).trimmed();
                    } else if (t.startsWith(QLatin1String("hardware_name"))) {
                        hwName = t.section(QLatin1Char(':'), 1).trimmed();
                    }
                }

                const bool oficial = fork.compare(QLatin1String("Official"), Qt::CaseInsensitive) == 0;

                if (oficial && !isReset) {
                    appendOutput(QStringLiteral(
                        "[ this Flipper runs OFFICIAL firmware -- the name can't be changed ]\n"
                        "  \"%1\" comes from the factory OTP block, burned in at production and\n"
                        "  read-only. Official firmware never reads name.settings and has no\n"
                        "  'name' command of its own -- verified on this device.\n"
                        "  Custom firmware (Momentum / Unleashed / RogueMaster) supports it.\n"
                        "  Nothing was written and your Flipper was NOT rebooted.\n")
                        .arg(hwName.isEmpty() ? QStringLiteral("the current name") : hwName));
                    appendOutput(prompt());
                    return;
                }

                if (isReset) {
                    // Clean both paths: /ext is where earlier versions of this
                    // command wrote, /int is where custom firmware looks. No
                    // reboot on Official -- there is nothing to apply.
                    sendRaw(QStringLiteral("storage remove /int/dolphin/name.settings"), [this, oficial](const QString &) {
                        sendRaw(QStringLiteral("storage remove /ext/dolphin/name.settings"), [this, oficial](const QString &) {
                            // "not found" aqui e normal -- pode nao haver nome custom.
                            if (oficial) {
                                appendOutput(QStringLiteral(
                                    "[ leftover name files removed -- no reboot needed ]\n"
                                    "  Official firmware takes the name from OTP, so those files\n"
                                    "  never did anything anyway.\n"));
                                appendOutput(prompt());
                                return;
                            }
                            appendOutput(QStringLiteral("[ name reset -- rebooting to apply ]\n"));
                            sendRaw(QStringLiteral("power reboot"), [this](const QString &) {
                                appendOutput(prompt());
                            });
                        });
                    });
                    return;
                }

                // Custom firmware: write and reboot. Both paths, because the
                // forks do not agree on where the file lives, and a 52-byte file
                // in the wrong place costs nothing.
                auto tmp = std::make_shared<QTemporaryFile>();
                tmp->setFileTemplate(QDir::temp().filePath(QStringLiteral("nikita-name-XXXXXX.settings")));
                if (!tmp->open()) {
                    appendOutput(QStringLiteral("[ couldn't stage the name file locally ]\n") + prompt());
                    return;
                }
                tmp->write(QStringLiteral("Filetype: Flipper Name File\nVersion: 1\nName: %1\n")
                           .arg(arg).toUtf8());
                tmp->close();

                m_xferChain = [this, arg, tmp](bool success) {
                    if (!success) { appendOutput(prompt()); return; }
                    auto tmp2 = std::make_shared<QTemporaryFile>();
                    tmp2->setFileTemplate(QDir::temp().filePath(QStringLiteral("nikita-name-XXXXXX.settings")));
                    if (tmp2->open()) {
                        tmp2->write(QStringLiteral("Filetype: Flipper Name File\nVersion: 1\nName: %1\n")
                                    .arg(arg).toUtf8());
                        tmp2->close();
                    }
                    m_xferChain = [this, arg, tmp2](bool ok2) {
                        Q_UNUSED(ok2)
                        appendOutput(QStringLiteral("[ name set to \"%1\" -- rebooting to apply ]\n").arg(arg));
                        sendRaw(QStringLiteral("power reboot"), [this](const QString &) {
                            appendOutput(prompt());
                        });
                    };
                    uploadToFlipper(tmp2->fileName(), QStringLiteral("/ext/dolphin/name.settings"), true);
                };
                uploadToFlipper(tmp->fileName(), QStringLiteral("/int/dolphin/name.settings"), true);
            });
            return;
        }

        case CliVerb::Help:
            if (args.count() >= 1) {
                const QString topic = args.at(0).toLower();
                QString h = cliHelpFor(topic);
                if (h.isEmpty()) { h = cliFwHelpFor(topic); }
                if (!h.isEmpty()) {
                    appendOutput(echo(line) + h + QStringLiteral("\n") + prompt());
                    return;
                }
                if (cliIsHostPassthrough(topic)) {
                    appendOutput(echo(line) + QStringLiteral(
                        "Runs %1 on THIS computer and pipes its output here.\n").arg(topic) + prompt());
                    return;
                }
                // Unknown to us -- let the firmware answer ("led ?" style).
                break;
            }
            // Bare "help": ask the firmware for its listing and merge ours into
            // the reply. Armed here rather than sniffed out of the reply,
            // because "Commands available:" can land split across two serial
            // chunks and the listing then slips through unformatted.
            if (m_port && m_active) {
                m_capture = Capture::Help;
                m_captureBuf.clear();
                armGuard();
                writeLine(QStringLiteral("help"), false);
                return;
            }
            appendOutput(echo(line) + cliOfflineHelp() + prompt());
            return;

        // ---- this computer -------------------------------------------------
        // Everything below runs a program here and pipes it back. They work
        // whether or not a Flipper is attached, which is the point: this panel
        // is the only terminal a lot of people will have open.
        case CliVerb::HostRun:
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            runHostCommand(args.positional);
            return;

        case CliVerb::Sudo: {
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            // A QProcess has no controlling terminal, so sudo cannot prompt for
            // a password: without -n it would sit there until the watchdog
            // killed it with nothing on screen to explain why. -n makes it fail
            // immediately and legibly instead, and the message says how to make
            // it work.
            QStringList argv = args.rawTokens;
            if (!argv.contains(QStringLiteral("-n")) && !argv.contains(QStringLiteral("--non-interactive"))) {
                argv.prepend(QStringLiteral("-n"));
            }
            argv.prepend(QStringLiteral("sudo"));
            runHostCommand(argv);
            return;
        }

        case CliVerb::SuLike:
            appendOutput(echo(line) + QStringLiteral(
                "[ %1 needs a real terminal to ask for a password, and this panel is not one.\n"
                "  Run 'sudo -v' in a terminal window to cache your credentials, then 'sudo <command>' works here. ]\n")
                .arg(args.typedVerb) + prompt());
            return;

        case CliVerb::Pm3: {
            if (args.rawTokens.isEmpty()) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ usage: pm3 <proxmark command>   e.g. pm3 hf search, pm3 lf search, pm3 hf mf autopwn.\n"
                    "  Runs the pm3 client on THIS computer -- the Proxmark3 must be plugged in here. ]\n") + prompt());
                return;
            }
            appendOutput(echo(line));
            // pm3 is normally interactive (it opens its own "[usb] pm3 -->"
            // prompt); a one-command-per-line terminal cannot drive that, so we
            // use its batch form: `pm3 -c "<command>"` runs one command and
            // exits. Proxmark ops (autopwn, dictionary attacks) run long.
            const QString pmCmd = args.rawTokens.join(QLatin1Char(' '));
            runHostProgram(QStringLiteral("pm3 -c \"%1\"").arg(pmCmd), QStringLiteral("pm3"),
                           { QStringLiteral("-c"), pmCmd }, 600000);
            return;
        }

        case CliVerb::Python3: {
            if (args.rawTokens.isEmpty()) {
                // Bare "python3" wants an interactive REPL. runHostProgram()
                // can't give it one -- it closes the child's stdin the instant
                // it starts, on purpose, so an unexpected prompt can't hang
                // forever -- so this goes through startInteractiveSession()
                // instead, which keeps stdin open and hands the panel's input
                // to the process until it exits on its own. -u: unbuffered, so
                // output shows up as it's printed rather than batched -- it
                // matters more than usual here because stdout isn't a tty.
                appendOutput(echo(line));
                startInteractiveSession(QStringLiteral("python3"), QStringLiteral("python3"),
                                        {QStringLiteral("-i"), QStringLiteral("-u")});
                return;
            }
            if (!cliIsDevicePath(args.at(0))) {
                // A script that already lives on this computer (or a bare
                // relative name, which -- like every other bare relative name
                // in this panel -- means the Flipper only when a command
                // routes by path; python3 never did). Same passthrough as
                // before this verb got its own row.
                appendOutput(echo(line));
                runHostCommand(QStringList{ QStringLiteral("python3") } + args.rawTokens);
                return;
            }
            if (!m_port || !m_active) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ not connected -- can't fetch %1 off the card ]\n").arg(args.at(0)) + prompt());
                return;
            }
            appendOutput(echo(line) + QStringLiteral("[ fetching %1 off the SD card... ]\n").arg(args.at(0)));
            const QString devPath = here(args.at(0));
            const QStringList restArgs = args.rawTokens.mid(1);
            readDeviceText(devPath, [this, devPath, restArgs](bool ok, const QString &content) {
                if (!ok) {
                    appendOutput(QStringLiteral("[ couldn't read %1 ]\n").arg(devPath) + prompt());
                    return;
                }
                // Named like the real script (Python errors report the file
                // name), and kept alive by the onFinished capture below until
                // the interpreter is done reading it -- deleting it the moment
                // this lambda returns would pull it out from under a process
                // that's still starting up.
                auto tmp = std::make_shared<QTemporaryFile>(
                    QDir::temp().filePath(QStringLiteral("nikita-XXXXXX-") + QFileInfo(devPath).fileName()));
                if (!tmp->open()) {
                    appendOutput(QStringLiteral("[ couldn't stage %1 locally ]\n").arg(devPath) + prompt());
                    return;
                }
                tmp->write(content.toUtf8());
                tmp->close();
                runHostProgram(QStringLiteral("python3 %1").arg(devPath), QStringLiteral("python3"),
                               QStringList{ tmp->fileName() } + restArgs, 30000,
                               [tmp]() { /* keeps tmp alive until now */ });
            });
            return;
        }

        case CliVerb::FlipperInstall: {
            if (args.count() < 2 || args.at(0) != QLatin1String("install")) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ usage: flipper install <url to a .fap> [Category/name.fap]\n"
                    "  Category defaults to Tools. The Flipper's app menu only sees apps filed\n"
                    "  under a category folder, e.g. NFC, Games, Tools -- one dropped straight in\n"
                    "  /ext/apps doesn't show up there, so this picks one for you unless you say\n"
                    "  otherwise. ]\n") + prompt());
                return;
            }
            if (!m_port || !m_active) {
                appendOutput(echo(line) + QStringLiteral("[ not connected ]\n") + prompt());
                return;
            }
            const QString url = args.at(1);
            // The on-device app menu is organised by category folder (Tools, NFC,
            // Games, ...); a .fap dropped straight in /ext/apps never shows up
            // there even though the file transfer itself succeeds. Tools is a
            // reasonable catch-all default; "Category/name.fap" overrides it.
            QString rel = args.count() > 2 ? args.at(2) : QUrl(url).fileName();
            if (rel.isEmpty()) { rel = QStringLiteral("app.fap"); }
            if (!rel.contains(QLatin1Char('/'))) { rel = QStringLiteral("Tools/") + rel; }
            if (!rel.endsWith(QLatin1String(".fap"), Qt::CaseInsensitive)) { rel += QStringLiteral(".fap"); }
            appendOutput(echo(line));
            startWget(url, QStringLiteral("/ext/apps/") + rel, true);
            return;
        }

        case CliVerb::Ping:
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            // The Flipper has no network at all, so this is unambiguous: it is
            // this computer pinging.
            runHostProgram(QStringLiteral("ping %1").arg(args.at(0)), QStringLiteral("ping"),
                           { QStringLiteral("-c"), args.value("c", QStringLiteral("4")), args.at(0) }, 30000);
            return;

        case CliVerb::Ifconfig:
            appendOutput(echo(line));
            // `ip addr` on Linux, `ifconfig` on macOS (no `ip` there by default).
            if (!QStandardPaths::findExecutable(QStringLiteral("ip")).isEmpty()) {
                runHostCommand(QStringList{ QStringLiteral("ip"), QStringLiteral("addr") });
            } else {
                runHostCommand(QStringList{ QStringLiteral("ifconfig") } + args.rawTokens);
            }
            return;

        case CliVerb::Uname:
            appendOutput(echo(line) + QStringLiteral(
                "[ the Flipper has no kernel; its firmware and hardware identity is 'device_info' "
                "or 'neofetch'. Showing this computer's: ]\n"));
            runHostCommand(QStringList{ QStringLiteral("uname") }
                           + (args.rawTokens.isEmpty() ? QStringList{ QStringLiteral("-a") } : args.rawTokens));
            return;

        case CliVerb::Whoami:
            // fwhoami asks the Flipper who IT is, and falls through to the
            // device half below. Only the bare spelling is answered here.
            if (args.flipper) { break; }
            // Printed rather than shelled out to: it is one environment lookup,
            // and spawning a process to answer it would put a visible pause on
            // the most trivial command in the panel.
            appendOutput(echo(line) + cliHostUser() + QLatin1Char('\n') + prompt());
            return;

        case CliVerb::Hostname:
            appendOutput(echo(line) + QSysInfo::machineHostName() + QLatin1Char('\n') + prompt());
            return;

        case CliVerb::Ps:
            appendOutput(echo(line) + QStringLiteral(
                "[ processes on THIS computer. For the Flipper's own threads use 'top'. ]\n"));
            runHostCommand(QStringList{ QStringLiteral("ps") }
                           + (args.rawTokens.isEmpty() ? QStringList{ QStringLiteral("aux") } : args.rawTokens));
            return;

        case CliVerb::Kill:
            if (!needs(1)) { return; }
            appendOutput(echo(line) + QStringLiteral(
                "[ signalling a process on THIS computer. To stop the Flipper's running app use 'close'. ]\n"));
            runHostCommand(QStringList{ args.typedVerb.toLower() } + args.rawTokens);
            return;

        case CliVerb::Pkg:
            appendOutput(echo(line) + QStringLiteral(
                "[ package manager on THIS computer. The Flipper has none: its apps are .fap files -- "
                "copy one into /ext/apps with 'cp', or fetch it with 'wget <url> /ext/apps/...'. ]\n"));
            runHostCommand(QStringList{ args.typedVerb.toLower() } + args.rawTokens);
            return;

        case CliVerb::Mount:
        case CliVerb::Umount:
        case CliVerb::Fdisk:
        case CliVerb::Lsblk: {
            // These name a real program here and mean nothing on the Flipper,
            // whose two volumes are always mounted and cannot be repartitioned
            // over the wire. Point that out once, then just run the thing.
            if (args.count() && cliIsDevicePath(args.at(0))) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ /int and /ext are always mounted and cannot be changed from here. "
                    "'df' shows their space, and Format erases the card. ]\n") + prompt());
                return;
            }
            appendOutput(echo(line));
            runHostCommand(QStringList{ args.typedVerb.toLower() } + args.rawTokens);
            return;
        }

        case CliVerb::Chmod: {
            // The one genuinely two-sided case. On this computer chmod is real;
            // on the Flipper's FatFS there is nothing to set, and saying so is
            // more useful than running a command that silently does nothing.
            const QString target = args.count() ? args.positional.last() : QString();
            if (target.isEmpty() || cliIsDevicePath(target) || !cliIsHostPath(target)) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ FatFS on the Flipper has no owners and no permission bits -- there is nothing to set.\n"
                    "  On this computer it works: %1 755 ~/some/file ]\n").arg(args.typedVerb.toLower())
                    + prompt());
                return;
            }
            appendOutput(echo(line));
            QStringList argv = args.rawTokens;
            for (QString &t : argv) { if (cliIsHostPath(t)) { t = lhere(t); } }
            runHostCommand(QStringList{ args.typedVerb.toLower() } + argv);
            return;
        }

        case CliVerb::Open: {
            // fopen (or an f-prefixed alias) means the Flipper -- loader open
            // <app> -- and falls through to the device switch below. Bare
            // open means this computer, generic the way macOS/Linux's own
            // open/xdg-open are: a file goes to its default app, a folder to
            // the file manager, a URL to the browser. One command covers
            // "open a file", "open a folder" and "open a website".
            if (args.flipper) { break; }
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            const QString target = args.rawTokens.join(QLatin1Char(' '));
            const bool isUrl = target.contains(QLatin1String("://"));
            const QUrl url = isUrl ? QUrl(target) : QUrl::fromLocalFile(lhere(target));
            if (!isUrl && !QFileInfo::exists(url.toLocalFile())) {
                appendOutput(QStringLiteral("[ no such file or folder on this computer: %1 ]\n")
                             .arg(url.toLocalFile()) + prompt());
                return;
            }
            if (!QDesktopServices::openUrl(url)) {
                appendOutput(QStringLiteral("[ couldn't open %1 ]\n").arg(target) + prompt());
                cliLogFail(QStringLiteral("open %1 -- couldn't open").arg(target));
                return;
            }
            // Success is the common case and macOS's own "open" says nothing
            // back either -- printing a line for it just clutters the
            // transcript. It still goes in the LOGS panel (the file manager
            // and the assistant's tools record what they touch too; the CLI
            // was the one blind spot).
            cliLog(QStringLiteral("open %1").arg(target));
            appendOutput(prompt());
            return;
        }

        default:
            break;   // falls through to the device commands below
        }
    }

    // A bare program name we know lives on this computer and nowhere on the
    // Flipper: git, tar, openssl, ssh and friends. Reachable directly so the
    // panel behaves like a terminal, without any of them needing a table row.
    if (!def && cliIsHostPassthrough(args.verb)) {
        appendOutput(echo(line));
        runHostCommand(tokens);
        return;
    }

    // ---- editor ------------------------------------------------------------
    // Sits between the two halves because it is the one command that genuinely
    // serves both machines: "edit /ext/x.txt" pulls the file off the card,
    // "edit ~/x.txt" opens the local one, and the same panel saves either back.
    if (def && def->id == CliVerb::Edit) {
        if (!needs(1)) { return; }
        appendOutput(echo(line));
        const bool host = cliIsHostPath(args.at(0));
        if (!host && (!m_port || !m_active)) {
            appendOutput(QStringLiteral("[ not connected -- only files on this computer can be edited ]\n")
                         + prompt());
            return;
        }
        if (host) { startHostEdit(lhere(args.at(0))); }
        else      { startEdit(here(args.at(0))); }
        return;
    }

    // ---- the path picks the machine ----------------------------------------
    // "ls ~/Desktop" used to mean "storage list /ext/Desktop", because ~ is /ext
    // on the Flipper side -- so a path that obviously belonged to this computer
    // went to the card and failed. cp already routed by path shape; everything
    // else did not, and "host" was the workaround for a distinction the CLI had
    // enough information to make on its own.
    //
    // Now an explicit host path in the arguments sends the whole command here,
    // to the real program of the same name. That is not an approximation of ls:
    // it IS ls, with every flag it has ever had. A bare relative name still
    // means the Flipper, because that is what the prompt is showing.
    if (def && cliRoutesByPath(def->id) && !args.flipper) {
        // The bare spelling means this computer, full stop -- that is the whole
        // point of the f/no-f split, and a rule with exceptions is not a rule.
        // The command runs as the real program of the same name, so it is not
        // an approximation of ls: it IS ls, with every flag it has ever had.
        // rm now runs on the user's actual computer by default. "rm -rf /" and
        // "rm -rf ~" are unrecoverable there in a way they never were on a
        // Flipper card, and this panel is not the place to find that out.
        if (def->id == CliVerb::Rm && args.has("r")) {
            for (const QString &t : args.positional) {
                const QString abs = cliExpandHostPath(t, m_hostCwd);
                if (cliIsProtectedHostPath(abs)) {
                    appendOutput(echo(line) + QStringLiteral(
                        "[ refusing to recursively delete %1 -- that is your home or a system "
                        "folder on this computer, and there is no undo.\n"
                        "  If you truly mean it, do it in a terminal where you can see what "
                        "you are typing. ]\n").arg(abs) + prompt());
                    return;
                }
            }
        }
        // grep and sed take a PATTERN as their first positional, not a path.
        // Expanding it turned "grep linha a.txt" into
        // "grep /Users/you/Desktop/linha /Users/you/Desktop/a.txt": the search
        // term became a directory that does not exist, and the command could
        // never match anything.
        const bool firstIsPattern = (def->id == CliVerb::Grep || def->id == CliVerb::Sed);
        bool tookFirstPositional = false;

        QStringList hostArgs;
        for (const QString &t : args.rawTokens) {
            if (t.startsWith(QLatin1Char('-'))) { hostArgs += t; continue; }
            if (firstIsPattern && !tookFirstPositional) {
                tookFirstPositional = true;
                hostArgs += t;               // verbatim: it is the pattern
                continue;
            }
            tookFirstPositional = true;
            if (cliIsDevicePath(t)) {
                // A Flipper path handed to the computer's half. Almost always a
                // missing f rather than a real intention, and running it would
                // either fail confusingly or, for rm, hit a same-named folder
                // here. Say which command they wanted.
                appendOutput(echo(line) + QStringLiteral(
                    "[ %1 is on the Flipper -- use %2 for that ]\n")
                    .arg(t, QLatin1String(*def->fname ? def->fname : def->name)) + prompt());
                return;
            }
            hostArgs += cliExpandHostPath(t, m_hostCwd);   // execve does not expand ~
        }
        appendOutput(echo(line));
        runHostCommand(QStringList{ cliHostProgramFor(def) } + hostArgs);
        return;
    }

    // ---- everything below talks to the Flipper -----------------------------
    if (!m_port || !m_active) {
        // Silently returning here is what made a disconnected panel look
        // broken: the line vanished, no prompt came back, and nothing said why.
        appendOutput(echo(line) + QStringLiteral(
            "[ not connected. Plug the Flipper in, or reopen this panel. "
            "Host commands (host, ping, git, ...) still work. ]\n") + prompt());
        return;
    }

    // Record what the user just did to the device. Placed after the panel-local
    // and host commands so "clear", "colors" and "ping" don't land in the log.
    if (cliCommandMutates(args.verb)) { cliLog(m_lastTyped); }

    QString fw;   // the real firmware command to run, when there is one

    if (def) {
        switch (def->id) {
        case CliVerb::Cd: {
            const QString p0 = args.at(0);
            QString target;
            if (p0.isEmpty() || p0 == QLatin1String("~")) { target = QStringLiteral("/ext"); }
            else if (p0 == QLatin1String("-"))            { target = m_cdPrev; }
            else                                          { target = here(p0); }
            if (target == m_cwd) { appendOutput(echo(line) + prompt()); return; }
            // Update the working directory immediately instead of waiting for a
            // "storage stat" round trip. The blocking version put the CLI in a
            // busy state until the device answered; when several commands were
            // pasted at once they queued behind it, the stat reply got mixed
            // into their output, and a perfectly real folder came back as "no
            // such folder" -- plus every following relative path resolved
            // against the OLD directory because cwd hadn't moved yet. A bad
            // path is caught by the next command that touches the device.
            m_cdPrev = m_cwd;
            m_cwd = target;
            emit promptChanged();
            setStatus(QStringLiteral("CLI live -- %1").arg(m_cwd));
            appendOutput(echo(line) + prompt());
            return;
        }

        case CliVerb::Pwd:
            appendOutput(echo(line) + m_cwd + QLatin1Char('\n') + prompt());
            return;


        case CliVerb::Ls:
            fw = QStringLiteral("storage list ") + here(args.at(0));
            break;

        case CliVerb::Tree:
            fw = QStringLiteral("storage tree ") + here(args.at(0));
            break;

        case CliVerb::Cat: {
            if (!needs(1)) { return; }
            // Through readDeviceText rather than raw "storage read", so the
            // "Size: N" header and the CRs the console adds to every line stay
            // out of the output. cat is supposed to print the file and nothing
            // else -- and head/tail/wc/grep already parse it exactly this way.
            const QString target = here(args.at(0));
            appendOutput(echo(line));
            readDeviceText(target, [this, target](bool ok, const QString &body) {
                if (!ok) {
                    appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(target) + prompt());
                    return;
                }
                appendOutput((body.isEmpty() ? QString() : body + QLatin1Char('\n')) + prompt());
            });
            return;
        }

        case CliVerb::Rm: {
            if (!needs(1)) { return; }
            const QString target = here(args.at(0));
            // "rm -rf /ext" is a factory wipe with none of the safeguards the
            // Format button has, and no undo. It is always a mistake to let it
            // through here.
            if (cliIsStorageRoot(target) || target == QLatin1String("/")) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ refusing to delete the whole of %1. Use the Format button, which "
                    "unmounts and rebuilds the card properly. ]\n").arg(target) + prompt());
                return;
            }
            if (target.contains(QLatin1Char('*')) || target.contains(QLatin1Char('?'))) {
                appendOutput(echo(line));
                expandDeviceGlob(target, [this](const QStringList &matches) {
                    if (matches.isEmpty()) {
                        appendOutput(QStringLiteral("[ no matches ]\n") + prompt());
                        return;
                    }
                    runRemoveQueue(matches);
                });
                return;
            }
            if (args.has("r")) {
                appendOutput(echo(line));
                // -f finally means something. Without it, a recursive delete
                // says what it is about to destroy and stops; the count comes
                // from the tree scan startRemoveTree would run anyway.
                if (args.has("f")) { startRemoveTree(target); return; }
                confirmRemoveTree(target);
                return;
            }
            fw = QStringLiteral("storage remove ") + target;
            break;
        }

        case CliVerb::Mkdir: {
            if (!needs(1)) { return; }
            const QString target = here(args.at(0));
            if (args.has("p")) {
                appendOutput(echo(line));
                ensureDeviceDir(target, [this, target]() {
                    appendOutput(QStringLiteral("[ %1 ready ]\n").arg(target) + prompt());
                });
                return;
            }
            fw = QStringLiteral("storage mkdir ") + target;
            break;
        }

        case CliVerb::Cp: {
            if (!needs(2)) { return; }
            // "cp a.txt b.txt somewhere/" quietly copied a.txt to b.txt and
            // threw the destination away. Two paths, or say so.
            if (args.count() > 2) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ cp takes one source and one destination. For several files use a wildcard: cp *.txt %1 ]\n")
                    .arg(args.positional.last()) + prompt());
                return;
            }
            const QString p0 = args.at(0);
            const QString p1 = args.at(1);
            const bool srcHost = cliIsHostPath(p0);
            const bool dstHost = cliIsHostPath(p1);
            const bool srcGlob = p0.contains(QLatin1Char('*')) || p0.contains(QLatin1Char('?'));

            if (srcHost && dstHost) {
                appendOutput(echo(line) + QStringLiteral(
                    "[ both paths are on this computer -- nothing for the Flipper to do. "
                    "Use 'host cp %1 %2'. ]\n").arg(p0, p1) + prompt());
                return;
            }
            if (srcHost && srcGlob) {
                appendOutput(echo(line));
                const QString pat = lhere(p0);
                const QFileInfo pi(pat);
                const QStringList names = QDir(pi.absolutePath())
                        .entryList(QStringList{ pi.fileName() }, QDir::Files, QDir::Name);
                if (names.isEmpty()) {
                    appendOutput(QStringLiteral("[ nothing on this computer matches %1 ]\n").arg(pat) + prompt());
                    return;
                }
                QStringList hostFiles;
                for (const QString &nm : names) { hostFiles += pi.absolutePath() + QLatin1Char('/') + nm; }
                runUploadQueue(hostFiles, here(p1));
                return;
            }
            if (!srcHost && srcGlob) {
                appendOutput(echo(line));
                const QString devGlob = here(p0);
                const QString dstResolved = dstHost ? lhere(p1) : here(p1);
                if (dstHost) { QDir().mkpath(dstResolved); }
                expandDeviceGlob(devGlob, [this, dstResolved, dstHost](const QStringList &matches) {
                    if (matches.isEmpty()) {
                        appendOutput(QStringLiteral("[ no matches ]\n") + prompt());
                        return;
                    }
                    runCopyQueue(matches, dstResolved, dstHost);
                });
                return;
            }
            if (!srcHost && !dstHost) {
                fw = QStringLiteral("storage copy ") + here(p0) + QLatin1Char(' ') + here(p1);
                break;
            }
            appendOutput(echo(line));
            if (srcHost) {
                const QString hostSrc = lhere(p0);
                if (!QFileInfo::exists(hostSrc)) {
                    appendOutput(QStringLiteral("[ no such file on this computer: %1 ]\n").arg(hostSrc) + prompt());
                    return;
                }
                if (args.has("r") || QFileInfo(hostSrc).isDir()) { startCopyUpTree(hostSrc, here(p1)); }
                else                                            { uploadToFlipper(hostSrc, here(p1)); }
            } else {
                const QString devSrc  = here(p0);
                const QString hostDst = lhere(p1);
                if (args.has("r")) { startCopyDownTree(devSrc, hostDst); }
                else               { downloadFromFlipper(devSrc, hostDst); }
            }
            return;
        }

        case CliVerb::Mv:
            if (!needs(2)) { return; }
            fw = QStringLiteral("storage rename ") + here(args.at(0)) + QLatin1Char(' ') + here(args.at(1));
            break;

        case CliVerb::Stat:
            if (!needs(1)) { return; }
            fw = QStringLiteral("storage stat ") + here(args.at(0));
            break;

        case CliVerb::Md5:
            if (!needs(1)) { return; }
            fw = QStringLiteral("storage md5 ") + here(args.at(0));
            break;

        case CliVerb::Df:
            // Ask about the volume, never about a folder inside it: "storage
            // info /ext/clitest" is not a volume and answers with usage text.
            fw = QStringLiteral("storage info ")
                 + cliVolumeOf(args.count() ? here(args.at(0)) : m_cwd);
            break;

        case CliVerb::Du:
            appendOutput(echo(line));
            startDu(args.count() ? here(args.at(0)) : m_cwd);
            return;

        case CliVerb::Touch:
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            writeTextToDevice(QString(), here(args.at(0)), false);
            return;

        case CliVerb::Find: {
            if (!needs(1)) { return; }
            QString pat  = args.at(0);
            QString root = args.count() > 1 ? here(args.at(1)) : m_cwd;
            // "find /ext/subghz *.sub" is how find(1) is spelled everywhere
            // else, so accept it: an absolute first argument with no wildcard
            // in it can only be the root.
            if (args.count() > 1 && pat.startsWith(QLatin1Char('/'))
                && !pat.contains(QLatin1Char('*')) && !pat.contains(QLatin1Char('?'))) {
                root = here(args.at(0));
                pat  = args.at(1);
            }
            appendOutput(echo(line));
            startFind(root, pat);
            return;
        }

        case CliVerb::Grep:
            if (!needs(2)) { return; }
            appendOutput(echo(line));
            startGrep(args.at(0), here(args.at(1)));
            return;

        case CliVerb::Head:
        case CliVerb::Tail: {
            if (!needs(1)) { return; }
            bool okN = false;
            const int parsed = args.value("n").toInt(&okN);
            const int n = (okN && parsed > 0) ? parsed : 10;
            appendOutput(echo(line));
            startHeadTail(here(args.at(0)), n, def->id == CliVerb::Head);
            return;
        }

        case CliVerb::Wc:
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            startWc(here(args.at(0)));
            return;

        case CliVerb::Sed:
            if (!needs(2)) { return; }
            appendOutput(echo(line));
            startSed(args.at(0), here(args.at(1)));
            return;

        case CliVerb::Diff:
            if (!needs(2)) { return; }
            appendOutput(echo(line));
            startDiff(here(args.at(0)), here(args.at(1)));
            return;

        case CliVerb::FileType:
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            startFileType(here(args.at(0)));
            return;

        case CliVerb::Locate:
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            startLocate(args.at(0));
            return;

        case CliVerb::Echo: {
            // "echo <text> > <file>" and ">>" write to the Flipper's storage.
            // The firmware's own echo only parrots bytes back down the wire, so
            // the redirect form is entirely ours. Matched against the raw line
            // rather than the tokens, because the spacing inside the text is
            // part of what gets written.
            static const QRegularExpression redir(
                QStringLiteral("^echo\\s+(.*?)\\s*(>>|>)\\s*(\\S+)\\s*$"));
            const QRegularExpressionMatch m = redir.match(line);
            if (m.hasMatch()) {
                QString text = m.captured(1);
                if (text.size() >= 2
                    && ((text.startsWith(QLatin1Char('"'))  && text.endsWith(QLatin1Char('"')))
                     || (text.startsWith(QLatin1Char('\'')) && text.endsWith(QLatin1Char('\''))))) {
                    text = text.mid(1, text.size() - 2);
                }
                appendOutput(echo(line));
                writeTextToDevice(text + QLatin1Char('\n'),
                                  cliResolvePath(m_cwd, m.captured(3)),
                                  m.captured(2) == QLatin1String(">>"));
                return;
            }
            fw = QStringLiteral("echo ") + args.positional.join(QLatin1Char(' '));
            break;
        }

        case CliVerb::Wget: {
            if (!needs(1)) { return; }
            appendOutput(echo(line));
            const QString url = args.at(0);
            // No destination -> keep the URL's own filename, drop it here.
            QString name = QUrl(url).fileName();
            if (name.isEmpty()) { name = QStringLiteral("download.bin"); }
            // Either way startWget gets a complete file path, so the upload
            // never has to guess whether the last segment is a folder.
            QString dst;
            const QString p1 = args.at(1);
            if (p1.isEmpty()) {
                dst = cliResolvePath(m_cwd, name);
            } else if (p1.endsWith(QLatin1Char('/'))) {
                QString dir = cliResolvePath(m_cwd, p1);
                while (dir.size() > 1 && dir.endsWith(QLatin1Char('/'))) { dir.chop(1); }
                dst = dir + QLatin1Char('/') + name;
            } else {
                dst = here(p1);
            }
            startWget(url, dst, true);
            return;
        }

        case CliVerb::Whoami:
        case CliVerb::FWhoami:  fw = QStringLiteral("device_info");  break;
        case CliVerb::Reboot:   fw = QStringLiteral("power reboot"); break;
        case CliVerb::Shutdown: fw = QStringLiteral("power off");    break;
        case CliVerb::Close:    fw = QStringLiteral("loader close"); break;

        case CliVerb::Open:
            // Join the rest: app names have spaces ("Bad USB", "Sub-GHz").
            if (!needs(1)) { return; }
            fw = QStringLiteral("loader open ") + args.positional.join(QLatin1Char(' '));
            break;

        case CliVerb::Vibro:
            // A bare "vibro" isn't valid on its own.
            fw = args.count() ? (QStringLiteral("vibro ") + args.at(0))
                              : QStringLiteral("vibro 1");
            break;

        // Handled above, before the connection check. Listed so the switch
        // stays exhaustive and the compiler keeps checking it for us.
        case CliVerb::None:
        case CliVerb::Clear:    case CliVerb::Colors:   case CliVerb::Verbose:
        case CliVerb::History:  case CliVerb::Help:     case CliVerb::Tgz:
        case CliVerb::HostRun:
        case CliVerb::Edit:     case CliVerb::Chmod:    case CliVerb::Kill:
        case CliVerb::Ps:       case CliVerb::Pkg:      case CliVerb::Mount:
        case CliVerb::Umount:   case CliVerb::Fdisk:    case CliVerb::Lsblk:
        case CliVerb::Ifconfig: case CliVerb::Ping:     case CliVerb::Uname:
        case CliVerb::Pm3:      case CliVerb::Sudo:     case CliVerb::SuLike:
        case CliVerb::Hostname: case CliVerb::Name:      case CliVerb::Python3:
        case CliVerb::FlipperInstall:
            break;
        }
    }

    if (!fw.isEmpty()) {
        // The user typed "ls"; the wire carries "storage list /ext". Show what
        // was typed and swallow the firmware's echo of the translation -- the
        // alias plumbing shouldn't be visible.
        if (fw != line) {
            appendOutput(echo(line));
            m_echoPending = fw + QLatin1Char('\n');
        }
        if (def && def->id == CliVerb::Ls) {
            m_capture = Capture::Listing;
            m_captureBuf.clear();
        }
        // Arm the guard for EVERY firmware command, not just ls. busy() folds
        // in the guard, and the paste queue uses busy() to decide when the
        // previous command has finished. Without this, cat/stat/md5/mkdir/rm
        // reported not-busy the instant they were written, so a pasted block
        // fired the next command before the firmware had answered -- the
        // commands piled onto the serial link, the device lost sync, and
        // everything after came back "no reply from the device".
        armGuard();
        writeLine(fw, false);
        return;
    }

    // Everything that isn't one of ours goes to the firmware verbatim:
    // device_info, free, uptime, log, top, the whole original command set.
    //
    // The guard has to be armed here too. It wasn't, so a raw command reported
    // idle the instant it was written and the queue fired the next one into the
    // middle of its output -- which is why "device_info" came back with a path
    // spliced into the middle of a URL, and why "colors on" arrived as
    // "storagecolors on".
    armGuard();
    writeLine(line);
}

// The command list without a device to ask. Bare "help" normally merges the
// firmware's own listing into ours; with nothing plugged in there is still a
// useful answer to give.
QString FlipperCli::cliOfflineHelp()
{
    QStringList flipperCmds = cliNamesIn(CliWhere::Flipper);
    QStringList deviceCmds  = cliNamesIn(CliWhere::Device) + cliNamesIn(CliWhere::Panel);
    for (const char *const n : kHostPassthrough) { deviceCmds += QLatin1String(n); }
    deviceCmds.removeDuplicates();
    std::sort(deviceCmds.begin(), deviceCmds.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    int colW = 0;
    for (const QString &n : flipperCmds) { colW = qMax(colW, n.size()); }
    for (const QString &n : deviceCmds)  { colW = qMax(colW, n.size()); }
    colW += 4;

    auto grid = [colW](const QStringList &names) {
        QString out;
        const int rows = (names.size() + 1) / 2;
        for (int r = 0; r < rows; ++r) {
            QString row = names.at(r);
            const int right = r + rows;
            if (right < names.size()) {
                while (row.size() < colW) { row += QLatin1Char(' '); }
                row += names.at(right);
            }
            out += row + QLatin1Char('\n');
        }
        return out;
    };

    QString out = QStringLiteral("Available commands:\n");
    out += QStringLiteral("  (help <command> for details)\n");
    out += QStringLiteral("  (f-prefixed acts on the Flipper, bare acts on this computer)\n");
    out += QStringLiteral("------ Flipper ------\n");
    out += grid(flipperCmds);
    out += QStringLiteral("------ Computer ------\n");
    out += grid(deviceCmds);
    // Listed above, but only usable if the program is installed here.
    {
        const QString missing = cliMissingPassthrough();
        if (!missing.isEmpty()) {
            out += QStringLiteral("  (not installed on this computer: ") + missing
                   + QStringLiteral(")\n");
        }
    }
    out += QStringLiteral("------ Firmware ------\n");
    out += QStringLiteral("  (connect a Flipper to list these)\n");
    return out;
}

// "rm -r" without -f: count first, destroy never. The scan is the same one
// startRemoveTree would run, so this costs nothing extra.
void FlipperCli::confirmRemoveTree(const QString &path)
{
    auto hold = holdBusy();
    sendRaw(QStringLiteral("storage tree ") + path, [this, path, hold](const QString &raw) {
        const QList<CliTreeEntry> entries = cliParseTree(raw);
        int files = 0, dirs = 0;
        for (const CliTreeEntry &e : entries) { e.isDir ? ++dirs : ++files; }
        if (entries.isEmpty()) {
            appendOutput(QStringLiteral("[ %1 is empty or does not exist -- nothing to delete ]\n").arg(path)
                         + prompt());
            return;
        }
        appendOutput(QStringLiteral(
            "[ %1 holds %2 file%3 and %4 folder%5. This cannot be undone.\n"
            "  Run 'rm -rf %1' to go ahead. ]\n")
            .arg(path).arg(files).arg(files == 1 ? QString() : QStringLiteral("s"))
            .arg(dirs).arg(dirs == 1 ? QString() : QStringLiteral("s")) + prompt());
    });
}

// "edit ~/notes.txt" -- the local half of the editor. The panel that opens is
// the same one the Flipper side uses; only the save path differs, which is why
// the editor is told which machine the file came from.
void FlipperCli::startHostEdit(const QString &path)
{
    QFile f(path);
    if (!f.exists()) {
        // Opening a file that isn't there yet is how every editor creates one.
        emit editRequested(path, QString());
        appendOutput(QStringLiteral("[ new file: %1 ]\n").arg(path) + prompt());
        return;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        appendOutput(QStringLiteral("[ can't read %1: %2 ]\n").arg(path, f.errorString()) + prompt());
        return;
    }
    const QByteArray body = f.read(kCliMaxEditBytes + 1);
    f.close();
    if (body.size() > kCliMaxEditBytes) {
        appendOutput(QStringLiteral("[ %1 is larger than %2 KB -- too big for the editor panel ]\n")
                     .arg(path).arg(kCliMaxEditBytes / 1024) + prompt());
        return;
    }
    emit editRequested(path, QString::fromUtf8(body));
    appendOutput(QStringLiteral("[ %1 open in the editor ]\n").arg(path) + prompt());
}

// Save from the editor panel. A path on this computer is written directly; a
// Flipper path goes back through the verified upload.
void FlipperCli::saveHostFile(const QString &path, const QString &content)
{
    QFile f(path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit editSaveError(path, f.errorString());
        appendOutput(QStringLiteral("[ can't save %1: %2 ]\n").arg(path, f.errorString()) + prompt());
        return;
    }
    f.write(content.toUtf8());
    f.close();
    emit editSaved(path);
    appendOutput(QStringLiteral("[ saved %1 ]\n").arg(path) + prompt());
}

// ---- host programs ---------------------------------------------------------
// Run argv[0] on THIS computer with the rest as its arguments. No shell is
// involved anywhere: the argument list goes to execve as-is, so a filename with
// a space, a quote or a semicolon in it is data and can never become syntax.
void FlipperCli::runHostCommand(const QStringList &argv, int timeoutMs)
{
    if (argv.isEmpty()) { appendOutput(prompt()); return; }
    const QString program = argv.first();
    if (cliNeedsTty(program)) {
        appendOutput(QStringLiteral(
            "[ %1 needs a real terminal to ask for a password, and this panel is not one. ]\n")
            .arg(program) + prompt());
        return;
    }
    runHostProgram(argv.join(QLatin1Char(' ')), program, argv.mid(1), timeoutMs);
}

void FlipperCli::runHostProgram(const QString &label, const QString &program,
                                const QStringList &args, int timeoutMs,
                                std::function<void()> onFinished)
{
    // A GUI app on macOS inherits a minimal PATH from launchd, so Homebrew and
    // the system sbin dirs have to be looked in explicitly or half the tools a
    // user has installed appear not to exist.
    static const QStringList extraDirs = {
        QStringLiteral("/usr/local/bin"), QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/bin"), QStringLiteral("/bin"),
        QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
        QStringLiteral("/opt/local/bin"),
    };
    QString exe = QStandardPaths::findExecutable(program);
    if (exe.isEmpty()) { exe = QStandardPaths::findExecutable(program, extraDirs); }
    if (exe.isEmpty() && program == QLatin1String("pm3")) {
        exe = QStandardPaths::findExecutable(QStringLiteral("proxmark3"), extraDirs);
    }
    if (exe.isEmpty()) {
        appendOutput(QStringLiteral("[ %1 isn't installed on this computer (looked on PATH, "
                                    "Homebrew and the system dirs) ]\n").arg(program) + prompt());
        if (onFinished) { onFinished(); }
        return;
    }

    appendOutput(QStringLiteral("[ %1 -- running on this computer, not the Flipper ]\n").arg(label));

    // These never touch the serial line, but they do stream into the same view
    // for seconds at a time. Holding the CLI busy keeps a pasted block from
    // printing its results in the middle of a ping's output.
    auto hold = holdBusy();
    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(QDir(m_hostCwd).exists() ? m_hostCwd : QDir::homePath());

    // A shared flag rather than two captures: the timeout handler and the
    // finished handler both need to know whether the kill was ours, and they
    // run at different times.
    auto killed = std::make_shared<bool>(false);
    auto *guard = new QTimer(this);
    guard->setSingleShot(true);
    guard->setInterval(timeoutMs);

    QPointer<QProcess> procRef(proc);
    connect(guard, &QTimer::timeout, this, [procRef, killed]() {
        // QPointer: the process may already have finished and been queued for
        // deletion, and dereferencing it then is a crash, not a missed kill.
        if (procRef && procRef->state() != QProcess::NotRunning) {
            *killed = true;
            procRef->kill();
        }
    });
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        appendOutput(QString::fromUtf8(proc->readAll()));
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, guard, hold, killed, timeoutMs, onFinished](int code, QProcess::ExitStatus status) {
        guard->stop();
        appendOutput(QString::fromUtf8(proc->readAll()));
        if (*killed) {
            appendOutput(QStringLiteral("[ stopped after %1s -- no answer ]\n").arg(timeoutMs / 1000));
        } else if (status == QProcess::CrashExit) {
            appendOutput(QStringLiteral("[ %1 crashed ]\n").arg(proc->program()));
        } else if (code != 0) {
            // A non-zero exit is information, not noise: "grep found nothing"
            // and "grep could not open the file" look identical without it.
            appendOutput(QStringLiteral("[ exit %1 ]\n").arg(code));
        }
        appendOutput(prompt());
        if (onFinished) { onFinished(); }
        guard->deleteLater();
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, guard, hold, onFinished](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) { return; }   // the rest still reach finished()
        appendOutput(QStringLiteral("[ couldn't start %1 ]\n").arg(proc->program()) + prompt());
        if (onFinished) { onFinished(); }
        guard->deleteLater();
        proc->deleteLater();
    });

    proc->start(exe, args);
    // Nothing here can type at the program. Closing stdin makes anything that
    // reads it (sort, cat with no file, an accidental interactive prompt) see
    // EOF and exit at once, instead of hanging until the watchdog kills it.
    proc->closeWriteChannel();
    guard->start();
}

// The interactive counterpart to runHostProgram() just above: same PATH
// resolution, but stdin stays OPEN (never closeWriteChannel()'d) and there is
// no watchdog, because the whole point is a process that waits on the user
// for as long as it takes. send() checks m_interactiveProc before it ever
// reaches the dispatcher, and routes every line here instead while a session
// is live -- see the comment at the top of send() for that half.
void FlipperCli::startInteractiveSession(const QString &label, const QString &program,
                                         const QStringList &args)
{
    static const QStringList extraDirs = {
        QStringLiteral("/usr/local/bin"), QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/bin"), QStringLiteral("/bin"),
        QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
        QStringLiteral("/opt/local/bin"),
    };
    QString exe = QStandardPaths::findExecutable(program);
    if (exe.isEmpty()) { exe = QStandardPaths::findExecutable(program, extraDirs); }
    if (exe.isEmpty()) {
        appendOutput(QStringLiteral("[ %1 isn't installed on this computer (looked on PATH, "
                                    "Homebrew and the system dirs) ]\n").arg(program) + prompt());
        return;
    }
    if (m_interactiveProc) {
        // Can't happen through the panel (send() routes to the live session
        // instead of the dispatcher, so a second CliVerb::Python3 can never
        // fire while one is already running) -- guarded anyway rather than
        // silently orphaning the process already running.
        appendOutput(QStringLiteral("[ %1 is already running -- finish or exit that first ]\n")
                     .arg(m_interactiveLabel) + prompt());
        return;
    }

    appendOutput(QStringLiteral("[ %1 -- running on this computer, not the Flipper. Type exit() "
                                "or quit() to leave the session. ]\n").arg(label));

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(QDir(m_hostCwd).exists() ? m_hostCwd : QDir::homePath());

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        appendOutput(QString::fromUtf8(proc->readAll()));
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, label](int code, QProcess::ExitStatus status) {
        appendOutput(QString::fromUtf8(proc->readAll()));
        if (status == QProcess::CrashExit) {
            appendOutput(QStringLiteral("[ %1 crashed ]\n").arg(label));
        } else if (code != 0) {
            appendOutput(QStringLiteral("[ %1 exited (%2) ]\n").arg(label).arg(code));
        } else {
            appendOutput(QStringLiteral("[ %1 session ended ]\n").arg(label));
        }
        m_interactiveProc = nullptr;
        m_interactiveLabel.clear();
        appendOutput(prompt());
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) { return; }   // finished() still fires for the rest
        appendOutput(QStringLiteral("[ couldn't start %1 ]\n").arg(proc->program()) + prompt());
        m_interactiveProc = nullptr;
        m_interactiveLabel.clear();
        proc->deleteLater();
    });

    proc->start(exe, args);
    // Stdin stays open -- this is the one difference from runHostProgram()
    // that makes the whole thing work.
    m_interactiveProc = proc;
    m_interactiveLabel = label;
}

void FlipperCli::setVerbose(bool value)
{
    if (m_verbose == value) { return; }
    m_verbose = value;
    emit verboseChanged();
}

void FlipperCli::setColored(bool value)
{
    if (m_colored == value) { return; }
    m_colored = value;
    emit coloredChanged();
}

// ---- verbose log -----------------------------------------------------------
// A single "cp -r" or "rm -r" is dozens of firmware commands, and every one of
// them used to happen behind a summary line. With verbose on, each command and
// each reply lands in the terminal as it happens, indented so it reads as
// machinery rather than as output.
void FlipperCli::trace(const QString &what)
{
    // Recorded even when the log is off, so traceReply always knows which line
    // is merely the firmware repeating what we just said.
    m_lastTraced = what.trimmed();
    if (!m_verbose || m_quiet) { return; }
    const QString s = m_lastTraced;
    if (s.isEmpty() || s == m_lastTyped) { return; }
    appendOutput(QStringLiteral("  · ") + s + QLatin1Char('\n'));
}

void FlipperCli::traceReply(const QString &raw)
{
    if (!m_verbose || m_quiet) { return; }
    QString s = raw;
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]"));
    s.remove(ansi);
    s.remove(QLatin1Char('\r'));
    static const QRegularExpression ctrl(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]"));
    s.remove(ctrl);
    // The log is there to follow along, not to replay the payload. A character
    // cap alone wasn't enough: "cat" of a 6 KB file still put 2000 characters of
    // that file in the log and then the command printed the whole thing again
    // underneath, and a "storage tree /ext" logged tens of thousands. Cap by
    // LINES as well, which is what actually makes the trail readable.
    static const int kMaxLines = 10;
    static const int kMaxChars = 800;

    QStringList kept;
    int dropped = 0;
    for (const QString &line : s.split(QLatin1Char('\n'))) {
        const QString l = line.trimmed();
        if (l.isEmpty() || l.startsWith(QLatin1String(">:"))) { continue; }
        if (l == m_lastTyped) { continue; }
        // The Flipper echoes every command back before answering it. Printing
        // that echo made each step appear twice in the log -- indented one
        // level apart, which reads like the command actually ran twice.
        if (l == m_lastTraced) { continue; }
        if (kept.size() >= kMaxLines) { ++dropped; continue; }
        kept += (l.size() > kMaxChars) ? (l.left(kMaxChars) + QStringLiteral(" …")) : l;
    }
    if (kept.isEmpty()) { return; }

    QString out;
    for (const QString &l : kept) { out += QStringLiteral("    \u00b7 ") + l + QLatin1Char('\n'); }
    if (dropped > 0) {
        out += QStringLiteral("    \u00b7 … (%1 more line%2)\n")
                   .arg(dropped).arg(dropped == 1 ? QString() : QStringLiteral("s"));
    }
    appendOutput(out);
}

// Is the serial line itself mid-exchange? This is the question sendRaw has to
// ask: a continuation running inside a multi-step operation must be allowed to
// issue its next command even though the operation as a whole is still busy.
bool FlipperCli::portBusy() const
{
    return m_xfer != Xfer::None || !m_cdPending.isEmpty() || m_capture != Capture::None;
}

bool FlipperCli::busy() const
{
    // Also busy while an operation guard is armed: commands like ls/cat/sed that
    // go out as "storage ..." and wait for a reply arm the guard but don't set
    // m_xfer, so without this the paste queue would fire the next command before
    // the previous one answered -- the pileup this was meant to prevent.
    //
    // m_opDepth covers the gaps the guard cannot: a composite operation (echo >>
    // = read + remove + write_chunk + md5, cp -r, sed, wget, a host process)
    // spends real time BETWEEN its steps with no command in flight and the guard
    // disarmed. Every one of those windows used to read as "idle", the queue
    // fired the next command into it, and two writers ended up sharing the port.
    const bool guarded = m_opGuard && m_opGuard->isActive();
    return portBusy() || guarded || m_opDepth > 0;
}

// busyChanged is a direct connection: emitting it from inside onReadyRead lets
// the paste queue call send() synchronously, part-way through a function that
// has not finished touching the port yet. That is how a queued command reached
// the firmware before an upload's payload did and got stored as the file's
// contents. Posting the signal moves it to the next event-loop turn, where the
// port is in a consistent state.
void FlipperCli::scheduleBusyChanged()
{
    if (m_busyNotifyQueued) { return; }
    m_busyNotifyQueued = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_busyNotifyQueued = false;
        emit busyChanged();
        drainPending();
    }, Qt::QueuedConnection);
}

// A refcount token for "an operation is still in progress". Capture the returned
// handle in every lambda of a chain; when the last one is destroyed the count
// drops and the CLI reports idle. This is deliberately a refcount rather than a
// flag: uploadToFlipper takes one of its own while running inside cp -r, which
// already holds one.
std::shared_ptr<void> FlipperCli::holdBusy()
{
    ++m_opDepth;
    scheduleBusyChanged();
    QPointer<FlipperCli> self(this);
    // Stamped with the generation it was issued in. Ctrl-C and the watchdog
    // zero the count and bump the generation; the abandoned lambdas of the
    // cancelled operation die some time later, and without this stamp their
    // deleters would decrement the count belonging to whatever started next.
    const quint64 gen = m_opGen;
    return std::shared_ptr<void>(reinterpret_cast<void *>(1), [self, gen](void *) {
        if (!self || self->m_opGen != gen) { return; }
        if (self->m_opDepth > 0) { --self->m_opDepth; }
        self->scheduleBusyChanged();
    });
}

// Hold the line idle for a beat. After a Ctrl-C or a forced abort the firmware
// still has a prompt (and sometimes the tail of a cancelled reply) to push down
// the wire; sending the next command straight into that stream leaves its reply
// buffer pre-loaded with someone else's output, which is how one recovered
// command went on to fail for reasons that had nothing to do with it.
void FlipperCli::settle(int ms)
{
    auto hold = holdBusy();
    QTimer::singleShot(ms, this, [hold]() { /* releasing the hold is the point */ });
}

// After "storage write_chunk <path> <n>" the firmware answers "Ready" and then
// reads exactly n bytes off the wire, with no timeout and no way to say no. If
// we abandon the upload without sending those bytes, every subsequent command
// is silently swallowed as file content -- the device looks dead and stays that
// way until it is unplugged. Ctrl-C is the only way out.
void FlipperCli::abortPendingChunk()
{
    if (!m_port) { return; }
    m_port->write("\x03");     // cancel the pending write_chunk read loop
    m_port->write("\r\n");
    m_port->flush();
    m_port->clear(QSerialPort::Input);
}

void FlipperCli::armGuard()
{
    if (!m_opGuard) {
        m_opGuard = new QTimer(this);
        m_opGuard->setSingleShot(true);
        m_opGuard->setInterval(8000);
        connect(m_opGuard, &QTimer::timeout, this, &FlipperCli::onOpTimeout);
    }
    m_opGuard->start();
    scheduleBusyChanged();   // busy() now reflects the armed guard
}

void FlipperCli::disarmGuard()
{
    const bool wasActive = m_opGuard && m_opGuard->isActive();
    if (m_opGuard) { m_opGuard->stop(); }
    // A command just finished (the device's prompt came back). busy() folds in
    // the guard, so tell listeners it flipped -- this is what lets the paste
    // queue send the next command the instant this one completes instead of
    // waiting on a timer, and it's why a long-output command like `locate`
    // no longer wedges the whole queue behind it.
    if (wasActive) { scheduleBusyChanged(); }
}

// Commands typed or pasted while something is running are held here rather than
// bounced, so a pasted block runs to completion in order no matter how the UI
// feeds it in. Bounded: a runaway producer must not grow this without limit.
void FlipperCli::queuePending(const QString &cmd)
{
    if (m_pending.size() >= 500) {
        appendOutput(QStringLiteral("[ queue full -- dropping: %1 ]\n").arg(cmd));
        return;
    }
    m_pending += cmd;
    armQueueStall();
}

void FlipperCli::drainPending()
{
    if (m_draining || m_pending.isEmpty()) { return; }
    // Nothing is ever going to run these: the link is gone.
    if (!m_port || !m_active) { clearPending(); return; }
    if (busy()) { return; }

    m_draining = true;
    const QString next = m_pending.takeFirst();
    send(next);
    m_draining = false;

    // One command per turn of the event loop, posted rather than looped.
    //
    // The loop this replaces ran the whole queue inside a single turn whenever
    // the commands finished locally (pwd, history, a usage error, the POSIX
    // stubs). Forty of those meant forty appendOutput passes with no repaint
    // and no chance to service the serial port in between -- the window locked
    // up and then every line landed at once, which is what a pasted block felt
    // like it was doing.
    //
    // Posting is also what keeps the queue moving at all: a locally-handled
    // command never touches the port, so it never produces the busyChanged that
    // would otherwise pump the next one.
    if (!m_pending.isEmpty()) {
        setStatus(QStringLiteral("CLI live -- %1   (%2 queued)").arg(m_cwd).arg(m_pending.size()));
        armQueueStall();
        QTimer::singleShot(0, this, [this]() { drainPending(); });
    } else {
        if (m_queueStall) { m_queueStall->stop(); }
        if (m_active) { setStatus(QStringLiteral("CLI live -- %1").arg(m_cwd)); }
    }
}

// Last-resort recovery. The op guard only covers a command that is actually in
// flight; a busy token stranded with nothing on the wire is invisible to it, and
// that is precisely the state that used to need the panel closed and reopened.
// This watches the one symptom that is never legitimate: commands waiting, and
// not one byte from the device for half a minute.
void FlipperCli::armQueueStall()
{
    if (m_pending.isEmpty()) {
        if (m_queueStall) { m_queueStall->stop(); }
        return;
    }
    if (!m_queueStall) {
        m_queueStall = new QTimer(this);
        m_queueStall->setSingleShot(true);
        m_queueStall->setInterval(30000);
        connect(m_queueStall, &QTimer::timeout, this, &FlipperCli::onQueueStall);
    }
    m_queueStall->start();
}

void FlipperCli::onQueueStall()
{
    if (m_pending.isEmpty()) { return; }
    if (!busy()) { drainPending(); return; }   // it was only ever a missed nudge

    appendOutput(QStringLiteral("[ nothing has moved for 30s with %1 command%2 waiting -- releasing the line ]\n")
                     .arg(m_pending.size())
                     .arg(m_pending.size() == 1 ? QString() : QStringLiteral("s")));
    // resetTransientState cancels a pending write_chunk, zeroes the operation
    // count and bumps the generation, so whatever was holding the line lets go
    // and its orphaned continuations can no longer interfere.
    resetTransientState();
    if (m_port) { m_port->clear(); }
    settle(300);
}

void FlipperCli::clearPending()
{
    if (m_pending.isEmpty()) { return; }
    const int n = m_pending.size();
    m_pending.clear();
    if (m_queueStall) { m_queueStall->stop(); }
    appendOutput(QStringLiteral("[ dropped %1 queued command%2 ]\n")
                     .arg(n).arg(n == 1 ? QString() : QStringLiteral("s")));
}

// Everything that is only true for the duration of one operation. Ctrl-C, the
// watchdog and disconnect all need exactly this set cleared, and keeping three
// copies of the list in sync is how one of them ends up missing a field.
void FlipperCli::resetTransientState()
{
    // Order matters: if the firmware is waiting on write_chunk bytes, cancel
    // that BEFORE clearing m_xfer, otherwise nothing left knows it has to.
    if (m_xfer == Xfer::UploadReady) { abortPendingChunk(); }
    disarmGuard();
    // The holds themselves live in lambdas we are about to drop on the floor.
    // Zero the count so the CLI doesn't report busy forever after one cancelled
    // operation, and bump the generation so those orphaned deleters, whenever
    // they finally run, can tell they no longer own anything.
    m_opDepth = 0;
    ++m_opGen;
    scheduleBusyChanged();
    m_cdPending.clear();
    m_cdRaw.clear();
    m_xfer = Xfer::None;
    m_xferRaw.clear();
    m_xferPayload.clear();
    m_xferSize = -1;
    m_rawCb = nullptr;
    m_xferChain = nullptr;
    if (m_captureFlush) { m_captureFlush->stop(); }
    m_capture = Capture::None;
    m_captureBuf.clear();
    m_echoPending.clear();
    m_escTail.clear();
    m_dirsEnsured.clear();
    m_swallowPrompt = false;
    m_quiet = false;
}

void FlipperCli::onOpTimeout()
{
    const Xfer what = m_xfer;
    auto rawCb      = m_rawCb;
    auto chain      = m_xferChain;

    // Take the continuations before the reset so the reset can't drop them.
    m_rawCb = nullptr;
    m_xferChain = nullptr;
    // A half-arrived listing is still worth showing.
    if (m_capture != Capture::None) { flushCapture(); }
    // If the timeout caught an upload, the firmware is very likely parked in
    // write_chunk waiting for bytes. resetTransientState sends the Ctrl-C that
    // frees it -- without that, this timeout is the LAST thing this session
    // ever reports, because every command after it becomes file content.
    resetTransientState();

    // Drain whatever half-reply is still sitting in the serial buffer. If the
    // device recovers, the next command must start from a clean line -- leftover
    // bytes from the timed-out command would otherwise prepend garbage to it and
    // desync every command after (which is how one stuck command cascaded into
    // "no reply" for the whole rest of a pasted run).
    if (m_port) { m_port->clear(); }
    settle(300);

    // Whoever was waiting has to be told, or a cp -r / rm -r queue simply stops
    // halfway with nothing on screen. The sentinel is worded as a storage error
    // because that is already how every continuation tests for failure.
    static const QString kNoReply = QStringLiteral("Storage error: no reply from the device");

    if (what == Xfer::Download || what == Xfer::UploadReady) {
        m_xferChain = chain;   // finishXfer advances the batch if there is one
        finishXfer(false, QStringLiteral("[ no reply from the Flipper -- transfer aborted ]"));
        return;
    }
    if (rawCb) {
        // This continuation may be one step of a batch that still owns the
        // chain, so hand it back before running it -- otherwise the queue is
        // orphaned and stalls silently, which is the failure this whole
        // watchdog exists to prevent.
        m_xferChain = chain;
        rawCb(kNoReply);
        return;
    }
    if (chain) { chain(false); return; }
    cliLogFail(QStringLiteral("%1 -- no reply from the Flipper").arg(m_lastTyped));
    appendOutput(QStringLiteral("[ no reply from the Flipper -- giving up ]\n") + prompt());
}

void FlipperCli::writeLine(const QString &cmd, bool logIt)
{
    if (!m_port) { return; }
    if (logIt) { trace(cmd); }
    m_port->write(cmd.toUtf8());
    m_port->write("\r\n");
}

// ---- clipboard -------------------------------------------------------------
QString FlipperCli::clipboardText() const
{
    QClipboard *cb = QGuiApplication::clipboard();
    return cb ? cb->text() : QString();
}

void FlipperCli::copyToClipboard(const QString &text) const
{
    QClipboard *cb = QGuiApplication::clipboard();
    if (cb) { cb->setText(text); }
}

// ---- Tab completion --------------------------------------------------------
// The first word completes against every command name this CLI answers to;
// anything after it completes against a real directory listing -- the Flipper's
// if the path lives there, this computer's for "~/..." and other host paths.
void FlipperCli::complete(const QString &line)
{
    // Anything mid-flight owns the port; completing now would inject a listing
    // into the middle of a transfer.
    if (!m_port || !m_active || busy()) {
        emit completion(line);
        return;
    }

    // Quote-aware: the token under the caret can legitimately contain spaces
    // ("/ext/badusb/Bad USB"), and walking back to the nearest whitespace
    // completed only the fragment after the last space.
    const int start = cliTokenStart(line);
    const QString head = line.left(start);
    const QStringList tail = cliTokenize(line.mid(start));
    const QString token = tail.isEmpty() ? QString() : tail.first();

    if (head.trimmed().isEmpty()) {
        // Nothing typed yet to narrow the command name by: a real shell's
        // first Tab on a blank prompt does nothing rather than dumping every
        // command it knows, so this stays a plain Tab too. Type a letter and
        // it completes normally, same as Linux.
        if (token.isEmpty()) { emit completion(line); return; }
        QStringList hits;
        for (const QString &n : cliAllCommandNames()) {
            if (n.startsWith(token, Qt::CaseInsensitive)) { hits += n; }
        }
        applyCompletion(head, token, hits, QList<bool>(), line);
        return;
    }

    const QStringList headTokens = cliTokenize(head);
    const CliMatch verbMatch = headTokens.isEmpty() ? CliMatch() : cliMatch(headTokens.first());

    // A handful of panel commands take a fixed vocabulary, not a path -- no
    // file listing, on either machine, is ever the right answer for them.
    // Without this "help " + Tab fell through to the generic path logic
    // below, and since "help" has no Flipper spelling (so it's never
    // f-prefixed) that logic's default is "this computer", which is how Tab
    // ended up listing the user's home folder for a command that takes a
    // command name.
    if (verbMatch && verbMatch.def->id == CliVerb::Help) {
        QStringList hits;
        for (const QString &n : cliAllCommandNames()) {
            if (n.startsWith(token, Qt::CaseInsensitive)) { hits += n; }
        }
        applyCompletion(head, token, hits, QList<bool>(), line);
        return;
    }
    if (verbMatch && (verbMatch.def->id == CliVerb::Colors || verbMatch.def->id == CliVerb::Verbose)) {
        QStringList hits;
        for (const QString &n : { QStringLiteral("on"), QStringLiteral("off") }) {
            if (n.startsWith(token, Qt::CaseInsensitive)) { hits += n; }
        }
        applyCompletion(head, token, hits, QList<bool>(), line);
        return;
    }

    const int slash = token.lastIndexOf(QLatin1Char('/'));
    const QString dirPart = (slash >= 0) ? token.left(slash + 1) : QString();   // keeps its '/'
    const QString base    = (slash >= 0) ? token.mid(slash + 1) : token;

    // Which machine the completion lists depends on the COMMAND being typed,
    // not just the shape of the token under the caret -- cliIsHostPath() alone
    // used to answer this, and a bare relative token (the common case: no
    // leading '/' or '~') never looks like a host path, so completion fell
    // through to the Flipper for every command, including "open" and other
    // this-computer-only ones typed without their f-prefixed twin.
    //
    // Mirrors dispatch()'s own routing: cp, chmod and edit are the only
    // genuinely two-sided commands, and all three decide by the shape of the
    // path itself there too, f-prefixed or not. An f-prefixed spelling
    // otherwise always means the Flipper; every other bare spelling -- ls,
    // cd, open, host, ping, whatever -- means this computer, full stop.
    bool wantsHost;
    if (verbMatch && (verbMatch.def->id == CliVerb::Cp || verbMatch.def->id == CliVerb::Chmod
                       || verbMatch.def->id == CliVerb::Edit)) {
        // Genuinely two-sided: the shape of the path decides, same as
        // dispatch, regardless of which spelling was typed.
        wantsHost = cliIsHostPath(dirPart.isEmpty() ? token : dirPart);
    } else if (verbMatch && (verbMatch.def->id == CliVerb::Sed || verbMatch.def->id == CliVerb::Echo
                              || verbMatch.def->id == CliVerb::Wget)) {
        // Their target file lives on the Flipper in dispatch's own handling
        // no matter which spelling was typed (sed/echo's redirect, wget's
        // download destination) -- there is no host-side form to complete.
        wantsHost = false;
    } else if (!verbMatch) {
        wantsHost = cliIsHostPath(dirPart.isEmpty() ? token : dirPart);
    } else if (verbMatch.flipper) {
        wantsHost = false;
    } else {
        wantsHost = true;
    }

    // Host side: plain QDir, no round trip.
    if (wantsHost) {
        const QString absDir = dirPart.isEmpty() ? m_hostCwd
                                                 : cliExpandHostPath(dirPart, m_hostCwd);
        QStringList hits;
        QList<bool> isDir;
        const QFileInfoList entries = QDir(absDir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                                 QDir::Name | QDir::DirsFirst);
        for (const QFileInfo &fi : entries) {
            if (!fi.fileName().startsWith(base, Qt::CaseInsensitive)) { continue; }
            hits  += dirPart + fi.fileName();
            isDir += fi.isDir();
        }
        applyCompletion(head, token, hits, isDir, line);
        return;
    }

    // Tab's own lookup is machinery the user never asked for -- it stays out of
    // the verbose log even when everything else goes in.
    m_quiet = true;
    const QString absDir = cliResolvePath(m_cwd, dirPart.isEmpty() ? QStringLiteral(".") : dirPart);
    sendRaw(QStringLiteral("storage list ") + absDir,
            [this, head, token, dirPart, base, line](const QString &raw) {
        m_quiet = false;
        static const QRegularExpression rowRe(QStringLiteral("^\\[([DF])\\]\\s+(.*?)(?:\\s+(\\d+)b)?$"));
        QStringList hits;
        QList<bool> isDir;
        for (const QString &l : raw.split(QLatin1Char('\n'))) {
            const auto m = rowRe.match(l.trimmed());
            if (!m.hasMatch()) { continue; }
            const QString name = m.captured(2);
            if (!name.startsWith(base, Qt::CaseInsensitive)) { continue; }
            hits  += dirPart + name;
            isDir += (m.captured(1) == QLatin1String("D"));
        }
        applyCompletion(head, token, hits, isDir, line);
    });
}

void FlipperCli::applyCompletion(const QString &head, const QString &token,
                                 const QStringList &hits, const QList<bool> &isDir,
                                 const QString &original)
{
    if (hits.isEmpty()) { emit completion(original); return; }

    if (hits.size() == 1) {
        const bool dir = isDir.value(0, false);
        // Requoted on the way back out: completing into a name with a space in
        // it and handing back a bare string means the next Enter re-splits it
        // into two arguments.
        emit completion(head + cliQuoteArg(hits.first()) + (dir ? QString() : QStringLiteral(" ")));
        return;
    }

    // Several candidates: fill in as far as they all agree, then show the list
    // the way a shell does -- reprinting the line underneath so the caret keeps
    // its place.
    QString common = hits.first();
    for (const QString &h : hits) {
        int i = 0;
        while (i < common.size() && i < h.size() && common.at(i) == h.at(i)) { ++i; }
        common.truncate(i);
    }
    if (common.size() <= token.size()) { common = token; }
    // An empty common prefix only happens with an empty token and candidates
    // that share no letters at all (e.g. Tab right after "cd " in a folder
    // full of unrelated names) -- cliQuoteArg("") turns that into a literal
    // pair of quote marks with nothing between them, inserted into the line
    // out of nowhere. Nothing to fill in yet, so leave the line alone; the
    // grid below still shows what is there.
    const QString typed = common.isEmpty() ? QString() : cliQuoteArg(common);

    QStringList shown;
    for (int i = 0; i < hits.size(); ++i) {
        const QString name = hits.at(i).section(QLatin1Char('/'), -1);
        shown += isDir.value(i, false) ? (name + QLatin1Char('/')) : name;
    }
    int colW = 0;
    for (const QString &s : shown) { colW = qMax(colW, s.size()); }
    colW += 2;
    const int cols = qMax(1, 76 / qMax(1, colW));
    QString grid;
    for (int i = 0; i < shown.size(); ++i) {
        QString cell = shown.at(i);
        const bool last = ((i % cols) == cols - 1) || (i == shown.size() - 1);
        if (!last) { while (cell.size() < colW) { cell += QLatin1Char(' '); } }
        grid += cell;
        if (last) { grid += QLatin1Char('\n'); }
    }

    appendOutput(original + QLatin1Char('\n') + grid + prompt());
    emit completion(typed.isEmpty() ? original : (head + typed));
}

void FlipperCli::interrupt()
{
    if (m_interactiveProc) {
        // A live host session (python3 with no script, started by
        // startInteractiveSession()) is what Ctrl-C/Escape kills here, the
        // same as a real terminal would -- it's a local subprocess, nothing
        // to do with the Flipper's serial line, so none of the device-side
        // handling below applies to it. The process's own finished handler
        // (kill() makes it a CrashExit) prints "session ended" plus the
        // prompt once it actually stops, so this doesn't do that itself.
        m_interactiveProc->kill();
        appendOutput(QStringLiteral("^C\n"));
        return;
    }
    if (!m_port || !m_active) { return; }
    m_port->write("\x03");   // Ctrl-C
    m_port->flush();

    // Anything the user queued behind the thing they just cancelled was queued
    // on the assumption it would run after it, not instead of it. Running it
    // now against a half-finished state is how a cancelled rm -rf is followed
    // by a mkdir that reports "already exists".
    clearPending();

    // Drop every half-finished capture. Without this, a Ctrl-C landing in the
    // middle of a help listing or a file transfer leaves the panel buffering
    // forever and nothing reaches the screen again.
    resetTransientState();
    if (m_port) { m_port->clear(QSerialPort::Input); }

    appendOutput(QStringLiteral("^C\n") + prompt());
}

void FlipperCli::onReadyRead()
{
    if (!m_port) { return; }
    const QByteArray chunk = m_port->readAll();
    if (!chunk.isEmpty()) { m_sawDeviceBytes = true; }

    // Bytes arriving mean the device is still talking, so the no-reply
    // watchdog only ever fires on real silence.
    if (m_opGuard && m_opGuard->isActive()) { m_opGuard->start(); }
    if (m_queueStall && m_queueStall->isActive()) { m_queueStall->start(); }

    // ---- raw transfer phases (must not go through the text sanitiser) -------
    if (!m_cdPending.isEmpty()) {
        m_cdRaw += chunk;
        const QString s = QString::fromUtf8(m_cdRaw);
        if (!s.contains(QLatin1String(">:")) && m_cdRaw.size() < 4096) { return; }
        // "Directory" for a folder; a storage root answers with the volume line
        // instead, and that is just as valid a place to stand.
        if (s.contains(QLatin1String("Directory"))
            || s.contains(QLatin1String("Storage, label"))
            || s.contains(QLatin1String("Total space"))) {
            m_cdPrev = m_cwd;
            m_cwd = m_cdPending;
            emit promptChanged();
            setStatus(QStringLiteral("CLI live -- %1").arg(m_cwd));
            appendOutput(prompt());
        } else if (s.contains(QLatin1String("File"))) {
            appendOutput(QStringLiteral("[ not a folder: %1 ]\n").arg(m_cdPending) + prompt());
        } else {
            appendOutput(QStringLiteral("[ no such folder: %1 ]\n").arg(m_cdPending) + prompt());
        }
        m_cdPending.clear();
        m_cdRaw.clear();
        disarmGuard();
        return;
    }

    // Generic one-shot command (sendRaw): buffer until the prompt marker, then
    // hand the raw text to whoever asked for it. Everything that isn't a
    // payload transfer -- md5 checks, tree/list scans, mkdir, remove -- goes
    // through here.
    if (m_xfer == Xfer::Raw) {
        m_xferRaw += chunk;
        const QString s = QString::fromUtf8(m_xferRaw);
        if (!s.contains(QLatin1String(">:")) && m_xferRaw.size() < 300000) { return; }
        m_xfer = Xfer::None;
        m_xferRaw.clear();
        // Stop the watchdog WITHOUT announcing it. disarmGuard() would post
        // busyChanged, and the continuation below is usually the next step of a
        // multi-step operation -- announcing idle first invited the queue to
        // interleave a command into the middle of it.
        if (m_opGuard) { m_opGuard->stop(); }
        auto cb = m_rawCb;
        m_rawCb = nullptr;
        if (cb) { cb(s); }        // may re-arm the guard for its own next step
        scheduleBusyChanged();
        return;
    }

    if (m_xfer == Xfer::UploadReady) {
        const CliXferStep st = cliUploadFeed(m_xferRaw, chunk, m_xferPayload, m_xferLabel);
        if (!st.done) { return; }
        if (st.failed) {
            // The write_chunk line already went out and the firmware may
            // already be counting down n bytes. Cancel it before anything else
            // is allowed near the port.
            abortPendingChunk();
            m_xfer = Xfer::None;
            m_xferPayload.clear();
            disarmGuard();
            settle(300);   // let the Ctrl-C's prompt land before the next command
            finishXfer(false, st.message);
            return;
        }
        // The payload goes out FIRST, before m_xfer is cleared and before the
        // guard is disarmed. Both of those make the CLI look idle, and a
        // command that reached the port ahead of these bytes was read by the
        // firmware as the file's contents -- which is exactly how a file
        // written by `echo hello world > a.txt` ended up containing the text
        // of the next command instead.
        if (!st.toWrite.isEmpty()) {
            m_port->write(st.toWrite);
            m_port->flush();
        }
        // Having answered "Ready" the firmware prints a fresh prompt once the
        // chunk is done. It arrives before the verification has even been sent,
        // so on screen the command looked finished while its own check was
        // still to come. Drop that one prompt; finishXfer prints the real one.
        // Set unconditionally: a zero-byte file writes no payload at all but
        // still gets the prompt, which is why `touch` kept leaking one.
        m_swallowPrompt = true;
        // write_chunk gives no per-byte ack, so "storage md5" against a local
        // hash is the only real proof the bytes weren't clipped on the wire.
        // The hold keeps the CLI busy across that round trip.
        auto hold = holdBusy();
        m_xfer = Xfer::None;
        disarmGuard();
        const QByteArray payload = m_xferPayload;
        m_xferPayload.clear();
        const QString devPath = m_xferDevPath;
        const QString baseMsg = st.message;
        QTimer::singleShot(150, this, [this, payload, devPath, baseMsg, hold]() {
            // An empty file has no meaningful md5 round trip on some firmware
            // builds ("storage md5" on a zero-byte file answers with an error
            // or a blank line), which is what reported a perfectly good `touch`
            // as a corrupt transfer. Confirm it exists and is zero bytes.
            if (payload.isEmpty()) {
                sendRaw(QStringLiteral("storage stat ") + devPath, [this, baseMsg, hold](const QString &raw) {
                    const bool ok = !raw.contains(QLatin1String("Storage error"));
                    finishXfer(ok, ok ? baseMsg + QStringLiteral(" [empty file created]")
                                      : baseMsg + QStringLiteral(" [the file was not created]"));
                });
                return;
            }
            sendRaw(QStringLiteral("storage md5 ") + devPath, [this, payload, baseMsg, hold](const QString &raw) {
                const QString got = cliExtractMd5(raw);
                const QString want = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Md5).toHex());
                const bool ok = !got.isEmpty() && got == want;
                finishXfer(ok, ok ? baseMsg + QStringLiteral(" [md5 ok]")
                                   : baseMsg + QStringLiteral(" [md5 MISMATCH -- transfer is suspect]"));
            });
        });
        return;
    }

    if (m_xfer == Xfer::Download) {
        const CliXferStep st = cliDownloadFeed(m_xferRaw, m_xferSize, chunk, m_xferHostDst);
        if (!st.done) { return; }
        disarmGuard();
        m_xfer = Xfer::None;
        if (st.failed) {
            // Whatever is still in the buffer belongs to the command that just
            // failed. Left there it becomes the next command's reply.
            if (m_port) { m_port->clear(QSerialPort::Input); }
            settle(200);
            finishXfer(false, st.message);
            return;
        }

        const QByteArray body = st.body;
        const QString devPath = m_xferDevPath;
        const QString hostDst = m_xferHostDst;

        // An empty file has no meaningful md5 round trip on this firmware -- the
        // same thing that reported every `touch` as a corrupt transfer. There is
        // also nothing to verify: zero bytes either arrived or they didn't.
        if (body.isEmpty()) {
            QFile out(hostDst);
            if (!out.open(QIODevice::WriteOnly)) {
                finishXfer(false, QStringLiteral("[ can't write %1: %2 ]").arg(hostDst, out.errorString()));
                return;
            }
            out.close();
            finishXfer(true, QStringLiteral("[ saved 0 bytes -> %1 ]").arg(hostDst));
            return;
        }

        auto hold = holdBusy();   // keeps the CLI busy across the md5 round trip
        sendRaw(QStringLiteral("storage md5 ") + devPath, [this, body, hostDst, hold](const QString &raw) {
            const QString got = cliExtractMd5(raw);
            const QString want = QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Md5).toHex());
            if (got.isEmpty() || got != want) {
                finishXfer(false, QStringLiteral("[ %1 -- md5 MISMATCH, file NOT saved ]").arg(hostDst));
                return;
            }
            QFile out(hostDst);
            if (!out.open(QIODevice::WriteOnly)) {
                finishXfer(false, QStringLiteral("[ can't write %1: %2 ]").arg(hostDst, out.errorString()));
                return;
            }
            out.write(body);
            out.close();
            finishXfer(true, QStringLiteral("[ saved %1 bytes -> %2 [md5 ok] ]").arg(body.size()).arg(hostDst));
        });
        return;
    }

    QString text = QString::fromUtf8(chunk);

    // An escape sequence can be split across two serial reads. When that
    // happened the regex below saw only "\x1B[0" (no final byte) and left it
    // alone, the control-character sweep then ate the lone ESC, and the "[0m"
    // remainder was printed as text -- the stray "[0m" after the help listing.
    // Hold an unterminated tail back and glue it onto the next chunk instead.
    static const QRegularExpression ansiWhole(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]"));
    if (!m_escTail.isEmpty()) { text.prepend(m_escTail); m_escTail.clear(); }
    {
        const int e = text.lastIndexOf(QChar(0x1B));
        if (e >= 0) {
            const QString tail = text.mid(e);
            // Bounded: a sequence this long is not one, and holding bytes back
            // forever would stall the view.
            if (tail.size() < 16 && !ansiWhole.match(tail).hasMatch()) {
                m_escTail = tail;
                text.chop(tail.size());
            }
        }
    }

    // Strip ANSI escape sequences (colours, cursor moves) for a clean text view.
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]"));
    text.remove(ansi);
    text.remove(QLatin1Char('\r'));
    // Strip stray C0/C1 control characters -- this is the little square the
    // firmware emits next to the prompt.
    static const QRegularExpression ctrl(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]"));
    text.remove(ctrl);

    // One bare prompt owed to us from an upload's payload (see Xfer::UploadReady).
    if (m_swallowPrompt) {
        const QString t = text.trimmed();
        if (t.isEmpty() || t == QLatin1String(">:")) {
            if (!t.isEmpty()) { m_swallowPrompt = false; }
            return;
        }
        m_swallowPrompt = false;
    }

    // Eat the echo of a translated command, character by character so a chunk
    // boundary in the middle of it doesn't leak the translation onto the screen.
    if (!m_echoPending.isEmpty()) {
        int i = 0;
        while (i < text.size() && !m_echoPending.isEmpty()) {
            const QChar c = text.at(i);
            if (c == m_echoPending.at(0)) { m_echoPending.remove(0, 1); ++i; }
            else if (c == QLatin1Char('\n') && m_echoPending.at(0) == QLatin1Char('\n')) { ++i; }
            else { m_echoPending.clear(); break; }   // not our echo -- leave it alone
        }
        text = text.mid(i);
        if (text.isEmpty()) { return; }
    }

    // Swap the firmware's bare ">: " for our shell-style prompt.
    text.replace(QLatin1String(">: "), prompt());
    if (text.endsWith(QLatin1String(">:"))) { text.chop(2); text += prompt(); }

    // The firmware streams its help listing across several serial chunks, so it
    // can't be reformatted chunk by chunk -- that's what left our shortcuts on a
    // different grid. Hold the whole listing back until the prompt returns, then
    // lay the entire thing out at once.
    // Both spellings: stock firmware says "Commands available:", Unleashed and
    // Momentum say "Available commands:". Matching only the first one meant the
    // whole help reformatter never ran on a fork -- which is why the listing
    // came through raw, with none of our own commands in it.
    static const QRegularExpression helpHdr(
        QStringLiteral("(?:commands available|available commands)\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    if (m_capture == Capture::None && helpHdr.match(text).hasMatch()) {
        m_capture = Capture::Help;
    }
    if (m_capture != Capture::None) {
        m_captureBuf += text;
        const QString p = prompt();
        const bool done = m_captureBuf.contains(p)
                          || m_captureBuf.contains(QLatin1String("Storage error"))
                          || m_captureBuf.size() > 16384;
        if (done) {
            flushCapture();
        } else {
            if (!m_captureFlush) {
                m_captureFlush = new QTimer(this);
                m_captureFlush->setSingleShot(true);
                connect(m_captureFlush, &QTimer::timeout, this, &FlipperCli::flushCapture);
            }
            m_captureFlush->start(500);   // safety net if the prompt never lands
        }
        return;
    }

    // "Storage error: file/dir not exist" and friends come back as plain text
    // on the serial line, so this is the only place they can be caught. The
    // guard stops a reply split across chunks from logging twice.
    if (text.contains(QLatin1String("Storage error"))) {
        const QString err = text.section(QLatin1String("Storage error"), 1, 1)
                                .section(QLatin1Char('\n'), 0, 0).trimmed();
        const QString line = QStringLiteral("%1 -- Storage error%2").arg(m_lastTyped, err);
        if (line != m_lastLoggedError) {
            m_lastLoggedError = line;
            cliLogFail(line);
        }
    }

    // A non-captured command finished when the prompt comes back in its output.
    // Disarm here so busy() clears immediately (the paste queue advances) rather
    // than waiting out the 8s watchdog. Captured commands disarm in flushCapture.
    if (m_capture == Capture::None && text.contains(prompt())) {
        disarmGuard();
    }

    appendOutput(text);
}

QString FlipperCli::prompt() const
{
    return cliPromptFor(m_devName, m_cwd, m_hostCwd);
}

void FlipperCli::flushCapture()
{
    if (m_capture == Capture::None) { return; }
    if (m_captureFlush) { m_captureFlush->stop(); }
    disarmGuard();
    const Capture what = m_capture;
    m_capture = Capture::None;
    if (m_captureBuf.isEmpty()) { return; }

    const QString buf = m_captureBuf;
    m_captureBuf.clear();

    // A listing that errors ends the capture on the error text, which arrives
    // BEFORE the firmware's prompt does -- so the flush printed the error and
    // no prompt, and the next command's echo ran straight onto the same line.
    // Print our own prompt and eat the real one when it turns up.
    const bool sawPrompt = buf.contains(prompt().trimmed());
    if (!sawPrompt && buf.contains(QLatin1String("Storage error"))) {
        const QString err = buf.section(QLatin1String("Storage error"), 1, 1)
                               .section(QLatin1Char('\n'), 0, 0)
                               .section(QLatin1Char(':'), 1).trimmed();
        // Same framing cat and the rest use, instead of the bare firmware line.
        appendOutput(QStringLiteral("[ %1 ]\n").arg(err.isEmpty() ? QStringLiteral("can't read that path") : err)
                     + prompt());
        m_swallowPrompt = true;
        return;
    }

    appendOutput(what == Capture::Help ? cliFormatHelp(buf, prompt().trimmed())
                                      : cliFormatListing(buf));
}

// ---- host <-> Flipper file transfer over the plain CLI ----------------------
void FlipperCli::uploadToFlipper(const QString &hostPath, const QString &devPath, bool exactDest)
{
    // Every bail-out below goes through finishXfer rather than printing and
    // returning. A cp -r sets m_xferChain before calling in, so a plain return
    // left the queue waiting on a continuation that never came: the remaining
    // files silently never copied and no summary was ever printed.
    if (!m_port || !m_active) {
        finishXfer(false, QStringLiteral("[ the CLI link is not open ]"));
        return;
    }

    const QFileInfo fi(hostPath);
    if (!fi.exists() || !fi.isFile()) {
        finishXfer(false, QStringLiteral("[ no such file on this computer: %1 ]").arg(hostPath));
        return;
    }
    QFile f(hostPath);
    if (!f.open(QIODevice::ReadOnly)) {
        finishXfer(false, QStringLiteral("[ can't read %1: %2 ]").arg(hostPath, f.errorString()));
        return;
    }
    const QByteArray data = f.readAll();
    f.close();

    // exactDest is set by the commands that build their payload in a temp file
    // (echo, touch, wget): there the host filename is a throwaway, and a
    // destination like /ext/notes has no dot in its last segment, so the
    // usual "looks like a folder" guess would bury the temp name inside it.
    const QString dst = exactDest ? devPath
                                  : cliJoinDest(devPath, fi.fileName(), cliLooksLikeDir(devPath));

    // The firmware reads write_chunk's payload in a fixed 1 KB loop rather than
    // malloc'ing the whole thing, so this cap is just a sane one-shot size for
    // the CLI link, not a memory ceiling. Anything past it still belongs to the
    // RPC file tools, which report progress.
    static const int kMaxChunk = 1 * 1024 * 1024;   // 1 MiB
    if (data.size() > kMaxChunk) {
        finishXfer(false, QStringLiteral("[ %1 is %2 bytes -- CLI uploads cap at %3. Close the CLI and use the file tools. ]")
                          .arg(fi.fileName()).arg(data.size()).arg(kMaxChunk));
        return;
    }

    m_xferDevPath = dst;
    appendOutput(QStringLiteral("[ uploading %1 (%2 bytes) -> %3 ]\n").arg(fi.fileName()).arg(data.size()).arg(dst));
    // storage write_chunk APPENDS to an existing file rather than replacing it
    // (FSOM_OPEN_APPEND), so a repeat "cp" would otherwise leave the file with
    // old and new content concatenated. Clear any previous copy first -- "not
    // found" here is expected and harmless.
    //
    // The hold spans the gap between the remove finishing and write_chunk going
    // out. That gap is a few milliseconds of a completely idle-looking CLI, and
    // it is where a queued command used to land -- ahead of the write_chunk, or
    // worse, ahead of the payload.
    auto hold = holdBusy();
    sendRaw(QStringLiteral("storage remove ") + dst, [this, data, dst, hold](const QString &pre) {
        if (!m_port || !m_active) {
            finishXfer(false, QStringLiteral("[ the CLI link dropped mid-upload ]"));
            return;
        }
        // sendRaw answers with this instead of running the command when the
        // line is already in use. Writing write_chunk anyway would put the
        // firmware into its byte-counting mode behind another command's back.
        if (pre.contains(QLatin1String("the CLI is busy"))) {
            finishXfer(false, QStringLiteral("[ the CLI line was busy -- upload not started ]"));
            return;
        }
        m_xfer = Xfer::UploadReady;
        m_xferRaw.clear();
        m_xferPayload = data;
        m_xferLabel = dst;
        // CR only. With "\r\n" the firmware takes the CR as Enter and then reads
        // the stray LF as the chunk's first byte -- that's the blank line that
        // showed up at the top of uploaded files.
        const QString wc = QStringLiteral("storage write_chunk %1 %2").arg(dst).arg(data.size());
        trace(wc);
        m_port->write(wc.toUtf8());
        m_port->write("\r");
        armGuard();
    });
}

void FlipperCli::downloadFromFlipper(const QString &devPath, const QString &hostPath)
{
    if (!m_port || !m_active) { return; }

    const QString name = devPath.section(QLatin1Char('/'), -1);
    QString dst = hostPath;
    if (dst.endsWith(QLatin1Char('/')) || QFileInfo(dst).isDir()) {
        dst = cliJoinDest(dst, name, true);
    }
    QDir().mkpath(QFileInfo(dst).absolutePath());

    m_xfer = Xfer::Download;
    m_xferRaw.clear();
    m_xferSize = -1;
    m_xferHostDst = dst;
    m_xferLabel = dst;
    m_xferDevPath = devPath;
    appendOutput(QStringLiteral("[ downloading %1 -> %2 ]\n").arg(devPath, dst));
    writeLine(QStringLiteral("storage read %1").arg(devPath));
    armGuard();
}

// Prints the result of one transfer -- unless a batch op is chained behind it,
// in which case the batch driver decides when the prompt finally comes back.
void FlipperCli::finishXfer(bool ok, const QString &message)
{
    m_xfer = Xfer::None;   // the transfer is over; busy() should reflect that
    m_swallowPrompt = false;
    if (!message.isEmpty()) {
        if (ok) { cliLog(message); } else { cliLogFail(message); }
    }
    if (m_xferChain) {
        auto cb = m_xferChain;
        m_xferChain = nullptr;
        appendOutput(message + QLatin1Char('\n'));
        cb(ok);
        scheduleBusyChanged();
        return;
    }
    appendOutput(message + QLatin1Char('\n') + prompt());
    scheduleBusyChanged();
}

// Generic one-shot command on the interactive port -- see the Xfer::Raw branch
// in onReadyRead. Refuses if a transfer or another raw command is already in
// flight rather than silently clobbering it.
void FlipperCli::sendRaw(const QString &cmd, std::function<void(const QString &)> onDone)
{
    // portBusy(), not busy(): every step of a composite operation runs while
    // that operation holds a busy token, and gating on busy() here would make
    // each of them refuse its own next step.
    if (!m_port || !m_active || portBusy()) {
        // Not an empty string: continuations test for failure with
        // contains("error"), so "" used to be read as a clean success.
        if (onDone) { onDone(QStringLiteral("Storage error: the CLI is busy")); }
        return;
    }
    m_xfer = Xfer::Raw;
    m_xferRaw.clear();
    m_rawCb = [this, onDone](const QString &raw) {
        traceReply(raw);
        if (onDone) { onDone(raw); }
    };
    writeLine(cmd);
    armGuard();
}

// mkdir -p: the firmware's own mkdir isn't recursive, so walk the path one
// segment at a time. "already exists" errors are expected and ignored.
void FlipperCli::ensureDeviceDir(const QString &path, std::function<void()> done)
{
    if (path.isEmpty() || path == QLatin1String("/ext") || path == QLatin1String("/int")) {
        if (done) { done(); }
        return;
    }
    auto queue = std::make_shared<QStringList>();
    QString acc;
    const QStringList segs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &s : segs) {
        acc += QLatin1Char('/') + s;
        // /ext and /int always exist and cannot be created; asking anyway put an
        // "already exist" error at the top of every single mkdir -p.
        if (cliIsStorageRoot(acc)) { continue; }
        // Already created during this batch. A cp -r of 30 files into one folder
        // re-issued the same mkdir 30 times and collected 30 "already exist"
        // errors -- two wasted round trips per file, and a log where the real
        // work was buried in noise.
        if (m_dirsEnsured.contains(acc)) { continue; }
        *queue += acc;
    }
    auto hold = holdBusy();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, queue, done, step, hold]() {
        if (queue->isEmpty()) {
            if (done) { done(); }
            // Break the self-reference. *step captures the shared_ptr that owns
            // it, so the chain keeps itself alive forever -- harmless when it
            // only leaked a few bytes, fatal now that the same lambda also
            // carries the busy token: the token was never destroyed, busy()
            // stayed true, and the command queue behind it stopped dead.
            // Deferred, because assigning to a std::function that is mid-call
            // would destroy the frame currently executing.
            QTimer::singleShot(0, this, [step]() { *step = nullptr; });
            return;
        }
        const QString dir = queue->takeFirst();
        sendRaw(QStringLiteral("storage mkdir ") + dir, [this, step, dir](const QString &) {
            // Recorded whether it was created or already there -- both mean it
            // exists now, which is all the next file needs to know.
            m_dirsEnsured.insert(dir);
            (*step)();
        });
    };
    (*step)();
}

// cp -r, computer -> Flipper: walk the local tree with QDirIterator (cheap,
// synchronous, local disk), mirror the folder structure on the SD card, and
// upload each file through the same verified single-file path used everywhere
// else -- chained one at a time via m_xferChain so the port is never shared.
void FlipperCli::startCopyUpTree(const QString &hostRoot, const QString &devRoot)
{
    m_dirsEnsured.clear();
    const QFileInfo fi(hostRoot);
    if (!fi.exists()) {
        appendOutput(QStringLiteral("[ no such file or folder on this computer: %1 ]\n").arg(hostRoot) + prompt());
        return;
    }
    QStringList files;
    if (fi.isDir()) {
        QDirIterator it(hostRoot, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { files += it.next(); }
    } else {
        files += fi.absoluteFilePath();
    }
    if (files.isEmpty()) {
        appendOutput(QStringLiteral("[ %1 has no files to copy ]\n").arg(hostRoot) + prompt());
        return;
    }
    std::sort(files.begin(), files.end());
    const QString base = fi.isDir() ? QDir(hostRoot).absolutePath() : QFileInfo(hostRoot).absolutePath();
    appendOutput(QStringLiteral("[ copying %1 file(s) -> %2 ]\n").arg(files.size()).arg(devRoot));

    auto queue = std::make_shared<QStringList>(files);
    auto ok = std::make_shared<int>(0);
    auto fail = std::make_shared<int>(0);
    auto hold = holdBusy();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, queue, ok, fail, base, devRoot, step, hold]() {
        if (queue->isEmpty()) {
            appendOutput(QStringLiteral("[ done -- %1 ok, %2 failed ]\n").arg(*ok).arg(*fail) + prompt());
            // Break the self-reference. *step captures the shared_ptr that owns
            // it, so the chain keeps itself alive forever -- harmless when it
            // only leaked a few bytes, fatal now that the same lambda also
            // carries the busy token: the token was never destroyed, busy()
            // stayed true, and the command queue behind it stopped dead.
            // Deferred, because assigning to a std::function that is mid-call
            // would destroy the frame currently executing.
            QTimer::singleShot(0, this, [step]() { *step = nullptr; });
            return;
        }
        const QString hostFile = queue->takeFirst();
        const QString rel = QDir(base).relativeFilePath(hostFile);
        const int slash = rel.lastIndexOf(QLatin1Char('/'));
        const QString devDir = (slash >= 0) ? (devRoot + QLatin1Char('/') + rel.left(slash)) : devRoot;
        m_xferChain = [ok, fail, step](bool success) {
            if (success) { ++(*ok); } else { ++(*fail); }
            (*step)();
        };
        ensureDeviceDir(devDir, [this, hostFile, devDir]() {
            uploadToFlipper(hostFile, devDir + QLatin1Char('/'));
        });
    };
    (*step)();
}

// cp -r, Flipper -> computer: "storage tree" already does the recursion, so
// just enumerate, mkpath the mirror on this side, and download each file.
void FlipperCli::startCopyDownTree(const QString &devRoot, const QString &hostRoot)
{
    appendOutput(QStringLiteral("[ scanning %1... ]\n").arg(devRoot));
    auto hold = holdBusy();
    sendRaw(QStringLiteral("storage tree ") + devRoot, [this, devRoot, hostRoot, hold](const QString &raw) {
        if (raw.contains(QLatin1String("Storage error"))) {
            appendOutput(QStringLiteral("[ can't read %1 on the Flipper ]\n").arg(devRoot) + prompt());
            return;
        }
        QStringList files;
        for (const auto &e : cliParseTree(raw)) { if (!e.isDir) { files += e.path; } }
        if (files.isEmpty()) {
            appendOutput(QStringLiteral("[ %1 has no files to copy ]\n").arg(devRoot) + prompt());
            return;
        }
        QDir().mkpath(hostRoot);
        appendOutput(QStringLiteral("[ copying %1 file(s) -> %2 ]\n").arg(files.size()).arg(hostRoot));

        auto queue = std::make_shared<QStringList>(files);
        auto ok = std::make_shared<int>(0);
        auto fail = std::make_shared<int>(0);
        auto step = std::make_shared<std::function<void()>>();
        *step = [this, queue, ok, fail, devRoot, hostRoot, step, hold]() {
            if (queue->isEmpty()) {
                appendOutput(QStringLiteral("[ done -- %1 ok, %2 failed ]\n").arg(*ok).arg(*fail) + prompt());
            // Break the self-reference. *step captures the shared_ptr that owns
            // it, so the chain keeps itself alive forever -- harmless when it
            // only leaked a few bytes, fatal now that the same lambda also
            // carries the busy token: the token was never destroyed, busy()
            // stayed true, and the command queue behind it stopped dead.
            // Deferred, because assigning to a std::function that is mid-call
            // would destroy the frame currently executing.
            QTimer::singleShot(0, this, [step]() { *step = nullptr; });
                return;
            }
            const QString devFile = queue->takeFirst();
            QString rel = devFile.startsWith(devRoot) ? devFile.mid(devRoot.size()) : devFile.section(QLatin1Char('/'), -1);
            while (rel.startsWith(QLatin1Char('/'))) { rel.remove(0, 1); }
            const QString hostFile = hostRoot + QLatin1Char('/') + rel;
            QDir().mkpath(QFileInfo(hostFile).absolutePath());
            m_xferChain = [ok, fail, step](bool success) {
                if (success) { ++(*ok); } else { ++(*fail); }
                (*step)();
            };
            downloadFromFlipper(devFile, hostFile);
        };
        (*step)();
    });
}

// Deletes "path" and, if a folder, everything under it. Children before
// parents: reversing "storage tree"'s listing order guarantees each child is
// gone before its parent. done(ok, existed) -- a path that was never there is
// a no-op, not a partial failure, and those read very differently.
void FlipperCli::removeTreeCore(const QString &path, std::function<void(bool, bool)> done)
{
    m_dirsEnsured.clear();   // anything cached may be about to stop existing
    // Plain remove FIRST -- covers a file or empty folder (the common case)
    // without a scan, instead of paying for a doomed "storage tree" per file.
    auto hold = holdBusy();
    sendRaw(QStringLiteral("storage remove ") + path, [this, path, done, hold](const QString &first) {
        if (!first.contains(QLatin1String("error"), Qt::CaseInsensitive)) {
            if (done) { done(true, true); }
            return;
        }
        // It failed: either the path is gone, or it is a folder with contents.
        // Only the second case is worth a recursive walk.
        sendRaw(QStringLiteral("storage tree ") + path, [this, path, done, hold](const QString &raw) {
        if (raw.contains(QLatin1String("Storage error"))) {
            // Both commands said no such path, so there was nothing to remove.
            const bool existed = !raw.contains(QLatin1String("not exist"));
            if (done) { done(false, existed); }
            return;
        }
        const auto entries = cliParseTree(raw);
        QStringList targets;
        for (const auto &e : entries) { if (!e.isDir) { targets += e.path; } }
        QStringList dirs;
        for (const auto &e : entries) { if (e.isDir) { dirs += e.path; } }
        std::reverse(dirs.begin(), dirs.end());
        targets += dirs;
        targets += path;   // the root itself, last

        auto queue = std::make_shared<QStringList>(targets);
        auto allOk = std::make_shared<bool>(true);
        auto step = std::make_shared<std::function<void()>>();
        *step = [this, queue, allOk, done, step, hold]() {
            if (queue->isEmpty()) {
                if (done) { done(*allOk, true); }
            // Break the self-reference. *step captures the shared_ptr that owns
            // it, so the chain keeps itself alive forever -- harmless when it
            // only leaked a few bytes, fatal now that the same lambda also
            // carries the busy token: the token was never destroyed, busy()
            // stayed true, and the command queue behind it stopped dead.
            // Deferred, because assigning to a std::function that is mid-call
            // would destroy the frame currently executing.
            QTimer::singleShot(0, this, [step]() { *step = nullptr; });
                return;
            }
            const QString t = queue->takeFirst();
            sendRaw(QStringLiteral("storage remove ") + t, [allOk, step](const QString &raw3) {
                if (raw3.contains(QLatin1String("error"), Qt::CaseInsensitive)) { *allOk = false; }
                (*step)();
            });
        };
        (*step)();
        });
    });
}

void FlipperCli::startRemoveTree(const QString &path)
{
    appendOutput(QStringLiteral("[ removing %1... ]\n").arg(path));
    removeTreeCore(path, [this, path](bool ok, bool existed) {
        if (!ok && !existed) {
            appendOutput(QStringLiteral("[ no such path: %1 ]\n").arg(path) + prompt());
            return;
        }
        QString note;
        // Standing inside what was just deleted leaves the prompt pointing at a
        // folder that no longer exists, and every relative path after it
        // resolves against nothing. Step out to the nearest surviving parent --
        // which is what a real shell would have forced you to do beforehand.
        if (ok && (m_cwd == path || m_cwd.startsWith(path + QLatin1Char('/')))) {
            QString up = path.section(QLatin1Char('/'), 0, -2);
            if (up.isEmpty()) { up = QStringLiteral("/ext"); }
            m_cdPrev = m_cwd;
            m_cwd = up;
            emit promptChanged();
            setStatus(QStringLiteral("CLI live -- %1").arg(m_cwd));
            note = QStringLiteral("[ that was the current folder -- moved to %1 ]\n").arg(up);
        }
        appendOutput((ok ? QStringLiteral("[ removed %1 ]\n").arg(path)
                          : QStringLiteral("[ some items under %1 could not be removed ]\n").arg(path))
                     + note + prompt());
    });
}

// Same as startRemoveTree but for a set of matches from a wildcard, with one
// summary at the end instead of one prompt per file.
void FlipperCli::runRemoveQueue(const QStringList &targets)
{
    auto queue = std::make_shared<QStringList>(targets);
    auto ok = std::make_shared<int>(0);
    auto fail = std::make_shared<int>(0);
    auto hold = holdBusy();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, queue, ok, fail, step, hold]() {
        if (queue->isEmpty()) {
            appendOutput(QStringLiteral("[ done -- %1 removed, %2 failed ]\n").arg(*ok).arg(*fail) + prompt());
            // Break the self-reference. *step captures the shared_ptr that owns
            // it, so the chain keeps itself alive forever -- harmless when it
            // only leaked a few bytes, fatal now that the same lambda also
            // carries the busy token: the token was never destroyed, busy()
            // stayed true, and the command queue behind it stopped dead.
            // Deferred, because assigning to a std::function that is mid-call
            // would destroy the frame currently executing.
            QTimer::singleShot(0, this, [step]() { *step = nullptr; });
            return;
        }
        const QString t = queue->takeFirst();
        removeTreeCore(t, [ok, fail, step](bool success, bool) {
            if (success) { ++(*ok); } else { ++(*fail); }
            (*step)();
        });
    };
    (*step)();
}

// Expands "*.sub" style patterns against one directory's listing (not
// recursive -- that's what "find" is for). Matches on the bare filename.
void FlipperCli::expandDeviceGlob(const QString &pattern, std::function<void(const QStringList &)> done)
{
    const int slash = pattern.lastIndexOf(QLatin1Char('/'));
    const QString dir  = (slash > 0) ? pattern.left(slash) : QStringLiteral("/ext");
    const QString glob = (slash >= 0) ? pattern.mid(slash + 1) : pattern;
    static const QRegularExpression rowRe(QStringLiteral("^\\[([DF])\\]\\s+(.*?)(?:\\s+(\\d+)b)?$"));
    const QRegularExpression rx(QRegularExpression::wildcardToRegularExpression(glob),
                                QRegularExpression::CaseInsensitiveOption);
    auto hold = holdBusy();
    sendRaw(QStringLiteral("storage list ") + dir, [dir, rx, done, hold](const QString &raw) {
        QStringList out;
        for (const QString &line : raw.split(QLatin1Char('\n'))) {
            const auto m = rowRe.match(line.trimmed());
            if (!m.hasMatch()) { continue; }
            const QString name = m.captured(2);
            if (rx.match(name).hasMatch()) { out += dir + QLatin1Char('/') + name; }
        }
        if (done) { done(out); }
    });
}

// The mirror image of runCopyQueue: a set of files on THIS computer, uploaded
// one at a time to one folder on the Flipper. Chained through m_xferChain like
// every other batch, so the port is never shared.
void FlipperCli::runUploadQueue(const QStringList &hostFiles, const QString &devDir)
{
    m_dirsEnsured.clear();
    QString dir = devDir;
    while (dir.size() > 1 && dir.endsWith(QLatin1Char('/'))) { dir.chop(1); }
    appendOutput(QStringLiteral("[ copying %1 file(s) -> %2 ]\n").arg(hostFiles.size()).arg(dir));

    auto hold = holdBusy();
    auto queue = std::make_shared<QStringList>(hostFiles);
    auto ok = std::make_shared<int>(0);
    auto fail = std::make_shared<int>(0);
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, queue, ok, fail, dir, step, hold]() {
        if (queue->isEmpty()) {
            appendOutput(QStringLiteral("[ done -- %1 ok, %2 failed ]\n").arg(*ok).arg(*fail) + prompt());
            QTimer::singleShot(0, this, [step]() { *step = nullptr; });   // break the self-reference
            return;
        }
        const QString hostFile = queue->takeFirst();
        m_xferChain = [ok, fail, step](bool success) {
            if (success) { ++(*ok); } else { ++(*fail); }
            (*step)();
        };
        uploadToFlipper(hostFile, dir + QLatin1Char('/'));
    };
    (*step)();
}

// Copies a set of wildcard matches (device paths) to one destination -- either
// this computer (dstHost) or another spot on the Flipper.
void FlipperCli::runCopyQueue(const QStringList &devMatches, const QString &dst, bool dstHost)
{
    auto queue = std::make_shared<QStringList>(devMatches);
    auto ok = std::make_shared<int>(0);
    auto fail = std::make_shared<int>(0);
    auto hold = holdBusy();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, queue, ok, fail, dst, dstHost, step, hold]() {
        if (queue->isEmpty()) {
            appendOutput(QStringLiteral("[ done -- %1 ok, %2 failed ]\n").arg(*ok).arg(*fail) + prompt());
            // Break the self-reference. *step captures the shared_ptr that owns
            // it, so the chain keeps itself alive forever -- harmless when it
            // only leaked a few bytes, fatal now that the same lambda also
            // carries the busy token: the token was never destroyed, busy()
            // stayed true, and the command queue behind it stopped dead.
            // Deferred, because assigning to a std::function that is mid-call
            // would destroy the frame currently executing.
            QTimer::singleShot(0, this, [step]() { *step = nullptr; });
            return;
        }
        const QString devFile = queue->takeFirst();
        auto next = [ok, fail, step](bool success) {
            if (success) { ++(*ok); } else { ++(*fail); }
            (*step)();
        };
        if (dstHost) {
            m_xferChain = next;
            downloadFromFlipper(devFile, dst + QLatin1Char('/'));
        } else {
            const QString name = devFile.section(QLatin1Char('/'), -1);
            sendRaw(QStringLiteral("storage copy ") + devFile + QLatin1Char(' ') + dst + QLatin1Char('/') + name,
                    [next](const QString &raw) { next(!raw.contains(QLatin1String("error"), Qt::CaseInsensitive)); });
        }
    };
    (*step)();
}

// find: "storage tree" the root, filter by wildcard against basename (falling
// back to a match against the full path) and print every hit.
void FlipperCli::startFind(const QString &root, const QString &pattern)
{
    const QRegularExpression rx(QRegularExpression::wildcardToRegularExpression(pattern),
                                QRegularExpression::CaseInsensitiveOption);
    // Second matcher for the whole-path fallback. In the default conversion "*"
    // stops at a path separator, so "*clitest*" never matched
    // /ext/clitest/a.txt and the fallback was dead code -- locate found the
    // folder and nothing inside it.
    const QRegularExpression rxPath(
        QRegularExpression::wildcardToRegularExpression(
            pattern, QRegularExpression::NonPathWildcardConversion),
        QRegularExpression::CaseInsensitiveOption);
    auto hold = holdBusy();
    sendRaw(QStringLiteral("storage tree ") + root, [this, rx, rxPath, hold](const QString &raw) {
        if (raw.contains(QLatin1String("Storage error"))) {
            appendOutput(QStringLiteral("[ can't read that path ]\n") + prompt());
            return;
        }
        QStringList hits;
        for (const auto &e : cliParseTree(raw)) {
            const QString name = e.path.section(QLatin1Char('/'), -1);
            if (rx.match(name).hasMatch() || rxPath.match(e.path).hasMatch()) { hits += e.path; }
        }
        appendOutput((hits.isEmpty() ? QStringLiteral("[ no matches ]\n") : hits.join(QLatin1Char('\n')) + QLatin1Char('\n'))
                     + prompt());
    });
}

// edit: read the file's text back like "cat" does, but hand it to
// editRequested() instead of just printing it, so a host-side editor panel can
// pop it open. Also echoes the content in the terminal so the command is still
// useful with nothing connected to that signal.
void FlipperCli::startEdit(const QString &path)
{
    // An editor panel is modal and takes the keyboard. Popping one open halfway
    // through a pasted block hijacks focus while the rest of the block is still
    // running, and whatever is left in that buffer then belongs to a file the
    // later commands may well have deleted -- which is exactly how a save landed
    // on a path that no longer existed.
    if (!m_pending.isEmpty()) {
        appendOutput(QStringLiteral("[ editor not opened: %1 command%2 still queued. "
                                    "The panel takes the keyboard, and the rest of the block would run behind it. "
                                    "Run 'edit %3' on its own. ]\n")
                         .arg(m_pending.size())
                         .arg(m_pending.size() == 1 ? QString() : QStringLiteral("s"), path)
                     + prompt());
        return;
    }
    sendRaw(QStringLiteral("storage read ") + path, [this, path](const QString &raw) {
        if (raw.contains(QLatin1String("Storage error"))) {
            appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(path) + prompt());
            return;
        }
        QString body = raw;
        const int c = raw.indexOf(QLatin1String("Size:"));
        if (c >= 0) {
            const int nl = raw.indexOf(QLatin1Char('\n'), c);
            body = (nl >= 0) ? raw.mid(nl + 1) : QString();
        }
        const int p = body.lastIndexOf(QLatin1String(">:"));
        if (p >= 0) { body = body.left(p); }
        body = body.trimmed();
        // Hand the file to the editor panel in the UI. We deliberately DON'T
        // also dump the body into the terminal any more -- that made edit look
        // like a glorified cat and buried the point. A short line confirms what
        // opened; the panel shows and edits the actual contents.
        emit editRequested(path, body);
        appendOutput(QStringLiteral("[ editing %1 -- opening editor ]\n").arg(path) + prompt());
    });
}

// ---- host-side text utilities over "storage read" --------------------------
//
// The Flipper cannot run grep, head, tail or wc itself, and never will: the
// firmware lives on an STM32WB55 with 256 KB of RAM and a fixed command table.
// So the file comes over the wire once and this computer does the work -- which
// is the same trade the rest of this panel already makes for find and cp -r.
void FlipperCli::readDeviceText(const QString &path, std::function<void(bool, const QString &)> done)
{
    sendRaw(QStringLiteral("storage read ") + path, [path, done](const QString &raw) {
        if (raw.contains(QLatin1String("Storage error"))) { done(false, QString()); return; }
        // "storage read" prints a "Size: N" header before the body and the
        // prompt after it; neither belongs to the file.
        QString body = raw;
        const int c = raw.indexOf(QLatin1String("Size:"));
        if (c >= 0) {
            const int nl = raw.indexOf(QLatin1Char('\n'), c);
            body = (nl >= 0) ? raw.mid(nl + 1) : QString();
        }
        const int p = body.lastIndexOf(QLatin1String(">:"));
        if (p >= 0) { body = body.left(p); }
        // The serial console ends every line with CR+LF and may leave a trailing
        // CR before the prompt. Left in, each split('\n') line keeps a stray \r,
        // which is why wc counted 7 "lines" for a 2-line file and head printed
        // phantom blank lines. Normalise to \n and trim the trailing whitespace.
        body.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        body.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        while (body.endsWith(QLatin1Char('\n')) || body.endsWith(QLatin1Char(' '))) { body.chop(1); }
        done(true, body);
    });
}

void FlipperCli::startGrep(const QString &pattern, const QString &path)
{
    readDeviceText(path, [this, pattern, path](bool ok, const QString &body) {
        if (!ok) { appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(path) + prompt()); return; }
        // Plain substring, case-insensitive -- the same thing `grep -i` does for
        // the overwhelming majority of what gets typed at a Flipper. A regex
        // pattern would need escaping rules the rest of this shell doesn't have.
        QStringList hits;
        const QStringList lines = body.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).contains(pattern, Qt::CaseInsensitive)) {
                hits += QStringLiteral("%1: %2").arg(i + 1, 4).arg(lines.at(i).trimmed());
            }
        }
        appendOutput((hits.isEmpty() ? QStringLiteral("[ no matches ]\n")
                                     : hits.join(QLatin1Char('\n')) + QLatin1Char('\n'))
                     + prompt());
    });
}

void FlipperCli::startHeadTail(const QString &path, int n, bool head)
{
    readDeviceText(path, [this, path, n, head](bool ok, const QString &body) {
        if (!ok) { appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(path) + prompt()); return; }
        QStringList lines = body.split(QLatin1Char('\n'));
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) { lines.removeLast(); }
        const int take = qBound(1, n, lines.size());
        const QStringList slice = head ? lines.mid(0, take) : lines.mid(lines.size() - take);
        appendOutput(slice.join(QLatin1Char('\n')) + QLatin1Char('\n') + prompt());
    });
}

void FlipperCli::startWc(const QString &path)
{
    // Straight to storage read rather than through readDeviceText, because the
    // byte count has to come from the firmware's own "Size:" header. Counting
    // the parsed buffer was one byte short on every file ending in a newline --
    // readDeviceText trims it, deliberately -- so wc said 23 for a file that
    // ls and stat both called 24.
    auto hold = holdBusy();
    sendRaw(QStringLiteral("storage read ") + path, [this, path, hold](const QString &raw) {
        if (raw.contains(QLatin1String("Storage error"))) {
            appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(path) + prompt());
            return;
        }
        qint64 bytes = -1;
        QString body = raw;
        const int c = raw.indexOf(QLatin1String("Size:"));
        if (c >= 0) {
            const int nl = raw.indexOf(QLatin1Char('\n'), c);
            bytes = raw.mid(c + 5, (nl >= 0 ? nl : raw.size()) - c - 5).trimmed().toLongLong();
            body = (nl >= 0) ? raw.mid(nl + 1) : QString();
        }
        const int p = body.lastIndexOf(QLatin1String(">:"));
        if (p >= 0) { body = body.left(p); }
        body.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        body.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        while (body.endsWith(QLatin1Char('\n')) || body.endsWith(QLatin1Char(' '))) { body.chop(1); }

        const QStringList lines = body.isEmpty() ? QStringList() : body.split(QLatin1Char('\n'));
        int words = 0;
        for (const QString &l : lines) {
            words += l.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
        }
        if (bytes < 0) { bytes = body.toUtf8().size(); }
        appendOutput(QStringLiteral("%1 lines  %2 words  %3 bytes  %4\n")
                         .arg(lines.size()).arg(words).arg(bytes).arg(path)
                     + prompt());
    });
}

// du: one "storage tree" walk, summed here. Prints the total plus a line per
// immediate child, which is the question actually being asked when an SD card
// is filling up.
void FlipperCli::startDu(const QString &path)
{
    sendRaw(QStringLiteral("storage tree ") + path, [this, path](const QString &raw) {
        if (raw.contains(QLatin1String("Storage error"))) {
            appendOutput(QStringLiteral("[ no such folder: %1 ]\n").arg(path) + prompt());
            return;
        }
        QString root = path;
        while (root.size() > 1 && root.endsWith(QLatin1Char('/'))) { root.chop(1); }

        qint64 total = 0;
        int files = 0;
        QMap<QString, qint64> perChild;
        for (const auto &e : cliParseTree(raw)) {
            if (e.isDir || e.size < 0) { continue; }
            total += e.size;
            ++files;
            QString rel = e.path;
            if (rel.startsWith(root)) { rel = rel.mid(root.size()); }
            while (rel.startsWith(QLatin1Char('/'))) { rel.remove(0, 1); }
            const int slash = rel.indexOf(QLatin1Char('/'));
            perChild[slash >= 0 ? rel.left(slash) : QStringLiteral(".")] += e.size;
        }

        auto human = [](qint64 b) {
            if (b >= 1024 * 1024) { return QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 1); }
            if (b >= 1024)        { return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1); }
            return QStringLiteral("%1 B").arg(b);
        };

        QStringList out;
        for (auto it = perChild.constBegin(); it != perChild.constEnd(); ++it) {
            out += QStringLiteral("%1\t%2").arg(human(it.value()), it.key());
        }
        out += QStringLiteral("%1\t%2  (%3 files)").arg(human(total), root).arg(files);
        appendOutput(out.join(QLatin1Char('\n')) + QLatin1Char('\n') + prompt());
    });
}

// echo > / >> and touch. Both stage the bytes in a temp file and hand them to
// the normal upload path, so they get its remove-then-write_chunk sequence and
// its MD5 verification for free rather than reimplementing either.
// Save from the editor panel: write the edited text straight back to the same
// path, verified through the normal upload path, and signal the panel so it can
// show "saved" and close. Kept separate from writeTextToDevice so the editor
// isn't coupled to the terminal's prompt printing.
void FlipperCli::saveEditedFile(const QString &path, const QString &content)
{
    if (path.isEmpty()) { emit editSaveError(path, QStringLiteral("No path.")); return; }

    // The editor panel opens files from both machines, and Save has to land on
    // the one the file came from. Anything outside /ext, /int and /any is on
    // this computer and is written directly -- pushing it at the Flipper would
    // create a nonsense path there and leave the real file untouched.
    if (cliIsHostPath(path)) { saveHostFile(path, content); return; }

    if (!m_port || !m_active) {
        emit editSaveError(path, QStringLiteral("Not connected -- can't save to the Flipper."));
        appendOutput(QStringLiteral("[ not connected -- %1 was not saved ]\n").arg(path) + prompt());
        return;
    }

    QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/nikita-edit-XXXXXX"));
    if (!tmp.open()) {
        emit editSaveError(path, QStringLiteral("Couldn't create a temp file on this computer."));
        return;
    }
    tmp.write(content.toUtf8());
    tmp.flush();

    // uploadToFlipper reads the temp file synchronously and reports into the
    // terminal; we additionally emit editSaved so the panel can react. A tiny
    // deferred emit keeps ordering predictable relative to the upload's own
    // terminal output.
    uploadToFlipper(tmp.fileName(), path, true);
    emit editSaved(path);
}

void FlipperCli::writeTextToDevice(const QString &text, const QString &devPath, bool append)
{
    if (append) {
        // ">>" has to read first: storage write_chunk appends, but the upload
        // deliberately removes the file beforehand so a repeated cp can't
        // concatenate. Merging here keeps that rule in one place.
        auto hold = holdBusy();   // spans the read and the write that follows it
        readDeviceText(devPath, [this, text, devPath, hold](bool ok, const QString &body) {
            QString merged = ok ? body : QString();
            if (!merged.isEmpty() && !merged.endsWith(QLatin1Char('\n'))) { merged += QLatin1Char('\n'); }
            merged += text;
            writeTextToDevice(merged, devPath, false);
        });
        return;
    }

    QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/nikita-write-XXXXXX"));
    if (!tmp.open()) {
        appendOutput(QStringLiteral("[ can't create a temp file on this computer ]\n") + prompt());
        return;
    }
    tmp.write(text.toUtf8());
    tmp.flush();
    const QString hostPath = tmp.fileName();
    // uploadToFlipper reads the file synchronously, so the temp file going out
    // of scope here is safe.
    uploadToFlipper(hostPath, devPath, true);
}

// wget: this computer downloads (the Flipper has no network stack), result
// lands on the SD card.
// sed s/PATTERN/REPLACEMENT/[g]: reads over the wire, edits here, writes
// back. No trailing target prints instead of saving; with one it overwrites,
// since round-tripping a device file just to not save it rarely makes sense.
void FlipperCli::startSed(const QString &expr, const QString &path)
{
    // Parse s/a/b/ or s/a/b/g. Any delimiter after the "s".
    if (expr.size() < 4 || !expr.startsWith(QLatin1Char('s'))) {
        appendOutput(QStringLiteral("[ only s/pattern/replacement/[g] is supported ]\n") + prompt());
        return;
    }
    const QChar delim = expr.at(1);
    const QStringList parts = expr.mid(2).split(delim);
    if (parts.size() < 2) {
        appendOutput(QStringLiteral("[ malformed sed expression ]\n") + prompt());
        return;
    }
    const QString pat = parts.value(0);
    const QString rep = parts.value(1);
    const bool global = parts.value(2).contains(QLatin1Char('g'));
    if (pat.isEmpty()) { appendOutput(QStringLiteral("[ empty pattern ]\n") + prompt()); return; }

    auto hold = holdBusy();   // spans the read and the write-back
    readDeviceText(path, [this, path, pat, rep, global, hold](bool ok, const QString &body) {
        if (!ok) { appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(path) + prompt()); return; }
        QString out = body;
        const QRegularExpression re(pat);
        if (!re.isValid()) { appendOutput(QStringLiteral("[ bad pattern: %1 ]\n").arg(re.errorString()) + prompt()); return; }
        int count = 0;
        if (global) {
            const QString before = out;
            out.replace(re, rep);
            count = before.count(re);
        } else {
            const QRegularExpressionMatch m = re.match(out);
            if (m.hasMatch()) { out.replace(m.capturedStart(), m.capturedLength(), rep); count = 1; }
        }
        // Nothing matched: say so and stop. Rewriting the file anyway meant a
        // no-op sed still did a remove + write_chunk + md5 round trip, putting
        // the file briefly at risk for no reason at all.
        if (count == 0) {
            appendOutput(QStringLiteral("[ sed: no matches in %1 -- file unchanged ]\n").arg(path) + prompt());
            return;
        }
        // Overwrite the file with the edited text; report how many it changed.
        appendOutput(QStringLiteral("[ sed: %1 replacement%2 in %3 ]\n")
                         .arg(count).arg(count == 1 ? QString() : QStringLiteral("s"), path));
        writeTextToDevice(out, path, false);
        // writeTextToDevice prints its own prompt on completion.
    });
}

// diff A B -- both files fetched, compared here. A plain line-level diff with
// +/- markers; enough to see what changed between two configs or scripts.
void FlipperCli::startDiff(const QString &pathA, const QString &pathB)
{
    auto hold = holdBusy();   // spans both reads
    readDeviceText(pathA, [this, pathA, pathB, hold](bool okA, const QString &bodyA) {
        if (!okA) { appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(pathA) + prompt()); return; }
        readDeviceText(pathB, [this, pathA, pathB, bodyA, hold](bool okB, const QString &bodyB) {
            if (!okB) { appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(pathB) + prompt()); return; }
            const QStringList la = bodyA.split(QLatin1Char('\n'));
            const QStringList lb = bodyB.split(QLatin1Char('\n'));
            // Simplest useful diff: walk both, mark lines that differ. Not an
            // LCS, but on a Flipper's small config files that reads fine and
            // keeps the code tiny.
            QStringList out;
            const int n = qMax(la.size(), lb.size());
            int diffs = 0;
            for (int i = 0; i < n; ++i) {
                const QString a = i < la.size() ? la.at(i) : QString();
                const QString b = i < lb.size() ? lb.at(i) : QString();
                if (a == b) { continue; }
                ++diffs;
                if (i < la.size()) { out += QStringLiteral("- %1").arg(a); }
                if (i < lb.size()) { out += QStringLiteral("+ %1").arg(b); }
            }
            if (diffs == 0) { appendOutput(QStringLiteral("[ files are identical ]\n") + prompt()); return; }
            appendOutput(out.join(QLatin1Char('\n')) + QLatin1Char('\n') + prompt());
        });
    });
}

// file -- identify a file by its leading bytes (magic numbers), the way Unix's
// file(1) does. The Flipper's own formats are text with a "Filetype:" header,
// so check that too -- it is more useful here than libmagic would be.
void FlipperCli::startFileType(const QString &path)
{
    readDeviceText(path, [this, path](bool ok, const QString &body) {
        if (!ok) { appendOutput(QStringLiteral("[ no such file: %1 ]\n").arg(path) + prompt()); return; }
        QString verdict;
        const QByteArray head = body.left(16).toUtf8();
        auto starts = [&](const char *sig) { return head.startsWith(sig); };
        if (body.startsWith(QLatin1String("Filetype:"))) {
            // Flipper's own asset format -- the second token names it.
            const QString first = body.section(QLatin1Char('\n'), 0, 0);
            verdict = QStringLiteral("Flipper asset (%1)").arg(first.section(QLatin1Char(':'), 1).trimmed());
        } else if (starts("\x89PNG"))            { verdict = QStringLiteral("PNG image"); }
        else if (starts("\xFF\xD8\xFF"))          { verdict = QStringLiteral("JPEG image"); }
        else if (starts("PK\x03\x04"))            { verdict = QStringLiteral("ZIP archive"); }
        else if (starts("\x1F\x8B"))              { verdict = QStringLiteral("gzip archive"); }
        else if (starts("%PDF"))                  { verdict = QStringLiteral("PDF document"); }
        else if (body.startsWith(QLatin1String("#!")))       { verdict = QStringLiteral("script (shebang)"); }
        else {
            bool binary = false;
            for (const QChar c : body.left(512)) {
                if (c.unicode() == 0 || (c.unicode() < 9)) { binary = true; break; }
            }
            verdict = binary ? QStringLiteral("binary data") : QStringLiteral("ASCII text");
        }
        appendOutput(QStringLiteral("%1: %2\n").arg(path, verdict) + prompt());
    });
}

// locate -- Linux locate reads a prebuilt index (updatedb); there is none here,
// so this is find rooted at /ext, i.e. "search the whole card". Same result,
// honest mechanism.
void FlipperCli::startLocate(const QString &pattern)
{
    QString pat = pattern;
    if (!pat.contains(QLatin1Char('*')) && !pat.contains(QLatin1Char('?'))) {
        pat = QLatin1Char('*') + pat + QLatin1Char('*');   // substring match, like real locate
    }
    startFind(QStringLiteral("/ext"), pat);
}

void FlipperCli::startWget(const QString &url, const QString &devPath, bool exactDest)
{
    QUrl u(url);
    if (!u.isValid() || u.scheme().isEmpty()) { u = QUrl(QStringLiteral("https://") + url); }
    if (!u.isValid() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https"))) {
        appendOutput(QStringLiteral("[ not a http(s) URL: %1 ]\n").arg(url) + prompt());
        return;
    }

    appendOutput(QStringLiteral("[ downloading %1 ]\n").arg(u.toString()));
    // Held across the whole fetch. The Flipper is idle while this runs, but the
    // CLI is not free: the bytes are on their way to an upload that owns the
    // serial line the moment they land.
    auto hold = holdBusy();
    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("nikita-cli"));
    // Without this a stalled connection never emits finished(), the busy token
    // it holds is never released, and the CLI refuses every later command with
    // no visible reason why.
    req.setTransferTimeout(30000);

    QNetworkReply *reply = m_dl.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, devPath, exactDest, hold]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            appendOutput(QStringLiteral("[ download failed: %1 ]\n").arg(reply->errorString()) + prompt());
            return;
        }
        const QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            appendOutput(QStringLiteral("[ the server sent an empty body ]\n") + prompt());
            return;
        }
        // Same 1 MiB ceiling the CLI upload path enforces -- say so here rather
        // than downloading first and refusing afterwards.
        if (data.size() > 1024 * 1024) {
            appendOutput(QStringLiteral("[ %1 bytes -- over the 1 MB CLI upload limit. Save it on this computer and use the file tools. ]\n")
                             .arg(data.size())
                         + prompt());
            return;
        }
        QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/nikita-wget-XXXXXX"));
        if (!tmp.open()) {
            appendOutput(QStringLiteral("[ can't create a temp file on this computer ]\n") + prompt());
            return;
        }
        tmp.write(data);
        tmp.flush();
        uploadToFlipper(tmp.fileName(), devPath, exactDest);
    });
}

void FlipperCli::clearOutput()
{
    if (m_output.isEmpty()) { return; }
    m_output.clear();
    emit outputChanged();
}

void FlipperCli::appendOutput(const QString &text)
{
    m_output += text;

    // No blank rows, ever. The firmware pads its replies with a spare newline
    // before the prompt and the reformatters add their own, which is what left
    // an empty line hanging above every prompt after a command like "ls".
    // Only the newly appended tail is scanned -- everything before it was
    // normalised on the way in, and re-running this over the whole buffer on
    // every serial chunk is real work during a streaming command like "top".
    static const QRegularExpression blankRows(QStringLiteral("\\n[ \\t]*(?:\\n[ \\t]*)+"));
    const int scanFrom = qMax(0, m_output.size() - text.size() - 64);
    QString tail = m_output.mid(scanFrom);
    tail.replace(blankRows, QStringLiteral("\n"));
    m_output.truncate(scanFrom);
    m_output += tail;
    while (m_output.startsWith(QLatin1Char('\n'))) { m_output.remove(0, 1); }

    // Collapse a prompt that lands straight after another one. The old version
    // matched three exact spellings of the gap between them (p+p, p+\n+p,
    // rtrim+\n+p), so a single stray space from the firmware was enough for a
    // duplicate to slip through. Two prompts with nothing but whitespace
    // between them means nothing was typed or printed in between, so the
    // earlier one is always the redundant one.
    const QString p = prompt();
    const QString rtrim = p.trimmed();
    if (!rtrim.isEmpty()) {
        for (;;) {
            int e = m_output.size();
            while (e > 0 && m_output.at(e - 1).isSpace()) { --e; }
            if (e < rtrim.size() || QStringView{m_output}.mid(e - rtrim.size(), rtrim.size()) != QStringView{rtrim}) { break; }
            const int lastStart = e - rtrim.size();

            int w = lastStart;
            while (w > 0 && m_output.at(w - 1).isSpace()) { --w; }
            if (w < rtrim.size() || QStringView{m_output}.mid(w - rtrim.size(), rtrim.size()) != QStringView{rtrim}) { break; }

            m_output.remove(w - rtrim.size(), lastStart - (w - rtrim.size()));
        }
    }
    // Whatever the user types next belongs on the prompt's own line, so the
    // prompt is never the second-to-last thing in the buffer.
    if (m_output.endsWith(p + QLatin1Char('\n')))      { m_output.chop(1); }
    else if (m_output.endsWith(rtrim + QLatin1Char('\n'))) { m_output.chop(1); }
    if (m_output.size() > kCliScrollbackMax) {
        m_output = m_output.right(kCliScrollbackKeep);
        // Drop the partial first line. A buffer starting mid-line can hand the
        // colouriser a fragment that matches a rule the whole line would not.
        const int nl = m_output.indexOf(QLatin1Char('\n'));
        if (nl >= 0) { m_output.remove(0, nl + 1); }
        // Say that it happened. Silently dropping the top of a "tree /ext" made
        // a truncated listing look like a listing that had simply ended, which
        // is the difference between "there are no more files" and "you cannot
        // see them from here".
        m_output.prepend(QStringLiteral("[ ...earlier output trimmed -- redirect to a file "
                                        "for the whole thing: cp <path> ~/out.txt ]\n"));
    }

    // Rate-limit the repaint. Streaming commands (top, log) push chunks far
    // faster than the view can lay out 16k characters, which is what makes the
    // panel feel locked up.
    if (!m_outputTick) {
        m_outputTick = new QTimer(this);
        m_outputTick->setSingleShot(true);
        m_outputTick->setInterval(60);
        connect(m_outputTick, &QTimer::timeout, this, &FlipperCli::outputChanged);
    }
    if (!m_outputTick->isActive()) { m_outputTick->start(); }
}

void FlipperCli::setActive(bool v)
{
    if (m_active != v) { m_active = v; emit activeChanged(); }
}

void FlipperCli::setStatus(const QString &s)
{
    if (m_status != s) { m_status = s; emit statusChanged(); }
}