#pragma once

#include <functional>
#include <memory>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QVariant>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QSerialPort>
#include <QSerialPortInfo>

class ApplicationBackend;
class QNetworkReply;
class QProcess;

// Nikita - an AI chat assistant inside qFlipper, with tool access to the
// to live-query the connected Flipper Zero over qFlipper's RPC link.
class FlipperCli;   // defined below; NikitaBackend holds a pointer for run_cli

class NikitaBackend : public QObject
{
    Q_OBJECT
    // ---- Master switch ------------------------------------------------------
    // Off hides the whole assistant behind a single ENABLE NIKITA control. It
    // is a UI state, not a kill switch: nothing is unloaded and no setting is
    // lost, so turning it back on restores the conversation as it was.
    // Persisted, because a master switch that forgets itself on restart is not
    // one the user can rely on.
    Q_PROPERTY(bool assistantEnabled READ assistantEnabled WRITE setAssistantEnabled
               NOTIFY assistantEnabledChanged)
    bool assistantEnabled() const;
    void setAssistantEnabled(bool on);

    // ---- The API ----------------------------------------------------------
    // Moonshot's kimi-k3, over the OpenAI wire format, and the only brain there
    // is. The local Ollama path was removed once this one worked -- with it went
    // the model catalog, the pull/remove machinery, the runtime installer and
    // the context-window arithmetic that came with running a 4B on an 8 GB Mac.
    // Whether a key is available at all, WITHOUT exposing it. QML needs to know
    // if it can send a turn; it never needs the characters, and a key readable
    // from QML is a key that ends up in a screenshot.
    Q_PROPERTY(bool apiKeyPresent READ apiKeyPresent NOTIFY apiKeyChanged)
    bool apiKeyPresent() const;
    // Where the key came from, for the settings panel to explain itself:
    // "environment" (MOONSHOT_API_KEY), "settings", or "" when there is none.
    Q_PROPERTY(QString apiKeySource READ apiKeySource NOTIFY apiKeyChanged)
    QString apiKeySource() const;
    // Which Kimi model answers. kimi-k3 is the newest flagship but is rate-limited
    // in preview; kimi-k2.6 / k2.7 are GA and run at the account's full tier. A
    // property so the setup panel can offer the choice, persisted in settings.
    Q_PROPERTY(QString apiModel READ apiModel WRITE setApiModel NOTIFY modelChanged)
    void setApiModel(const QString &id);
    // The selectable Kimi models, each as {id, label, note} for the picker.
    Q_INVOKABLE QVariantList apiModelChoices() const;

    // Set from the password field in setup. Write-only on purpose: there is no
    // getter, so nothing in QML can read it back out.
    //
    // The explicit public: is load-bearing and not style. Everything from the
    // top of this class down to the first public: below is implicitly PRIVATE,
    // which is harmless for the Q_PROPERTY accessors around here -- moc's
    // generated code is a class member and reaches them regardless -- but a
    // private Q_INVOKABLE is a different story: it compiles, it appears in the
    // meta-object, and then QML refuses to call it at runtime with
    // "Property 'setApiKey' ... is not a function". Every other Q_INVOKABLE in
    // this file happens to sit after that public:, so nothing had ever caught
    // it. Restored to private straight after, so the declarations below keep
    // the access they were written with.
public:
    Q_INVOKABLE void setApiKey(const QString &key);
    Q_INVOKABLE void clearApiKey();
    // Hands the key back in the clear, for the eye control in setup.
    //
    // This is a deliberate reversal: the field was built write-only, with no
    // getter at all, so that nothing in QML could read the characters even by
    // accident. Revealing your own key on your own machine is what every
    // password manager does and is a reasonable thing to want -- but it does
    // mean the key can now reach a screen, and a key on a screen is a key in
    // whatever is recording the screen. Named revealApiKey() rather than
    // apiKey() so a call site can never look like an innocent read, and NOT a
    // Q_PROPERTY, so no binding can pull it in without asking.
    Q_INVOKABLE QString revealApiKey() const;
    // Copy without exposing. The key goes from settings straight onto the
    // clipboard inside C++ and is never handed to QML, so the copy button --
    // unlike the eye -- costs nothing in reach. Answers false when there is
    // nothing to copy, so the UI can stay quiet instead of flashing "copied".
    Q_INVOKABLE bool copyApiKeyToClipboard() const;
private:

    Q_PROPERTY(bool thinking READ thinking NOTIFY thinkingChanged)
    // ---- Live turn status --------------------------------------------------
    // What is happening RIGHT NOW, in the user's words: "thinking", "writing
    // the file", "running the command", "wrapping up". A turn on a local 4B
    // takes a minute or more and the reply cannot stream while tools are
    // attached (see dispatchTurn), so without this the window sits dead
    // and a working assistant is indistinguishable from a hung one.
    // Every value is set at a real transition -- never a decorative cycle.
    Q_PROPERTY(QString turnStatus READ turnStatus NOTIFY turnStatusChanged)
    QString turnStatus() const;
    // Seconds since the turn started. Ticks once a second while thinking.
    Q_PROPERTY(int turnElapsed READ turnElapsed NOTIFY turnStatusChanged)
    int turnElapsed() const;
    // The same thing as "4m 21s" -- a bare seconds count stops being readable
    // about a minute in, which on a local 4B is most turns.
    Q_PROPERTY(QString turnElapsedText READ turnElapsedText NOTIFY turnStatusChanged)
    QString turnElapsedText() const;
    // Tokens this turn has cost so far, prompt + generated, summed across every
    // round trip it takes. "2.1k tokens" past a thousand.
    Q_PROPERTY(QString turnTokensText READ turnTokensText NOTIFY turnStatusChanged)
    QString turnTokensText() const;
    // What the turn has cost so far, as "$0.0123". Empty until the first round
    // reports usage. Tokens alone stopped being the interesting number the
    // moment the brain moved to an API: the same 8k tokens is a rounding error
    // as input and real money as output, and only the dollars say which.
    Q_PROPERTY(QString turnCostText READ turnCostText NOTIFY turnStatusChanged)
    QString turnCostText() const;
    Q_PROPERTY(bool configured READ configured CONSTANT)
    Q_PROPERTY(bool hasBle READ hasBle CONSTANT)   // true wherever HZUI_BLE is compiled in
    Q_PROPERTY(QString modelName READ modelName NOTIFY modelChanged)
    Q_PROPERTY(bool agentEnabled READ agentEnabled WRITE setAgentEnabled NOTIFY agentChanged)
    Q_PROPERTY(QString agentDir READ agentDir WRITE setAgentDir NOTIFY agentChanged)

public:
    // Erases everything NIKITA has kept about this user: the conversation, the
    // durable facts, the learned moves, and the permissions granted to it --
    // on this computer AND on the Flipper's SD card. Called when the master
    // switch is turned off, after the person confirms.
    //
    // MUST stay in a public section: moc generates the entry either way, but
    // QML refuses to call a non-public invokable, and the failure is a runtime
    // TypeError inside the click handler -- so the button silently does
    // nothing instead of erroring anywhere visible.
    Q_INVOKABLE void wipeAssistantData();
    explicit NikitaBackend(QObject *parent = nullptr);

    // Gives Nikita access to the connected device (for the inspection tools).
    void setAppBackend(ApplicationBackend *backend);
    void setCli(FlipperCli *cli);   // link the CLI for run_cli

    bool thinking() const;
    bool configured() const;
    bool hasBle() const;   // true only where Qt Bluetooth is compiled in
    QString modelName() const;
    bool agentEnabled() const;                       // host-workspace (self-edit) tools on?
    QString agentDir() const;                        // workspace root Nikita may touch
    void setAgentEnabled(bool on);
    void setAgentDir(const QString &dir);
    Q_INVOKABLE QString extractScript(const QString &text) const;   // pull code from a message
    Q_INVOKABLE void saveScriptToFlipper(const QString &folder,     // manual, model-free save
                                         const QString &filename,
                                         const QString &content);
    Q_INVOKABLE void openFileForEdit(const QString &path);          // read a file into the editor
    Q_INVOKABLE void clearHistory();                                // wipe the chat conversation
    Q_INVOKABLE void writeFile(const QString &path, const QString &content); // save edits back
    // A host_run call is waiting on screen for a person to approve or refuse
    // it. Called from the confirmation dialog's Run/Cancel buttons.
    Q_INVOKABLE void answerHostRunConfirm(bool allow, bool alwaysAllow);
    // Same idea for the other mutating host_* tools (write/mkdir/move/copy/
    // delete); one shared dialog gates all of them. See requestHostActionConfirm().
    Q_INVOKABLE void answerHostActionConfirm(bool allow, bool alwaysAllow);
    // The user's choice when a Flipper save would overwrite an existing file:
    // action is "replace", "rename" (with newName), or "cancel".
    Q_INVOKABLE void answerSaveConflict(const QString &action, const QString &newName);

    Q_INVOKABLE void send(const QString &userText, const QString &deviceContext);
    // Typed while a turn is still running. Held, not dropped, and delivered as
    // one message the moment the turn ends -- so a thought does not have to
    // wait on a 4B model finishing its sentence.
    Q_INVOKABLE void queueMessage(const QString &text);
    Q_INVOKABLE void clearQueue();
    Q_PROPERTY(int queuedCount READ queuedCount NOTIFY queuedChanged)
    int queuedCount() const;
    Q_PROPERTY(QString queuedPreview READ queuedPreview NOTIFY queuedChanged)
    QString queuedPreview() const;
    // Interrupts the reply being waited on: aborts the HTTP request.
    // Does not reach into an in-flight tool call -- only the dispatch path.
    Q_INVOKABLE void stopThinking();
    // Called from the chat when the user says whether the last action actually
    // worked. Their eyes beat any check this code can run: a file can be
    // written, verified, and still be the wrong file in the wrong place.
    // note is the user's own words about what went wrong -- optional, and the
    // most valuable half when it is there. "I asked for Chrome and it made a
    // txt file" is something no automatic check can produce: the tool reported
    // success, the file exists, and the request was still not met.
    Q_INVOKABLE void rateLastAction(bool worked, const QString &note = QString());

    // True while the last finished turn ran at least one tool and has not been
    // rated yet -- the window in which the feedback strip is shown.
    Q_PROPERTY(bool canRate READ canRate NOTIFY canRateChanged)
    bool canRate() const;
    Q_INVOKABLE void reset();

    // ---- Model manager (gear icon) ----------------------------------------
    // it's currently installed. QML renders this as the model-manager list.

    // of whether the server is currently answering /api/tags).

    // ---- Access filters ----------------------------------------------------
    // What the assistant is allowed to touch. Each list item is a QVariantMap
    // with id, label, blurb and enabled -- ready for a Repeater. The groups
    // themselves live in NIKITA_FILTERS in the .cpp.
    Q_PROPERTY(QVariantList filters READ filters NOTIFY filtersChanged)
    QVariantList filters() const;
    // 1 = all on (full access), 0 = all off (no access), -1 = a mix. Only so
    // the UI knows which preset to highlight.
    Q_PROPERTY(int filterPreset READ filterPreset NOTIFY filtersChanged)
    int filterPreset() const;
    Q_INVOKABLE void setFilter(const QString &id, bool on);
    Q_INVOKABLE void setAllFilters(bool on);   // the two presets: all / none

    // One op at a time; op == "" when idle.

    Q_INVOKABLE QStringList personalityPresets() const;  // preset persona names
    Q_INVOKABLE void applyPreset(const QString &name);   // set persona to a preset
    Q_INVOKABLE void applyNamePersonality();             // persona built from the Flipper's name

    // These three are called from QML and were sitting under private:. Qt's
    // QML engine refuses to invoke them there, so every call threw and took
    // the rest of its handler down with it, which is what stopped
    // Backend.installFirmware() from ever running.
    Q_INVOKABLE void logAction(const QString &what) const;
    Q_INVOKABLE void reloadMemory();           // re-read memory.txt from the card
    Q_INVOKABLE void syncMemoryToFlipper();    // back up memory + self to the SD

    // Copy the user's own files off the SD card. The stock Backup button saves
    // /int (pairing keys, dolphin state, region), which is settings, not the
    // captures, scripts and dumps people actually mean by "backup".
    // Backup, restore and format are minutes of work with no visible sign that
    // anything is happening: the LOGS panel is not somewhere most people
    // think to look. These drive the same kind of progress screen the firmware
    // install uses.
    Q_PROPERTY(bool transferActive READ transferActive NOTIFY transferChanged)
    Q_PROPERTY(QString transferTitle READ transferTitle NOTIFY transferChanged)
    Q_PROPERTY(QString transferNote READ transferNote NOTIFY transferChanged)
    Q_PROPERTY(double transferProgress READ transferProgress NOTIFY transferChanged)
    bool transferActive() const { return m_transferActive; }
    QString transferTitle() const { return m_transferTitle; }
    QString transferNote() const { return m_transferNote; }
    double transferProgress() const { return m_transferProgress; }

    Q_INVOKABLE void backupSdCard(const QUrl &target);
    // Puts a backup folder back onto /ext. Takes the folder the picker returned.
    Q_INVOKABLE void restoreSdCard(const QString &folderUrl);
    // Empties the SD card. /int is reset separately, by the factory reset the
    // QML chains after this one finishes.
    Q_INVOKABLE void formatSdCard();
    // Straight reboot into the OS. The session already knows how; this just
    // gives the UI a way to ask for it.
    Q_INVOKABLE void rebootDevice();
    // True once the card has been emptied and before anything is installed onto
    // it again: a reinstall repairs an existing install, and after a format
    // there is nothing on the card left to repair.
    Q_PROPERTY(bool sdFormatted READ sdFormatted NOTIFY sdFormattedChanged)
    bool sdFormatted() const { return m_sdFormatted; }

signals:
    void backupProgress(const QString &note, double frac);
    void backupFinished(const QString &path, int items);
    void backupFailed(const QString &error);
    void restoreProgress(const QString &note, double frac);
    void restoreFinished(int items);
    void restoreFailed(const QString &error);
    void formatProgress(const QString &note, double frac);
    void formatFinished();
    void formatFailed(const QString &error);
    void sdFormattedChanged();
    void transferChanged();

    void replyReceived(const QString &text);
    void errorOccurred(const QString &text);
    void thinkingChanged();
    void assistantEnabledChanged();
    void apiKeyChanged();
    void canRateChanged();
    void queuedChanged();
    void turnStatusChanged();
    // One tool call, as its own line in the chat rather than as part of the
    // reply text. `seq` identifies the call for the whole of its life, so the
    // line the UI drew when it STARTED is the same line that gets rewritten
    // when it finishes -- a running step and its result are one event, not two.
    // `detail` is the exact call -- tool name and arguments, e.g.
    // "press_button(button=up, times=2)" -- so the row can be expanded in the
    // chat to show what was actually executed.
    void toolActivity(int seq, const QString &text, const QString &detail,
                      bool finished, bool failed);
    void modelChanged();
    void agentChanged();
    void filtersChanged();
    void modelInstallFinished(const QString &name, bool ok, const QString &message);
    void modelUninstallFinished(const QString &name, bool ok, const QString &message);
    void scriptSaved(const QString &path);        // manual save succeeded
    void scriptSaveError(const QString &message);  // manual save failed
    void fileOpened(const QString &path, const QString &content); // editor: file read
    void fileSaved(const QString &path);           // editor: file written
    void fileEditError(const QString &message);    // editor: read/write failed
    void partialReceived(const QString &text);   // live-typing: reply text so far
    // Every host_* tool call, as it happens, for the chat to show inline.
    void hostActionRan(const QString &summary);
    // host_run wants to execute `command` (in `cwd`) on this computer and is
    // waiting for a person to say yes. Nothing runs until answerHostRunConfirm
    // is called back.
    void hostRunConfirmRequested(const QString &command, const QString &cwd);
    // One of the other mutating host_* tools (write/mkdir/move/copy/delete)
    // wants to act and is waiting for a person to say yes. `kind` is the verb
    // ("write"/"mkdir"/"move"/"copy"/"delete"), `summary` is a one-line
    // description for the dialog title, `detail` is optional extra context
    // (a content preview for a write, empty for the rest).
    // A save_file to the Flipper found a file already at that path. QML shows
    // Replace / Rename / Cancel; the answer comes back via answerSaveConflict.
    void saveConflictRequested(const QString &path, const QString &preview);
    void hostActionConfirmRequested(const QString &kind, const QString &summary,
                                    const QString &detail);

private:
    void setThinking(bool value);
    // memory.txt can be edited by hand; these keep the in-memory copy honest.
    void applyMemoryText(const QString &text, const QString &source);
    void refreshMemoryFromDisk();
    void writeMemoryCache() const;
    bool adoptMemoryIfMemoryFile(const QString &path, const QString &content);
    // The twin for actions-memory.txt: a hand-edit of the proven-moves file is
    // authoritative, including deleting it down to nothing. Without this the
    // stale in-memory list was written straight back on the next turn, so an
    // erase on the card reverted itself.
    bool adoptSkillsIfSkillsFile(const QString &path, const QString &content);

    // Filters: stored as the OFF set (the .cpp explains why that way round).
    void loadFilters();
    void saveFilters();
    bool toolAllowed(const QString &tool) const;
    QSet<QString> allowedTools() const;
    QSet<QString> m_filtersOff;
    bool       m_assistantEnabled = true;

    QString systemPrompt() const;
    // Generated from the CLI command table so the assistant can never be taught
    // a command that no longer exists.
    static QString cliReferenceForPrompt();
    QString assistantName() const;   // device name -> who the log credits

    void loadHistory();   // restore past conversation from disk
    void saveHistory();   // persist conversation (user + final replies only)

    void dispatchTurn();                                   // POST history + tools
    void onStreamData(QNetworkReply *reply);      // parse streamed NDJSON chunks
    void onStreamFinished(QNetworkReply *reply);
    void finalizeStream();                        // a full response arrived
    // One decoded reply, normalised out of the OpenAI shape into the one the
    // rest of this file reads. Returns true when the turn is finished.
    bool consumeModelFrame(const QJsonObject &obj);
    void runToolCalls(const QJsonArray &toolCalls, int index); // execute tools sequentially
    // Every exit point that ends a turn with a user-visible reply calls this
    // instead of `emit replyReceived` directly, so the closing "done in" line
    // rides along with the text it produced rather than vanishing once the
    // streaming bubble in QML is overwritten by the model's own words.
    void emitReply(const QString &text);

    // There is one path only (dispatchTurn). Every continuation point in
    // finalizeStream()/runToolCalls() calls THIS, never the backend directly,
    // so a mid-turn retry and a tool-result round trip go the same way.
    void redispatch();
    void runOneTool(const QString &name, const QJsonObject &args,
                    std::function<void(const QString &)> done); // one async RPC tool
    void ensureFlipperDir(const QByteArray &dirPath,
                          std::function<void()> done);           // mkdir -p on the SD card

    // Host agent: full access to this computer, plus the Flipper.
    bool agentReady() const;                                     // agent mode is on
    QString resolveAgentPath(const QString &rel, bool mustExist) const;
    bool moveStillHolds(const QString &tool, const QJsonObject &args) const;
    // Where relative paths land when nothing else says otherwise. Never empty:
    // an unconfigured workspace means home.
    QString agentBaseDir() const;
    // The folder the agent is standing in. host_cd walks it, it survives the
    // whole conversation, and every turn's prompt states it, so the model
    // never has to spend a call asking where it is.
    QString agentCwd() const;
    QString m_agentCwd;
    void runHostTool(const QString &name, const QJsonObject &args,
                     std::function<void(const QString &)> done);
    // Actually spawns the command (async QProcess + watchdog, never blocks the
    // GUI thread). Called either straight away, when the exact command is on
    // the always-allow list, or from answerHostRunConfirm() once a person
    // approves it on screen.
    void executeHostRun(const QString &cmd, const QString &cwd,
                        std::function<void(const QString &)> done);
    bool hostRunAlwaysAllowed(const QString &cmd) const;
    void rememberHostRunAllowed(const QString &cmd);
    // A host_run call waiting on the on-screen confirmation dialog. One at a
    // time: runToolCalls() only starts the next tool after this one's done()
    // fires, so a second host_run can never arrive while this is still set.
    QString m_pendingHostRunCmd;
    QString m_pendingHostRunCwd;
    std::function<void(const QString &)> m_pendingHostRunDone;

    // Shared confirmation gate for host_write/host_mkdir/host_move/host_copy/
    // host_delete. Unlike host_run's always-allow list (one exact command
    // string), this remembers by KIND, since paths differ on every call.
    // `run` is the already-resolved mutation; `done` reports back on decline.
    // Flipper save with an overwrite guard: stat the path, and if a file is
    // already there hand the choice to the user before writing.
    void beginFlipperSave(const QByteArray &path, const QString &content,
                          std::function<void(const QString &)> done);
    void writeFlipperFile(const QByteArray &path, const QString &content,
                          std::function<void(const QString &)> done);
    QString    m_pendingSaveContent;
    QByteArray m_pendingSavePath;
    std::function<void(const QString &)> m_pendingSaveDone;

    void requestHostActionConfirm(const QString &kind, const QString &summary,
                                  const QString &detail,
                                  std::function<void()> run,
                                  std::function<void(const QString &)> done);
    bool hostActionAlwaysAllowed(const QString &kind) const;
    void rememberHostActionAllowed(const QString &kind);
    QString m_pendingHostActionKind;
    std::function<void()> m_pendingHostActionRun;
    std::function<void(const QString &)> m_pendingHostActionDone;
    void rememberFact(const QString &fact);   // append a durable fact to memory
    void loadPortableMemory();                 // pull memory/self from the Flipper SD
    void readPortableMemory();                 // the two chained SD reads

    int  forgetFacts(const QString &match);    // remove matching facts (or all); returns count

    QNetworkAccessManager m_net;
    ApplicationBackend *m_appBackend = nullptr;
    FlipperCli         *m_cli = nullptr;   // for the run_cli tool (set by Application)

    QJsonArray m_history;        // running messages (user / assistant / tool)
    QString    m_deviceContext;  // latest diagnostics snapshot from QML
    int        m_toolRounds = 0;
    bool       m_turnNeedsTools = false;   // set per turn by the intent router
    // Does this turn concern the Flipper at all? Decides whether the radio /
    // NFC / IR / BadUSB manual is worth its ~2000 tokens of window this turn.
    bool       m_turnIsDevice = false;
    // Which machine the turn is about (0 both / 1 Flipper / 2 this computer).
    // Decided when the message arrives, used later when the request is built --
    // dispatchTurn() no longer has the message in hand by then.
    int        m_turnFocus = 0;
    // A turn gets one forced retry: when the model answers a write request in
    // prose instead of calling anything, it is asked again with a single tool.
    int        m_forcedRetry = 0;            // corrections spent on this turn
    // Set when a turn was supposed to act and didn't. The next message is then
    // armed with tools no matter how it is phrased, because that next message
    // is almost always "no, you didn't actually do it".
    bool       m_lastTurnMissed = false;

    // Proven moves: one verified example per tool, shown to the model as its own
    // track record. Kept out of m_memory because that list is facts about the
    // USER; this is evidence about what this assistant has already pulled off.
    QStringList m_skills;
    QString     m_lastUserText;     // phrasing the current turn gets filed under
    QString     m_lastProvenTool;   // learned this turn, so it can be unlearned
    QStringList m_queued;           // typed while thinking, sent when the turn ends
    QString     m_lastDeviceContext;
    bool        m_canRate = false;  // a finished turn ran tools and awaits a verdict
    QStringList m_mistakes;         // "ask \t what it did \t the user's words"
    void flushQueue();
    void loadMistakes();
    void saveMistakes();
    void recordProvenMove(const QString &tool, const QJsonObject &args);
    void saveProvenMoves() const;
    void loadProvenMoves();
    QString    forcedToolName() const;
    // Does the reply assert an action whose tool never ran this turn? Compares
    // the sentence against m_turnToolsRan, because "some tool ran" is not the
    // same as "the tool for the thing you just claimed ran".
    bool       claimsUnrunAction(const QString &reply, QStringList *missing = nullptr) const;
    // Ledger appended after each tool round, so a small model does not lose the
    // second half of a two-part request the moment the first half succeeds.
    void       appendContinuationNudge();
    // The artifacts a request names, and the checklist built from them. Anchored
    // to what was ASKED rather than to what has been done, which is the only
    // reference that can tell you something is still missing.
    static QStringList requestedArtifacts(const QString &userText);
    QString    buildChecklist() const;
    // Lessons are queued during a turn and written once at the end, so a
    // multi-step request is never filed as though its first tool solved it.
    void       flushPendingMoves();
    // Anti-hallucination bookkeeping (see finalizeStream) + "edit the same file":
    bool       m_turnWasFileAction = false;  // this user turn asked to save/create/write a file
    bool       m_turnRanAnyTool    = false;  // at least one tool actually executed this turn
    bool       m_turnHadToolError  = false;  // a tool returned {"error":...} this turn
    QSet<QString> m_turnToolsRan;            // tools that ran WITHOUT error this turn
    // Basenames of every path a tool actually touched this turn. A claim is
    // checked against these, so "created A and B" cannot pass on having
    // written only A.
    QSet<QString> m_turnPathsTouched;
    // Confirms this turn's work on the filesystem, independently of anything
    // the model said. See the .cpp for the conditions it insists on.
    bool turnWorkVerified() const;
    QList<QPair<QString, QJsonObject>> m_pendingMoves;   // this turn's lessons, unwritten
    // Every distinct tool call this turn has already made. A round that asks
    // for nothing new is a loop, and looping -- not a step count -- is what
    // ends a turn.
    QSet<QString> m_turnCallSigs;
    int        m_repeatRounds = 0;
    // What was last written to /ext/nikita, so an unchanged sync is skipped
    // instead of costing a MkDir plus two Writes over USB on every message.
    QString    m_syncedMemory;
    QString    m_syncedSkills;
    // Turns of tool access remaining after an action, so a correction like
    // "you didn't actually create it" arrives with the tools to fix it.
    int        m_toolTurnCooldown = 0;
    bool       m_lastTurnWasAction = false;  // the PREVIOUS turn did a file/device action
                                             // -> keep read tools on for the follow-up question
    // Sticky, for the same reason: "now add a delay to it" is still about the
    // script on the Flipper even though the word never appears in the message.
    bool       m_lastTurnWasDevice = false;
    QString    m_lastSavedPath;              // path of the most recent save_file this session,
                                             // injected into the prompt so "make it fancy" edits
                                             // the SAME file instead of spawning a new one
    bool       m_thinking = false;
    QStringList m_models;   // models discovered via /api/tags
    bool        m_agentEnabled = true;   // computer tools; on unless explicitly turned off
    QString     m_agentRoot;             // absolute workspace folder Nikita may edit
    // SD-card backup. Entries are copied one at a time so the device is never
    // asked for two transfers at once.
    // Walked by hand with storageList + storageRead rather than the utility's
    // downloadDirectory(): that returned a tree of empty folders and skipped
    // most entries outright, and without its source there was no way to tell
    // why. These two primitives are the ones already proven in this feature.
    QString     m_backupRoot;
    QStringList m_backupDirs;     // remote directories still to enumerate
    QList<QPair<QString,qint64>> m_backupPending;  // <remote path, size>
    int         m_backupSkipped = 0;
    int         m_backupTotal = 0;
    int         m_backupFiles = 0;
    qint64      m_backupBytes = 0;
    void backupEnumerateNext();
    void backupCopyNext();
    // Files land in a working folder first, then get packed into bkp.tgz and
    // the folder is dropped, so a half-finished run never leaves a partial
    // archive sitting where a good one should be.
    void backupPackArchive();
    QString m_backupArchive;

    bool    m_transferActive = false;
    QString m_transferTitle;
    QString m_transferNote;
    double  m_transferProgress = -1.0;      // negative means indeterminate
    void    setTransfer(const QString &title, const QString &note, double frac);
    void    endTransfer();
    void    finishTransfer(bool ok, const QString &text);

    QStringList m_formatQueue;
    int         m_formatTotal = 0;
    bool        m_sdFormatted = false;
    void        runFormatQueue();

    QString     m_memory;                // durable facts to remember across sessions
    bool        m_portableLoaded = false; // loaded memory from the Flipper this session

    QByteArray m_streamBuf;       // buffer for partial streamed lines
    // The key is never held in a member: it is read from the environment or
    // QSettings at the moment a request is built and goes straight into the
    // header. Nothing keeps a copy that could be logged or dumped with the
    // rest of the object's state.
    QString    apiKey() const;
    QString    apiModel() const;  // the model id sent to the API
    // The app builds messages in a loose shape: a tool call whose arguments are
    // a JSON object, and a tool result that just follows it. The
    // OpenAI wire format does not: a tool call needs an id and its arguments
    // as a STRING, and every tool result has to name the call it answers.
    // Converted per request rather than stored, so the history on disk stays
    // in the shape everything else in this file reads.
    static QJsonArray toOpenAiMessages(const QJsonArray &msgs);
    static QJsonObject normaliseApiReply(const QJsonObject &resp);
    QString    m_streamContent;   // accumulated reply text this response (one round)
    QString    m_turnText;        // best prose across all rounds of the turn (survives round resets)
    QJsonArray m_streamTools;
    // The final frame, kept only so an empty answer can be explained
    // rather than guessed at (eval_count tells us whether tokens were made).
    QString    m_lastRawFrame;
    // Tool results shown in the chat as they land, so the pane isn't blank
    // during the round trip that turns them into a sentence.
     // accumulated tool calls this response
    void setTurnStatus(const QString &s);
    QString    m_turnStatus;
    qint64     m_turnStartedMs = 0;
    int        m_turnTokens = 0;
    int        m_toolSeq = 0;       // ids handed to toolActivity(), unique per session
    // Estimated prompt tokens for the round in flight, shown in the footer while
    // the model is thinking. The API reports real usage only when the whole
    // reply lands, so without this the counter is blank for the entire wait.
    int        m_turnPromptEstTok = 0;
    int        m_apiRateRetry = 0;  // 429 retries used this turn (see NIKITA_API_MAX_RATE_RETRIES)
    int        m_activeToolSeq = 0; // the call currently running, so its result
                                    // rewrites the row its start opened
    // Trimmed stdout of the most recent command this turn (host_run or run_cli),
    // so a script that must contain a command's result can be filled in by the
    // code -- via the {{LAST_RESULT}} token -- instead of the model retyping the
    // number by hand and risking a transcription error.
    QString    m_lastRunOutput;
    QString    substituteRunResult(const QString &content) const;
    // What this turn has cost in dollars so far, summed across its rounds. Only
    // ever non-zero on the API path; a local turn costs electricity, which this
    // has no way to price and no reason to pretend it can.
    double     m_turnCostUsd = 0.0;
    bool       m_turnTruncated = false;
    QString    m_lastTurnCost;     // "8m 36s · 4.8k tokens", frozen when the turn ends
    QTimer     m_turnTicker;
    QNetworkReply *m_currentReply = nullptr;
    // Set by stopThinking() right before it aborts/kills, so the finish
    // handler can tell "the user stopped this" apart from a genuine error.
    bool m_userStoppedThinking = false;

    // ---- Model manager --------------------------------------------------
    // Used to launch pull/rm so they don't rely on the inherited PATH either.

    // logs once per phase change, then once per 10% within a phase, instead
    // of once per '\r' redraw.
};

// Tracks community Flipper firmwares (Official, Momentum, Unleashed, RogueMaster),
// fetches each one's latest version live, and downloads the update .tgz so
// qFlipper's normal installer (ApplicationBackend::installFirmware) can flash it.
// A Flipper runs one firmware at a time; this makes the latest build of any
// fork one click away; it never flashes without an explicit click.
class FirmwareStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The version string the Flipper reports. Set from QML; every source in
    // sources() is then tagged as installed / up to date / neither, so the UI
    // can offer UPDATE instead of INSTALL for the firmware already running.
    Q_PROPERTY(QString deviceVersion READ deviceVersion WRITE setDeviceVersion NOTIFY changed)
    // Resolved from deviceVersion, for the main screen: which source is running,
    // what its newest build is, and whether that differs from what is installed.
    Q_PROPERTY(int installedIndex READ installedIndex NOTIFY changed)
    Q_PROPERTY(QString installedName READ installedName NOTIFY changed)
    Q_PROPERTY(QString installedLatest READ installedLatest NOTIFY changed)
    Q_PROPERTY(bool installedReady READ installedReady NOTIFY changed)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
    Q_PROPERTY(bool sourceIsNewer READ sourceIsNewer NOTIFY changed)
    Q_PROPERTY(QString installedDate READ installedDate NOTIFY changed)
    Q_PROPERTY(QString installedReleaseUrl READ installedReleaseUrl NOTIFY changed)
    // A firmware picked in the store but not yet flashed. The panel only ever
    // stages a choice; the install button on the main screen is what commits it.
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY changed)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY changed)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY changed)
    Q_PROPERTY(QString selectedVersion READ selectedVersion NOTIFY changed)
    Q_PROPERTY(QString selectedDate READ selectedDate NOTIFY changed)
    // Feedback for the "check for updates" action: how many lookups are still
    // out, and a one-line verdict once they all land.
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(QString checkSummary READ checkSummary NOTIFY changed)
    // The channels of whatever firmware is running, so the update-channel
    // control can describe the actual firmware instead of the official one.
    Q_PROPERTY(QStringList installedChannels READ installedChannels NOTIFY changed)
    // Same list shaped like the stock channel model ({name: ...}), so the
    // existing ChannelDelegate renders it unchanged.
    Q_PROPERTY(QVariantList installedChannelModel READ installedChannelModel NOTIFY changed)
    Q_PROPERTY(QString installedChannel READ installedChannel NOTIFY changed)
    // Which channel the running build was flashed from, when this app did it.
    Q_PROPERTY(QString installedFromChannel READ installedFromChannel NOTIFY changed)
    Q_PROPERTY(bool channelSwitchPending READ channelSwitchPending NOTIFY changed)

public:
    explicit FirmwareStore(QObject *parent = nullptr);

    bool isOpen() const { return m_open; }
    void setOpen(bool value);
    QVariantList sources() const;
    bool busy() const { return m_busy; }
    QString deviceVersion() const { return m_deviceVersion; }
    void setDeviceVersion(const QString &v);
    // A rolling channel names builds by commit hash, so the hash is the only
    // thing that can match what the device reports against what a feed offers.
    void setDeviceCommit(const QString &c);
    // Which channel the running build came from, as the device reports it.
    // installedFromChannel() only knows what this app flashed; this is always
    // true, so it is what a cleared pick can be restored to.
    void setDeviceChannel(const QString &c);
    void setDeviceDate(const QDate &d);

    // Which source the running firmware came from, by the shape of its version
    // string ("mntm-012" -> Momentum). -1 when it can't be told.
    int installedIndex() const;
    QString installedName() const;
    QString installedLatest() const;
    bool installedReady() const;
    bool updateAvailable() const;
    bool sourceIsNewer() const;
    QString installedDate() const;
    QString installedReleaseUrl() const;
    bool checking() const { return m_pending > 0; }
    QString checkSummary() const { return m_checkSummary; }
    QStringList installedChannels() const;
    QVariantList installedChannelModel() const;
    QString installedChannel() const;
    QString installedFromChannel() const;
    bool channelSwitchPending() const;
    Q_INVOKABLE void setInstalledChannel(const QString &id);
    // Re-flash the firmware already on the device, from its own source.
    Q_INVOKABLE void reinstallInstalled();

    int selectedIndex() const { return m_selected; }
    bool hasSelection() const { return m_selected >= 0 && m_selected < m_sources.size(); }
    QString selectedName() const;
    QString selectedVersion() const;
    QString selectedDate() const;

    // Toggles: picking the row that is already staged clears it.
    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void installSelected();

    Q_INVOKABLE void refresh();               // force a re-fetch of every source
    void refreshIfStale();                    // only go to the network if the cache aged out

    // Derived results survive a restart; the raw payloads are too big to keep
    // and don't need to be, since only these fields drive the UI and install.
    void rememberFlashedChannel(int index);
    void noteLookupDone(int index);   // tally a finished lookup; verdict on the last
    void saveDerived(int index) const;
    bool loadDerived(int index);
    Q_INVOKABLE void install(int index);      // download that source's latest .tgz
    Q_INVOKABLE void cycleChannel(int index); // switch a source's channel (release/dev/rc)

signals:
    void openChanged();
    void changed();
    void busyChanged();
    void readyToInstall(const QString &fileUrl);              // hand off to Backend.installFirmware
    void progress(int index, qreal frac, const QString &note);
    void failed(int index, const QString &message);

private:
    enum class Kind { DirJson, GitHub };
    struct Source {
        QString     name;
        Kind        kind;
        QString     locator;      // directory.json URL, or "owner/repo" for GitHub
        QString     blurb;
        QStringList channels;     // available channel ids (discovered for DirJson, fixed for GitHub)
        QString     wantChannel;  // user-selected channel id (persisted); default "release"
        QString     latest;       // discovered version for the selected channel
        QString     date;         // release date of that version, "yyyy-MM-dd"
        QString     tgzUrl;       // discovered download URL for the selected channel
        QString     status;       // "", "checking", "ready", "error"
        QByteArray  raw;          // cached payload, so channel switches need no re-fetch
    };

    void fetchOne(int index);
    void deriveFromCache(int index);                  // recompute latest/tgz for the chosen channel
    QString currentChannelId(const Source &s) const;  // wantChannel, clamped to what's available
    // Declared here, after struct Source: taking one by reference above the
    // struct's own declaration doesn't compile.
    QString channelVersion(const Source &s, const QString &ch) const;
    int distinctChannelCount(const Source &s) const;   // 1 when every channel is the same build
    void setBusy(bool value);

    QNetworkAccessManager m_net;
    QList<Source> m_sources;
    bool m_open = false;
    bool m_busy = false;
    QString m_deviceVersion;
    QString m_deviceCommit;
    QString m_deviceChannel;
    QDate   m_deviceDate;
    bool m_fetchedOnce = false;
    int m_selected = -1;
    int m_pending = 0;               // lookups still in flight
    int m_foundUpdates = 0;          // sources that differ from what is running
    QString m_checkSummary;
    // The GitHub sources share an unauthenticated 60-requests-per-hour budget
    // for the whole machine, and four of them are fetched per refresh. Opening
    // the panel used to spend that budget every time.
    QDateTime m_lastFetch;
};

class QSerialPort;

// In-app Flipper CLI terminal. The serial line shares text-CLI and protobuf
// RPC on one wire, so this pauses RPC (ApplicationBackend::releasePort),
// reopens the same USB serial in text mode, and pipes it to an in-app
// terminal; closing it restarts RPC. USB-only for now. Exposed to QML as `Cli`.
class FlipperCli : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)   // serial link live
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool verbose READ verbose WRITE setVerbose NOTIFY verboseChanged)
    Q_PROPERTY(bool colored READ colored WRITE setColored NOTIFY coloredChanged)
    Q_PROPERTY(QString promptText READ promptText NOTIFY promptChanged)
    // True while a command is in flight (transfer, cd stat, capture, or an armed
    // op guard). The paste queue watches this to send one command at a time.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit FlipperCli(QObject *parent = nullptr);

    void setAppBackend(ApplicationBackend *backend) { m_appBackend = backend; }

    bool isOpen() const { return m_open; }
    void setOpen(bool value);
    bool active() const { return m_active; }
    QString output() const { return m_output; }
    QString status() const { return m_status; }
    bool verbose() const { return m_verbose; }
    void setVerbose(bool value);
    bool colored() const { return m_colored; }
    void setColored(bool value);
    // The panel colours its own output, so it needs to know which prefix is the
    // prompt rather than guessing at it.
    QString promptText() const { return prompt(); }

    Q_INVOKABLE void send(const QString &cmd);   // write a command + CR
    Q_INVOKABLE void interrupt();                // send Ctrl-C
    Q_INVOKABLE void clearOutput();

    // Tab completion. The panel hands over the text to the left of the caret;
    // the reply on completion() is what that text becomes.
    Q_INVOKABLE void complete(const QString &line);
    Q_INVOKABLE QString clipboardText() const;                  // Cmd/Ctrl+V
    Q_INVOKABLE void copyToClipboard(const QString &text) const;

    // Run a single CLI command in isolation (used by the assistant). Pauses RPC,
    // opens the port, sends the command, collects output until the line goes idle,
    // then closes and hands RPC back. Calls done(ok, output) when finished. Refuses
    // if the interactive CLI panel is open or another one-shot is in flight.
    void runOneShot(const QString &cmd, std::function<void(bool, QString)> done);
    bool oneShotBusy() const { return m_runBusy; }

signals:
    void openChanged();
    void activeChanged();
    void outputChanged();
    void statusChanged();
    void verboseChanged();
    void coloredChanged();
    void promptChanged();
    void busyChanged();
    // Tab completion result: the replacement for the text left of the caret.
    void completion(const QString &line);
    // "edit <path>" fetched the file, so whatever panel does host-side text
    // editing can hook this to pop it open instead of just printing it.
    void editRequested(const QString &path, const QString &content);
    // The editor panel finished writing the file back (or hit an error).
    void editSaved(const QString &path);
    void editSaveError(const QString &path, const QString &message);

public:
    // Called by the editor panel's Save button: write the edited text back to
    // the same path. Reuses the storage upload path (remove + write_chunk + md5)
    // so a save here is verified exactly like a file-tool save.
    Q_INVOKABLE void saveEditedFile(const QString &path, const QString &content);

private slots:
    void onReadyRead();

private:
    void connectCli();     // release RPC, open the serial in CLI/text mode
    void disconnectCli();  // close the serial, hand RPC back

    // Shared by connectCli() and the auto-reconnect below: create m_port on the
    // given info, wire it up, reset per-connection state.
    void openPort(const QSerialPortInfo &portInfo, const QString &devName);
    // A command run from inside the panel (name, freboot, factory_reset, an
    // update) drops the device off the bus for a few seconds -- and a rename
    // brings it back under a DIFFERENT serial path. m_port dies with it; this
    // notices, waits for the same physical Flipper to reappear (however it's
    // now named), and reopens the panel on it. The main window already does
    // this on its own path; the CLI panel didn't, so a rename or reboot
    // triggered from inside it used to need the panel closed and reopened.
    void onPortError(QSerialPort::SerialPortError err);
    void beginReconnect();
    void pollReconnect();
    bool m_reconnecting = false;
    QTimer *m_reconnectPoll = nullptr;
    int m_reconnectElapsedMs = 0;

    // One line in, one action out. send() only guarantees that exactly one
    // command reaches this at a time; every decision about what a command means
    // is here, as a switch over the command table's ids.
    void dispatch(const QString &line, const QStringList &tokens);

    // "!12", "!!", "!ls". Returns false when there is no such entry, so the
    // caller can say so instead of silently running the wrong thing.
    bool expandHistory(const QString &token, QString *out) const;
    static QString historyPath();
    void loadHistory();
    void saveHistory() const;
    static QString cliOfflineHelp();

    // rm -r without -f: report what would be destroyed and stop.
    void confirmRemoveTree(const QString &path);

    // The editor's this-computer half, so "edit ~/notes.txt" works the same way
    // "edit /ext/notes.txt" does.
    void startHostEdit(const QString &path);
    void saveHostFile(const QString &path, const QString &content);

    // Run argv on this computer, argv[0] being the program. No shell anywhere:
    // the list goes to execve untouched, so a path with a space or a semicolon
    // in it stays data and can never become syntax.
    void runHostCommand(const QStringList &argv, int timeoutMs = 30000);
    void appendOutput(const QString &text);
    void setActive(bool v);
    void setStatus(const QString &s);
    void finishOneShot(bool ok, const QString &out);   // cleanup + callback

    // Verbose log. Every byte of command this panel puts on the wire, and every
    // reply it swallows internally, is echoed into the terminal so a multi-step
    // op (cp -r, rm -r, find, a transfer) shows its whole trail rather than just
    // the summary line at the end.
    // logIt is false for the one command that is just the user's own line
    // translated (ls -> storage list): the panel already showed what they
    // typed, so repeating the wire form is noise. Everything the CLI runs on
    // its own behalf still goes in the log.
    void writeLine(const QString &cmd, bool logIt = true);

    // One conversation at a time. Every mode below owns the serial line
    // exclusively, so this is the single predicate that decides whether a new
    // one may start; the checks used to be spelled out per call site, which
    // is how "cd" in flight and a raw command could eat each other's replies.
    bool busy() const;

    // busy() is the question the UI and the command queue ask: "is the whole
    // operation still running?" portBusy() is the question the plumbing asks:
    // "is the wire itself mid-exchange right now?" They have to be separate.
    // Every step of a composite operation (echo >> = read + remove +
    // write_chunk + md5) runs while that operation still holds busy(), so a
    // step gating on busy() would refuse its own continuation.
    bool portBusy() const;

    // busyChanged is a direct connection: emitting it from inside onReadyRead
    // let the queue call send() synchronously, part-way through a function that
    // had not finished touching the port. That is how a queued command reached
    // the firmware ahead of an upload's payload and got stored as the file's
    // contents. This posts the signal to the next event-loop turn instead.
    void scheduleBusyChanged();

    // Refcount token for "an operation is still in progress". Capture the
    // handle in every lambda of a chain; when the last copy dies, the CLI
    // reports idle again. A refcount and not a flag, because uploadToFlipper
    // takes one of its own while running inside a cp -r that already holds one.
    std::shared_ptr<void> holdBusy();

    // "storage write_chunk <path> <n>" makes the firmware read exactly n bytes
    // off the wire with no timeout and no way to say no. Abandoning an upload
    // without sending them leaves every later command being swallowed as file
    // content, and the device looks dead until it is unplugged. Ctrl-C is the
    // only way out, and this is it.
    void abortPendingChunk();

    // Holds the line idle for a beat after a forced recovery, so the next
    // command doesn't start with the tail of a cancelled reply in its buffer.
    void settle(int ms);

    // Commands that arrive while something is running are held here rather than
    // bounced. A pasted block arrives far faster than the firmware can answer,
    // and rejecting the overflow meant most of the block silently never ran.
    void queuePending(const QString &cmd);
    void drainPending();
    void clearPending();

    // Last-resort recovery. The op guard only covers a command that is actually
    // in flight; a busy token stranded with nothing on the wire is invisible to
    // it, and that is exactly the state that used to need the panel closed and
    // reopened. This watches the one symptom that is never legitimate: commands
    // waiting, and not one byte from the device for half a minute.
    void armQueueStall();
    void onQueueStall();
    QTimer *m_queueStall = nullptr;

    // No-reply watchdog for the interactive path. Every step that waits on the
    // device arms it; every completion disarms it; incoming bytes restart it,
    // so a slow transfer survives but true silence does not. Without this, a
    // reply that never lands leaves the panel dead with no prompt and no error.
    void armGuard();
    void disarmGuard();
    void onOpTimeout();
    void resetTransientState();   // shared by Ctrl-C, timeout and disconnect
    void trace(const QString &what);         // one wire command
    void traceReply(const QString &raw);     // what came back from it

    // Tab completion: shared tail for both the command-name and the path case.
    void applyCompletion(const QString &head, const QString &token,
                         const QStringList &hits, const QList<bool> &isDir,
                         const QString &original);

    // The firmware streams multi-line replies across several serial chunks, so
    // help listings and directory listings are held back and laid out in one
    // pass once the prompt returns.
    void flushCapture();

    // Host <-> Flipper file copy, driven over the plain CLI (RPC is paused while
    // the panel owns the port). Every transfer is verified with "storage md5"
    // against a local QCryptographicHash before it's called done.
    // exactDest: use devPath verbatim instead of treating a dot-less last
    // segment as a folder to drop the source's filename into.
    void uploadToFlipper(const QString &hostPath, const QString &devPath, bool exactDest = false);
    void downloadFromFlipper(const QString &devPath, const QString &hostPath);
    void finishXfer(bool ok, const QString &message);   // print (or chain to a batch)

    // Generic raw command on the interactive port: write it, buffer the reply
    // until the prompt returns, hand the raw text to the continuation. This is
    // what "storage tree" / "storage list" / "storage md5" / "storage mkdir"
    // run through outside of a file transfer.
    void sendRaw(const QString &cmd, std::function<void(const QString &)> onDone);

    // Recursive / batch ops built on top of the above: cp -r (both directions),
    // rm -r, wildcard cp/rm, and find, all driven off "storage tree" or
    // "storage list", walking the result with the single-file primitives above.
    void ensureDeviceDir(const QString &path, std::function<void()> done);         // mkdir -p
    void startCopyUpTree(const QString &hostRoot, const QString &devRoot);         // cp -r host -> device
    void startCopyDownTree(const QString &devRoot, const QString &hostRoot);       // cp -r device -> host
    // done(ok, existed): "it fought back" and "it was never there" are not the
    // same answer, and only one of them is worth alarming the user about.
    void removeTreeCore(const QString &path, std::function<void(bool, bool)> done);   // no printing; used by both below
    void startRemoveTree(const QString &path);                                     // rm -r, single target
    void runRemoveQueue(const QStringList &targets);                              // rm of a wildcard match set
    void expandDeviceGlob(const QString &pattern, std::function<void(const QStringList &)> done);
    void runUploadQueue(const QStringList &hostFiles, const QString &devDir);   // cp <host glob> -> Flipper
    void runCopyQueue(const QStringList &devMatches, const QString &dst, bool dstHost); // cp of a wildcard match set
    void startFind(const QString &root, const QString &pattern);                   // find
    void startEdit(const QString &path);                                           // edit

    // Text utilities. The Flipper's firmware has no grep/head/tail/wc, and no
    // room to grow one: it is an STM32WB55 with 256 KB of RAM. So the file is
    // read once with "storage read" and the filtering happens on this computer,
    // which is also why they are all built on the one helper below.
    void readDeviceText(const QString &path, std::function<void(bool, const QString &)> done);
    void startGrep(const QString &pattern, const QString &path);
    void startHeadTail(const QString &path, int n, bool head);
    void startWc(const QString &path);
    void startDu(const QString &path);                                             // du
    // Writing: both go out as a normal upload, so they inherit its MD5 check.
    void writeTextToDevice(const QString &text, const QString &devPath, bool append);
    void startWget(const QString &url, const QString &devPath, bool exactDest);    // wget

    // More text utilities over "storage read", same host-does-the-work model.
    void startSed(const QString &expr, const QString &path);                       // sed s/a/b/g
    void startDiff(const QString &pathA, const QString &pathB);                    // diff A B
    void startFileType(const QString &path);                                       // file (magic sniff)
    void startLocate(const QString &pattern);                                      // locate = find from /ext

    // Host-side programs run on THIS computer (the Flipper has no OS/network),
    // their output piped into the terminal. Honest about where they ran.
    // onFinished runs once the process is well and truly done (normal exit,
    // crash, timeout kill, or a failure to even start) -- e.g. to delete a
    // staged temp file the process was reading from, which would be pulled out
    // from under it if deleted the moment runHostProgram() merely returns.
    void runHostProgram(const QString &label, const QString &program,
                        const QStringList &args, int timeoutMs = 8000,
                        std::function<void()> onFinished = nullptr);

    // A genuine live session with a host program that wants to read from
    // stdin repeatedly -- python3 with no script being the motivating case.
    // Unlike runHostProgram() (which closes stdin immediately so an
    // unexpected prompt can't hang forever), this keeps stdin open and, while
    // m_interactiveProc is set, every line typed goes to the child's stdin
    // instead of being parsed as a CLI command. Deliberately no watchdog
    // timeout -- it waits on the user by design. Ends when the program exits
    // on its own (exit()/quit(), EOF, a crash).
    void startInteractiveSession(const QString &label, const QString &program,
                                 const QStringList &args);
    QProcess *m_interactiveProc = nullptr;
    QString m_interactiveLabel;

    ApplicationBackend *m_appBackend = nullptr;
    QSerialPort *m_port = nullptr;
    QString m_output;
    QString m_status;
    bool m_open = false;
    bool m_active = false;
    bool m_verbose = true;   // log everything by default
    bool m_colored = true;   // ls --color style output
    bool m_quiet = false;    // suppresses the log for Tab's own lookup
    // Whether the device has said anything since the port was opened. The
    // firmware announces itself on connect; this stops us asking for a second
    // prompt it already sent.
    bool m_sawDeviceBytes = false;
    // Host-side downloads for "wget".
    QNetworkAccessManager m_dl;
    QStringList m_history;   // commands typed this session, for "history"

    // Shell state: "cd" keeps a current folder that relative paths resolve against.
    QString prompt() const;                      // "Name@qflipper ~/nfc % "
    QString m_devName;                           // the Flipper's own name
    QString m_cwd = QStringLiteral("/ext");
    QString m_cdPrev = QStringLiteral("/ext");   // for "cd -"
    // The working folder on THIS computer, moved with "lcd". Host programs run
    // in it and bare host paths resolve against it, so the panel behaves like a
    // terminal on both machines instead of pinning one of them to $HOME.
    QString m_hostCwd = QDir::homePath();
    static const int kCliHistoryMax     = 500;
    static const int kCliScrollbackMax  = 20000;   // characters held in the view
    static const int kCliScrollbackKeep = 16000;   // kept when it overflows
    static const int kCliMaxEditBytes  = 1 << 20;   // 1 MB, the editor panel's limit
    // Composite operations in flight (see holdBusy). m_opGen invalidates the
    // tokens of an operation that Ctrl-C or the watchdog already tore down, so
    // their deleters can't decrement a count that now belongs to something else.
    int         m_opDepth = 0;
    quint64     m_opGen = 0;
    bool        m_busyNotifyQueued = false;
    bool        m_draining = false;   // reentrancy guard for drainPending
    QStringList m_pending;            // commands waiting for the line

    QString m_cdPending;                         // folder awaiting confirmation
    QByteArray m_cdRaw;

    // Buffered reply reformatting (help listing / directory listing).
    enum class Capture { None, Help, Listing };
    Capture m_capture = Capture::None;
    QString m_captureBuf;
    QTimer *m_captureFlush = nullptr;

    // An ANSI escape sequence cut in half by a read boundary, held back until
    // the rest of it arrives so it can be stripped as one piece.
    QString m_escTail;

    // One bare prompt owed to us: after an upload's payload the firmware prints
    // a prompt before the md5 verification has even been sent, so the command
    // looked finished while it was still being checked.
    bool m_swallowPrompt = false;

    // Echo of a translated command still to be swallowed ("storage list /ext"
    // when the user typed "ls").
    // Folders confirmed to exist during the current batch. Without it a cp -r
    // re-ran the same mkdir for every single file it copied.
    QSet<QString> m_dirsEnsured;

    QString m_echoPending;

    // What the user actually typed this turn. The verbose log skips it: the
    // panel already printed it, and the firmware echoes it back too.
    QString m_lastTyped;

    // The last command written to the wire. The firmware echoes each one back,
    // and the verbose log has to recognise that echo to avoid printing every
    // step twice.
    QString m_lastTraced;

    // Stops a firmware error split across serial chunks from logging twice.
    QString m_lastLoggedError;

    // Coalesces outputChanged so streaming commands can't relayout per chunk.
    QTimer *m_outputTick = nullptr;

    // Fires when the device has said nothing for long enough that whatever we
    // were waiting on is never coming.
    QTimer *m_opGuard = nullptr;

    // File transfer state. "Raw" is the generic one-shot-command state used by
    // sendRaw() (md5 checks, tree/list scans, mkdir, remove): anything that
    // isn't a payload transfer but still needs the reply buffered to the prompt.
    enum class Xfer { None, UploadReady, Download, Raw };
    Xfer m_xfer = Xfer::None;
    QByteArray m_xferPayload;
    QByteArray m_xferRaw;
    QString m_xferLabel;
    QString m_xferHostDst;
    QString m_xferDevPath;               // remote path of the in-flight transfer (for md5 verify)
    qint64 m_xferSize = -1;
    std::function<void(const QString &)> m_rawCb;   // continuation for sendRaw()
    std::function<void(bool)> m_xferChain;          // set by a batch op: called instead of prompting

    // One-shot (assistant) run state, isolated from the interactive panel.
    QSerialPort *m_runPort = nullptr;
    QString m_runBuf;
    bool m_runBusy = false;
    QTimer *m_runIdle = nullptr;
    QTimer *m_runGuard = nullptr;
    std::function<void(bool, QString)> m_runDone;
    // Whether the command being run is expected to drop the USB link (reboot,
    // power off, shutdown). Used so a disconnect during those is not misread as
    // a crash -- for everything else, the link vanishing mid-command IS a crash.
    bool m_runRebootExpected = false;
};