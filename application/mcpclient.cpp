#include "mcpclient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

// The revision of MCP this client implements. Servers negotiate down, so an
// older server still works; a newer one tells us its own version in the
// initialize result and we go on speaking this one.
static const char *MCP_PROTOCOL_VERSION = "2025-06-18";

static const int MCP_HANDSHAKE_TIMEOUT_MS = 20000;
static const int MCP_LIST_TIMEOUT_MS      = 20000;
// A tool call gets the same generous ceiling a shell command gets: an MCP
// server that wraps a build, a model, or a network fetch is not misbehaving
// just because it takes minutes.
static const int MCP_CALL_TIMEOUT_MS      = 300000;

// Chars of tool output handed back to the model. Past this the payload stops
// being information and starts being the whole context window.
static const int MCP_RESULT_CAP = 60000;

static const char *kMcpEnabledKey = "nikita/mcpEnabled";

QString McpClient::prefix() { return QStringLiteral("mcp__"); }

bool McpClient::isMcpTool(const QString &name)
{
    return name.startsWith(prefix());
}

static QString mcpJsonError(const QString &message)
{
    QJsonObject o{{QStringLiteral("error"), message}};
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

McpClient::McpClient(QObject *parent)
    : QObject(parent)
{
    QSettings s;
    m_enabled = s.value(QLatin1String(kMcpEnabledKey), true).toBool();
}

McpClient::~McpClient()
{
    shutdown();
    qDeleteAll(m_servers);
    m_servers.clear();
}

bool McpClient::enabled() const { return m_enabled; }

void McpClient::setEnabled(bool on)
{
    if (m_enabled == on) { return; }
    m_enabled = on;
    QSettings().setValue(QLatin1String(kMcpEnabledKey), on);
    if (!on) { shutdown(); }
    else     { reload(); }
    emit changed();
}

void McpClient::setWorkspace(const QString &dir)
{
    if (m_workspace == dir) { return; }
    m_workspace = dir;
    if (m_enabled) { reload(); }
}

// ---- Device identity -----------------------------------------------------

void McpClient::setDeviceIdentity(const QString &rawDeviceId)
{
    // Normalised before anything else, so the SAME Flipper produces the same
    // pseudonym in every client of this ecosystem. The desktop reads the USB
    // serial number and the phone reads hardware.uid over RPC; for a Flipper
    // Zero those are the same STM32 value, but they arrive spelled differently
    // -- different case, sometimes separators. Hashing the spelling instead of
    // the value would give one device two identities, and a server keeping
    // per-device state would then see two devices.
    QString raw;
    for (const QChar &c : rawDeviceId) {
        if (c.isLetterOrNumber()) { raw.append(c.toUpper()); }
    }
    if (raw == m_rawDeviceId) { return; }
    m_rawDeviceId = raw;
    if (raw.isEmpty()) {
        m_deviceId.clear();
        log(QStringLiteral("no device attached; calls carry no device identity"));
    } else {
        // Truncated to 32 hex. A full sha256 is 64 characters of header on
        // every request for no gain: 128 bits is far past collision range for
        // a population of Flipper Zeros.
        m_deviceId = QString::fromLatin1(
            QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256)
                .toHex().left(32));
        // The pseudonym is logged; the raw id never is. A log file is a place
        // a serial number leaks from.
        log(QStringLiteral("device identity: %1").arg(m_deviceId));
    }
    emit changed();
}

QString McpClient::deviceId() const { return m_deviceId; }

QString McpClient::serverSecret(const McpServerDef &def)
{
    if (!def.token.isEmpty()) { return def.token; }
    // A bearer header is a secret by another name. Take the credential out of
    // "Bearer xyz" rather than signing with the word "Bearer" included.
    for (auto it = def.headers.cbegin(); it != def.headers.cend(); ++it) {
        if (it.key().compare(QLatin1String("authorization"),
                             Qt::CaseInsensitive) != 0) { continue; }
        QString value = it.value().trimmed();
        if (value.startsWith(QLatin1String("Bearer "), Qt::CaseInsensitive)) {
            value = value.mid(7).trimmed();
        }
        return value;
    }
    return QString();
}

QString McpClient::deviceAuthFor(const Server *s, const QString &method,
                                 const QString &tool) const
{
    if (!s || m_deviceId.isEmpty()) { return QString(); }
    // No signature for a local child: it is not a security boundary, and the
    // key would have to come from somewhere it does not exist.
    if (s->def.kind != QLatin1String("http")) { return QString(); }
    const QString secret = serverSecret(s->def);
    if (secret.isEmpty()) { return QString(); }

    const qint64 ts = QDateTime::currentSecsSinceEpoch();
    // 128 bits from the system CSPRNG. A nonce a server has already seen is a
    // replay, so it has to be unguessable and never repeat.
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QByteArray nonceBytes;
    for (quint32 w : words) {
        nonceBytes.append(reinterpret_cast<const char *>(&w), sizeof(w));
    }
    const QString nonce = QString::fromLatin1(nonceBytes.toHex());

    // Everything the server needs to bind the claim to THIS call. The method
    // and tool are inside, so a signature cannot be moved from tools/list to a
    // tools/call, or from a harmless tool to a destructive one.
    const QByteArray payload = QStringLiteral("v1|%1|%2|%3|%4|%5")
                                   .arg(m_deviceId)
                                   .arg(ts)
                                   .arg(nonce, method, tool)
                                   .toUtf8();
    const QString mac = QString::fromLatin1(
        QMessageAuthenticationCode::hash(payload, secret.toUtf8(),
                                         QCryptographicHash::Sha256).toHex());

    return QStringLiteral("v1:%1:%2:%3:%4").arg(m_deviceId).arg(ts).arg(nonce, mac);
}

QJsonObject McpClient::callMeta(const Server *s, const QString &tool) const
{
    QJsonObject meta;
    if (m_deviceId.isEmpty()) { return meta; }
    // Namespaced keys, because `_meta` is shared ground: an unprefixed
    // "device" would be this client claiming a name it does not own.
    meta[QStringLiteral("nikita/client")] = QStringLiteral("nikita-qflipper");
    meta[QStringLiteral("nikita/device")] = m_deviceId;
    const QString auth = deviceAuthFor(s, QStringLiteral("tools/call"), tool);
    if (!auth.isEmpty()) { meta[QStringLiteral("nikita/deviceAuth")] = auth; }
    return meta;
}

void McpClient::log(const QString &text) const
{
    // const_cast so logging stays available from const accessors; the signal
    // is the only thing it touches.
    emit const_cast<McpClient *>(this)->logLine(QStringLiteral("mcp: ") + text);
}

QString McpClient::configPath() const
{
    return QDir::homePath() + QStringLiteral("/.nikita/mcp.json");
}

// ---- Config --------------------------------------------------------------

QString McpClient::expandVars(const QString &in)
{
    if (!in.contains(QLatin1String("${"))) { return in; }
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString out = in;
    static const QRegularExpression re(QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}"));
    QRegularExpressionMatch m = re.match(out);
    while (m.hasMatch()) {
        const QString key = m.captured(1);
        // An unset variable is left standing rather than blanked: "${TOKEN}"
        // in the log is a readable mistake, an empty string is a silent one.
        const QString value = env.contains(key) ? env.value(key) : m.captured(0);
        if (value == m.captured(0)) { break; }
        out.replace(m.capturedStart(0), m.capturedLength(0), value);
        m = re.match(out);
    }
    return out;
}

void McpClient::parseServerMap(const QJsonObject &map, const QString &source,
                               QList<McpServerDef> *out)
{
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        const QString name = it.key();
        if (name.isEmpty() || !it.value().isObject()) { continue; }
        // First source wins, so a name already claimed is skipped rather than
        // overwritten -- that is what makes the priority order mean anything.
        bool already = false;
        for (const McpServerDef &d : *out) {
            if (d.name == name) { already = true; break; }
        }
        if (already) { continue; }

        const QJsonObject o = it.value().toObject();

        // Nikita's brain is the Kimi API and nothing else -- no local model, in
        // the chat or behind a tool. A server wired to a local runtime is
        // skipped rather than adopted: inheriting Claude Code's config is a
        // convenience, not permission to bring a second model in through the
        // side door. The check is on the configuration, not the name, so a
        // server called something else with the same wiring is caught too.
        {
            const QString blob = QString::fromUtf8(
                QJsonDocument(o).toJson(QJsonDocument::Compact)).toLower();
            static const char *kLocalModelMarkers[] = {
                "ollama", "llama.cpp", "lmstudio", "lm-studio", "127.0.0.1:11434",
                "localhost:11434"
            };
            bool localModel = false;
            for (const char *marker : kLocalModelMarkers) {
                if (blob.contains(QLatin1String(marker))) { localModel = true; break; }
            }
            if (localModel) { continue; }
        }

        McpServerDef def;
        def.name = name;
        def.source = source;

        const QString declared = o.value(QStringLiteral("type")).toString().toLower();
        const QString url = expandVars(o.value(QStringLiteral("url")).toString());
        // "type" is optional in the wild: a command means stdio, a url means
        // http, and that inference is more reliable than the field.
        if (declared == QLatin1String("http") || declared == QLatin1String("sse")
            || (declared.isEmpty() && !url.isEmpty())) {
            def.kind = QStringLiteral("http");
            def.url = url;
            const QJsonObject h = o.value(QStringLiteral("headers")).toObject();
            for (auto hit = h.constBegin(); hit != h.constEnd(); ++hit) {
                def.headers.insert(hit.key(), expandVars(hit.value().toString()));
            }
            def.token = expandVars(o.value(QStringLiteral("token")).toString());
            if (def.url.isEmpty()) { continue; }
        } else {
            def.kind = QStringLiteral("stdio");
            def.command = expandVars(o.value(QStringLiteral("command")).toString());
            const QJsonArray a = o.value(QStringLiteral("args")).toArray();
            for (const QJsonValue &v : a) { def.args += expandVars(v.toString()); }
            const QJsonObject e = o.value(QStringLiteral("env")).toObject();
            for (auto eit = e.constBegin(); eit != e.constEnd(); ++eit) {
                def.env.insert(eit.key(), expandVars(eit.value().toString()));
            }
            if (def.command.isEmpty()) { continue; }
        }
        out->append(def);
    }
}

QList<McpServerDef> McpClient::readConfig() const
{
    QList<McpServerDef> out;

    auto readFile = [](const QString &path) -> QJsonObject {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) { return QJsonObject(); }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            return QJsonObject();
        }
        return doc.object();
    };

    // 1. Nikita's own file. Accepts either { "mcpServers": {...} } or the bare
    //    map, because both spellings are in circulation and rejecting one is a
    //    puzzle for the user rather than a safeguard.
    {
        const QJsonObject o = readFile(configPath());
        const QJsonObject map = o.contains(QStringLiteral("mcpServers"))
                                    ? o.value(QStringLiteral("mcpServers")).toObject()
                                    : o;
        parseServerMap(map, QStringLiteral("~/.nikita/mcp.json"), &out);
    }

    // 2. Claude Code's user config, so a server set up there is simply here.
    {
        const QJsonObject o = readFile(QDir::homePath() + QStringLiteral("/.claude.json"));
        parseServerMap(o.value(QStringLiteral("mcpServers")).toObject(),
                       QStringLiteral("~/.claude.json"), &out);
    }

    // 3. The project-scoped file, from the workspace Nikita is pointed at.
    if (!m_workspace.isEmpty()) {
        const QString path = m_workspace + QStringLiteral("/.mcp.json");
        const QJsonObject o = readFile(path);
        const QJsonObject map = o.contains(QStringLiteral("mcpServers"))
                                    ? o.value(QStringLiteral("mcpServers")).toObject()
                                    : o;
        parseServerMap(map, QStringLiteral(".mcp.json"), &out);
    }

    return out;
}

// ---- Lifecycle -----------------------------------------------------------

void McpClient::reload()
{
    if (!m_enabled) { emit changed(); return; }

    const QList<McpServerDef> defs = readConfig();

    // Drop servers that left the config, and anything whose definition changed
    // under it -- a command or url edit has to reach a running process.
    QList<Server *> keep;
    for (Server *s : m_servers) {
        const McpServerDef *match = nullptr;
        for (const McpServerDef &d : defs) {
            if (d.name == s->def.name) { match = &d; break; }
        }
        const bool same = match
                       && match->kind == s->def.kind
                       && match->command == s->def.command
                       && match->args == s->def.args
                       && match->url == s->def.url;
        if (same) { keep.append(s); continue; }
        if (s->proc) { s->proc->kill(); s->proc->waitForFinished(1500); delete s->proc; s->proc = nullptr; }
        delete s;
    }
    m_servers = keep;

    for (const McpServerDef &d : defs) {
        if (server(d.name)) { continue; }
        Server *s = new Server;
        s->def = d;
        m_servers.append(s);
        log(QStringLiteral("configured %1 (%2, from %3)").arg(d.name, d.kind, d.source));
    }

    // Connect eagerly. The first turn should already be able to see the tools:
    // a server discovered only when the model happens to ask for it is a
    // server the model never knew it had.
    for (Server *s : m_servers) {
        if (s->state == QLatin1String("idle") || s->state == QLatin1String("failed")) {
            ensureReady(s->def.name, [](bool, const QString &) {});
        }
    }
    emit changed();
}

void McpClient::shutdown()
{
    for (Server *s : m_servers) {
        for (auto it = s->pending.begin(); it != s->pending.end(); ++it) {
            if (it->timer) { it->timer->stop(); it->timer->deleteLater(); }
            if (it->cb) { it->cb(QJsonObject(), QStringLiteral("MCP shut down")); }
        }
        s->pending.clear();
        resolveWaiters(s, false, QStringLiteral("MCP shut down"));
        if (s->proc) {
            s->proc->closeWriteChannel();
            s->proc->terminate();
            if (!s->proc->waitForFinished(1500)) { s->proc->kill(); s->proc->waitForFinished(500); }
            delete s->proc;
            s->proc = nullptr;
        }
        s->tools = QJsonArray();
        s->state = QStringLiteral("idle");
    }
    emit changed();
}

McpClient::Server *McpClient::server(const QString &name)
{
    for (Server *s : m_servers) {
        if (s->def.name == name) { return s; }
    }
    return nullptr;
}

QStringList McpClient::serverNames() const
{
    QStringList out;
    for (Server *s : m_servers) { out += s->def.name; }
    return out;
}

int McpClient::toolCount() const
{
    int n = 0;
    for (Server *s : m_servers) { n += s->tools.size(); }
    return n;
}

QString McpClient::statusLine() const
{
    if (!m_enabled) { return QStringLiteral("MCP off"); }
    if (m_servers.isEmpty()) { return QStringLiteral("no MCP servers configured"); }
    int ready = 0, failed = 0;
    for (Server *s : m_servers) {
        if (s->state == QLatin1String("ready")) { ++ready; }
        else if (s->state == QLatin1String("failed")) { ++failed; }
    }
    QString out = QStringLiteral("%1/%2 server(s), %3 tool(s)")
                      .arg(ready).arg(m_servers.size()).arg(toolCount());
    if (failed > 0) { out += QStringLiteral(" - %1 failed").arg(failed); }
    return out;
}

QVariantList McpClient::serverList() const
{
    QVariantList out;
    for (Server *s : m_servers) {
        QVariantMap m;
        m[QStringLiteral("name")]   = s->def.name;
        m[QStringLiteral("kind")]   = s->def.kind;
        m[QStringLiteral("state")]  = s->state;
        m[QStringLiteral("tools")]  = s->tools.size();
        m[QStringLiteral("error")]  = s->lastError;
        m[QStringLiteral("source")] = s->def.source;
        m[QStringLiteral("label")]  = s->serverLabel;
        // The command, but never the environment: an env map is where a token
        // lives.
        m[QStringLiteral("detail")] = s->def.kind == QLatin1String("http")
                                          ? s->def.url
                                          : (s->def.command + QLatin1Char(' ')
                                             + s->def.args.join(QLatin1Char(' ')));
        out.append(m);
    }
    return out;
}

void McpClient::resolveWaiters(Server *s, bool ok, const QString &error)
{
    const QList<std::function<void(bool, const QString &)>> waiters = s->waiters;
    s->waiters.clear();
    for (const auto &w : waiters) { w(ok, error); }
}

void McpClient::fail(Server *s, const QString &why)
{
    s->state = QStringLiteral("failed");
    s->lastError = why;
    ++s->failures;
    s->tools = QJsonArray();
    log(QStringLiteral("%1 failed: %2").arg(s->def.name, why));

    // Anything still in flight is answered, not dropped: a tool call whose
    // callback never fires freezes the turn it belongs to.
    const QHash<int, Pending> pending = s->pending;
    s->pending.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (it->timer) { it->timer->stop(); it->timer->deleteLater(); }
        if (it->cb) { it->cb(QJsonObject(), why); }
    }
    resolveWaiters(s, false, why);
    emit changed();
}

void McpClient::ensureReady(const QString &name,
                            std::function<void(bool ok, const QString &error)> done)
{
    Server *s = server(name);
    if (!s) { done(false, QStringLiteral("no MCP server named \"%1\"").arg(name)); return; }
    if (s->state == QLatin1String("ready")) { done(true, QString()); return; }

    s->waiters.append(done);
    if (s->state == QLatin1String("starting")) { return; }

    s->lastError.clear();
    s->state = QStringLiteral("starting");
    emit changed();

    if (s->def.kind == QLatin1String("stdio")) { startStdio(s); }
    else                                       { handshake(s); }
}

void McpClient::startStdio(Server *s)
{
    QProcess *p = new QProcess(this);
    s->proc = p;
    s->buf.clear();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = s->def.env.cbegin(); it != s->def.env.cend(); ++it) {
        env.insert(it.key(), it.value());
    }
    // The pseudonym only. A stdio child is local and already trusted, so it
    // needs no signature -- and a secret in a child's environment is a secret
    // in `ps`.
    if (!m_deviceId.isEmpty()) {
        env.insert(QStringLiteral("NIKITA_CLIENT"), QStringLiteral("nikita-qflipper"));
        env.insert(QStringLiteral("NIKITA_DEVICE"), m_deviceId);
    }
    p->setProcessEnvironment(env);
    // Separate channels: a server that writes a banner or a warning to stderr
    // must not have it land in the middle of the JSON on stdout.
    p->setProcessChannelMode(QProcess::SeparateChannels);

    const QString name = s->def.name;
    connect(p, &QProcess::readyReadStandardOutput, this, [this, name]() {
        if (Server *sv = server(name)) { onStdioReadable(sv); }
    });
    connect(p, &QProcess::readyReadStandardError, this, [this, name]() {
        Server *sv = server(name);
        if (!sv || !sv->proc) { return; }
        const QString text = QString::fromUtf8(sv->proc->readAllStandardError()).trimmed();
        if (!text.isEmpty()) { log(QStringLiteral("%1 stderr: %2").arg(name, text.left(400))); }
    });
    connect(p, &QProcess::errorOccurred, this, [this, name](QProcess::ProcessError) {
        Server *sv = server(name);
        if (!sv || !sv->proc) { return; }
        if (sv->state == QLatin1String("failed")) { return; }
        fail(sv, sv->proc->errorString());
    });
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, name](int code, QProcess::ExitStatus) {
        Server *sv = server(name);
        if (!sv) { return; }
        if (sv->state == QLatin1String("failed")) { return; }
        fail(sv, QStringLiteral("server exited (code %1)").arg(code));
    });

    log(QStringLiteral("starting %1: %2 %3")
            .arg(s->def.name, s->def.command, s->def.args.join(QLatin1Char(' '))));
    p->start(s->def.command, s->def.args);
    if (!p->waitForStarted(8000)) {
        fail(s, QStringLiteral("could not start \"%1\": %2")
                    .arg(s->def.command, p->errorString()));
        return;
    }
    handshake(s);
}

// ---- JSON-RPC ------------------------------------------------------------

void McpClient::request(Server *s, const QString &method, const QJsonObject &params,
                        int timeoutMs,
                        std::function<void(const QJsonObject &, const QString &)> cb)
{
    const int id = s->nextId++;

    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    const QString name = s->def.name;
    connect(timer, &QTimer::timeout, this, [this, name, id, method, timeoutMs]() {
        Server *sv = server(name);
        if (!sv) { return; }
        auto it = sv->pending.find(id);
        if (it == sv->pending.end()) { return; }
        auto cb = it->cb;
        if (it->timer) { it->timer->deleteLater(); }
        sv->pending.erase(it);
        if (cb) {
            cb(QJsonObject(), QStringLiteral("%1 timed out after %2s on %3")
                                  .arg(name).arg(timeoutMs / 1000).arg(method));
        }
    });

    Pending pending;
    pending.cb = cb;
    pending.timer = timer;
    s->pending.insert(id, pending);
    timer->start(timeoutMs);

    QJsonObject msg{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method}
    };
    if (!params.isEmpty()) { msg[QStringLiteral("params")] = params; }

    if (s->def.kind == QLatin1String("stdio")) { sendStdio(s, msg); }
    else                                       { sendHttp(s, msg, true); }
}

void McpClient::notify(Server *s, const QString &method, const QJsonObject &params)
{
    QJsonObject msg{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), method}
    };
    if (!params.isEmpty()) { msg[QStringLiteral("params")] = params; }
    if (s->def.kind == QLatin1String("stdio")) { sendStdio(s, msg); }
    else                                       { sendHttp(s, msg, false); }
}

void McpClient::sendStdio(Server *s, const QJsonObject &message)
{
    if (!s->proc || s->proc->state() != QProcess::Running) {
        fail(s, QStringLiteral("server is not running"));
        return;
    }
    // Compact, so the payload cannot contain the newline that delimits it.
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    s->proc->write(line);
}

void McpClient::sendHttp(Server *s, const QJsonObject &message, bool expectsReply)
{
    QNetworkRequest req(QUrl(s->def.url));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Both, because Streamable HTTP lets the server answer either way and the
    // client is required to accept both.
    req.setRawHeader("Accept", "application/json, text/event-stream");
    req.setRawHeader("MCP-Protocol-Version", MCP_PROTOCOL_VERSION);
    if (!s->sessionId.isEmpty()) {
        req.setRawHeader("Mcp-Session-Id", s->sessionId.toUtf8());
    }
    for (auto it = s->def.headers.cbegin(); it != s->def.headers.cend(); ++it) {
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    // Which Flipper is asking, and the signed claim when there is a secret to
    // sign with. Derived from the message so the signature covers the method
    // and, for a tools/call, the tool name.
    if (!m_deviceId.isEmpty()) {
        req.setRawHeader("X-Nikita-Client", "nikita-qflipper");
        req.setRawHeader("X-Nikita-Device", m_deviceId.toUtf8());
        const QString method = message.value(QStringLiteral("method")).toString();
        const QString tool = message.value(QStringLiteral("params")).toObject()
                                 .value(QStringLiteral("name")).toString();
        const QString auth = deviceAuthFor(s, method, tool);
        if (!auth.isEmpty()) {
            req.setRawHeader("X-Nikita-Device-Auth", auth.toUtf8());
        }
    }

    QNetworkReply *reply = m_net.post(req, QJsonDocument(message).toJson(QJsonDocument::Compact));
    m_replyServer.insert(reply, s->def.name);
    const bool wantReply = expectsReply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, wantReply]() {
        const QString name = m_replyServer.take(reply);
        reply->deleteLater();
        Server *sv = server(name);
        if (!sv) { return; }

        const QByteArray sid = reply->rawHeader("Mcp-Session-Id");
        if (!sid.isEmpty()) { sv->sessionId = QString::fromUtf8(sid); }

        if (reply->error() != QNetworkReply::NoError) {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString body = QString::fromUtf8(reply->readAll()).left(300);
            fail(sv, code > 0 ? QStringLiteral("HTTP %1: %2").arg(code).arg(body)
                              : reply->errorString());
            return;
        }
        if (!wantReply) { return; }

        const QByteArray payload = reply->readAll();
        const QString ctype = reply->header(QNetworkRequest::ContentTypeHeader).toString();

        // An SSE body carries the JSON-RPC message inside data: lines. One
        // response per request here, so every frame is delivered as it is
        // found rather than trying to match the id -- deliver() ignores
        // anything it has no pending entry for.
        if (ctype.contains(QLatin1String("text/event-stream"))) {
            const QList<QByteArray> lines = payload.split('\n');
            for (const QByteArray &raw : lines) {
                const QByteArray line = raw.trimmed();
                if (!line.startsWith("data:")) { continue; }
                const QByteArray json = line.mid(5).trimmed();
                if (json.isEmpty() || json == "[DONE]") { continue; }
                const QJsonDocument doc = QJsonDocument::fromJson(json);
                if (doc.isObject()) { deliver(sv, doc.object()); }
            }
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (doc.isObject())     { deliver(sv, doc.object()); }
        else if (doc.isArray()) {
            const QJsonArray a = doc.array();
            for (const QJsonValue &v : a) {
                if (v.isObject()) { deliver(sv, v.toObject()); }
            }
        }
    });
}

void McpClient::onStdioReadable(Server *s)
{
    if (!s->proc) { return; }
    s->buf += s->proc->readAllStandardOutput();
    while (true) {
        const int nl = s->buf.indexOf('\n');
        if (nl < 0) { break; }
        const QByteArray line = s->buf.left(nl).trimmed();
        s->buf.remove(0, nl + 1);
        if (line.isEmpty()) { continue; }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            // Servers do print the occasional non-JSON line. Noted, not fatal.
            log(QStringLiteral("%1 sent a non-JSON line: %2")
                    .arg(s->def.name, QString::fromUtf8(line.left(200))));
            continue;
        }
        deliver(s, doc.object());
    }
}

void McpClient::deliver(Server *s, const QJsonObject &message)
{
    // A server-initiated request (sampling, roots, elicitation) is answered
    // with "not supported" rather than ignored: leaving it unanswered leaves
    // the server waiting forever, which looks like a hang on our side.
    if (message.contains(QStringLiteral("method"))
        && message.contains(QStringLiteral("id"))) {
        QJsonObject err{
            {QStringLiteral("code"), -32601},
            {QStringLiteral("message"), QStringLiteral("not supported by this client")}
        };
        QJsonObject resp{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), message.value(QStringLiteral("id"))},
            {QStringLiteral("error"), err}
        };
        if (s->def.kind == QLatin1String("stdio")) { sendStdio(s, resp); }
        return;
    }

    // A notification from the server. The one worth acting on is a changed
    // tool list.
    if (message.contains(QStringLiteral("method"))) {
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QLatin1String("notifications/tools/list_changed")
            && s->state == QLatin1String("ready")) {
            log(QStringLiteral("%1 says its tools changed; re-listing").arg(s->def.name));
            listTools(s, QString());
        }
        return;
    }

    if (!message.contains(QStringLiteral("id"))) { return; }
    const int id = message.value(QStringLiteral("id")).toInt(-1);
    auto it = s->pending.find(id);
    if (it == s->pending.end()) { return; }
    auto cb = it->cb;
    if (it->timer) { it->timer->stop(); it->timer->deleteLater(); }
    s->pending.erase(it);
    if (!cb) { return; }

    if (message.contains(QStringLiteral("error"))) {
        const QJsonObject e = message.value(QStringLiteral("error")).toObject();
        QString text = e.value(QStringLiteral("message")).toString();
        if (text.isEmpty()) { text = QStringLiteral("JSON-RPC error"); }
        const int code = e.value(QStringLiteral("code")).toInt();
        cb(QJsonObject(), QStringLiteral("%1 (code %2)").arg(text).arg(code));
        return;
    }
    cb(message.value(QStringLiteral("result")).toObject(), QString());
}

// ---- Handshake and tool discovery ----------------------------------------

void McpClient::handshake(Server *s)
{
    QJsonObject params{
        {QStringLiteral("protocolVersion"), QLatin1String(MCP_PROTOCOL_VERSION)},
        {QStringLiteral("capabilities"), QJsonObject{}},
        {QStringLiteral("clientInfo"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("nikita-qflipper")},
            {QStringLiteral("version"), QCoreApplication::applicationVersion()}
        }}
    };

    const QString name = s->def.name;
    request(s, QStringLiteral("initialize"), params, MCP_HANDSHAKE_TIMEOUT_MS,
            [this, name](const QJsonObject &result, const QString &error) {
        Server *sv = server(name);
        if (!sv) { return; }
        if (!error.isEmpty()) { fail(sv, error); return; }

        const QJsonObject info = result.value(QStringLiteral("serverInfo")).toObject();
        sv->serverLabel = info.value(QStringLiteral("name")).toString();
        const QString version = info.value(QStringLiteral("version")).toString();
        if (!version.isEmpty()) { sv->serverLabel += QLatin1Char(' ') + version; }

        // The spec requires this before any other request.
        notify(sv, QStringLiteral("notifications/initialized"), QJsonObject());
        listTools(sv, QString());
    });
}

void McpClient::listTools(Server *s, const QString &cursor)
{
    if (cursor.isEmpty()) { s->tools = QJsonArray(); }

    QJsonObject params;
    if (!cursor.isEmpty()) { params[QStringLiteral("cursor")] = cursor; }

    const QString name = s->def.name;
    request(s, QStringLiteral("tools/list"), params, MCP_LIST_TIMEOUT_MS,
            [this, name](const QJsonObject &result, const QString &error) {
        Server *sv = server(name);
        if (!sv) { return; }
        if (!error.isEmpty()) { fail(sv, error); return; }

        const QJsonArray tools = result.value(QStringLiteral("tools")).toArray();
        for (const QJsonValue &v : tools) {
            const QJsonObject t = v.toObject();
            const QString bare = t.value(QStringLiteral("name")).toString();
            if (bare.isEmpty()) { continue; }

            // The description the model reads. An MCP description can be
            // enormous; the model needs to know what the tool does and where
            // it lives, not a manual.
            QString desc = t.value(QStringLiteral("description")).toString();
            if (desc.isEmpty()) { desc = QStringLiteral("Tool provided by the MCP server."); }
            desc = QStringLiteral("[MCP: %1] ").arg(name) + desc.left(900);

            QJsonObject schema = t.value(QStringLiteral("inputSchema")).toObject();
            // A function schema with no type is rejected by the API, and some
            // servers ship an empty one for a no-argument tool.
            if (!schema.contains(QStringLiteral("type"))) {
                schema[QStringLiteral("type")] = QStringLiteral("object");
            }
            if (!schema.contains(QStringLiteral("properties"))) {
                schema[QStringLiteral("properties")] = QJsonObject{};
            }

            sv->tools.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), prefix() + name + QStringLiteral("__") + bare},
                    {QStringLiteral("description"), desc},
                    {QStringLiteral("parameters"), schema}
                }}
            });
        }

        const QString next = result.value(QStringLiteral("nextCursor")).toString();
        if (!next.isEmpty()) { listTools(sv, next); return; }

        sv->state = QStringLiteral("ready");
        sv->lastError.clear();
        log(QStringLiteral("%1 ready: %2 tool(s)%3")
                .arg(name).arg(sv->tools.size())
                .arg(sv->serverLabel.isEmpty() ? QString()
                                               : QStringLiteral(" (%1)").arg(sv->serverLabel)));
        resolveWaiters(sv, true, QString());
        emit changed();
    });
}

// ---- What the model is offered -------------------------------------------

QJsonArray McpClient::toolSchemas(int cap) const
{
    QJsonArray out;
    if (!m_enabled) { return out; }
    // Round-robin across servers rather than first-come, so one large server
    // cannot use up the whole budget and hide a small one entirely.
    int index = 0;
    bool more = true;
    while (more && out.size() < cap) {
        more = false;
        for (Server *s : m_servers) {
            if (s->state != QLatin1String("ready")) { continue; }
            if (index >= s->tools.size()) { continue; }
            more = true;
            out.append(s->tools.at(index));
            if (out.size() >= cap) { break; }
        }
        ++index;
    }
    return out;
}

// ---- Calling -------------------------------------------------------------

void McpClient::callTool(const QString &fullName, const QJsonObject &args,
                         std::function<void(const QString &)> done)
{
    if (!m_enabled) {
        done(mcpJsonError(QStringLiteral("MCP is switched off in Nikita settings.")));
        return;
    }
    if (!isMcpTool(fullName)) {
        done(mcpJsonError(QStringLiteral("not an MCP tool: %1").arg(fullName)));
        return;
    }

    // mcp__<server>__<tool>. The server name cannot contain the separator, so
    // splitting on the first "__" after the prefix is unambiguous; the tool
    // name keeps whatever it contains.
    const QString rest = fullName.mid(prefix().size());
    const int sep = rest.indexOf(QStringLiteral("__"));
    if (sep <= 0) {
        done(mcpJsonError(QStringLiteral("malformed MCP tool name: %1").arg(fullName)));
        return;
    }
    const QString serverName = rest.left(sep);
    const QString toolName   = rest.mid(sep + 2);

    ensureReady(serverName, [this, serverName, toolName, args, done](bool ok, const QString &error) {
        if (!ok) {
            done(mcpJsonError(QStringLiteral("MCP server \"%1\" is not available: %2")
                                  .arg(serverName, error)));
            return;
        }
        Server *sv = server(serverName);
        if (!sv) {
            done(mcpJsonError(QStringLiteral("MCP server \"%1\" went away").arg(serverName)));
            return;
        }

        QJsonObject params{
            {QStringLiteral("name"), toolName},
            {QStringLiteral("arguments"), args}
        };
        // The transport-neutral carrier, and the only one that follows a device
        // being unplugged and replaced under a server that keeps running.
        const QJsonObject meta = callMeta(sv, toolName);
        if (!meta.isEmpty()) { params[QStringLiteral("_meta")] = meta; }
        request(sv, QStringLiteral("tools/call"), params, MCP_CALL_TIMEOUT_MS,
                [serverName, toolName, done](const QJsonObject &result, const QString &error) {
            if (!error.isEmpty()) {
                done(mcpJsonError(QStringLiteral("%1/%2: %3").arg(serverName, toolName, error)));
                return;
            }

            // Flatten the content blocks into the text the model reads. Text
            // blocks go through as they are; anything else is named rather
            // than dropped, so "it returned an image" is visible instead of
            // looking like an empty result.
            QString text;
            const QJsonArray content = result.value(QStringLiteral("content")).toArray();
            for (const QJsonValue &v : content) {
                const QJsonObject b = v.toObject();
                const QString type = b.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("text")) {
                    if (!text.isEmpty()) { text += QLatin1Char('\n'); }
                    text += b.value(QStringLiteral("text")).toString();
                } else if (type == QLatin1String("resource")) {
                    const QJsonObject r = b.value(QStringLiteral("resource")).toObject();
                    const QString inner = r.value(QStringLiteral("text")).toString();
                    if (!inner.isEmpty()) {
                        if (!text.isEmpty()) { text += QLatin1Char('\n'); }
                        text += inner;
                    } else {
                        if (!text.isEmpty()) { text += QLatin1Char('\n'); }
                        text += QStringLiteral("(resource: %1)")
                                    .arg(r.value(QStringLiteral("uri")).toString());
                    }
                } else {
                    if (!text.isEmpty()) { text += QLatin1Char('\n'); }
                    text += QStringLiteral("(%1 content, not shown)").arg(type);
                }
            }

            // Structured output, when the server provides it and there was no
            // prose: the model can read JSON perfectly well.
            if (text.isEmpty() && result.contains(QStringLiteral("structuredContent"))) {
                text = QString::fromUtf8(QJsonDocument(
                    result.value(QStringLiteral("structuredContent")).toObject())
                        .toJson(QJsonDocument::Compact));
            }

            bool truncated = false;
            if (text.size() > MCP_RESULT_CAP) {
                text = text.left(MCP_RESULT_CAP);
                truncated = true;
            }

            // isError is the server saying the tool itself failed. Reported as
            // an error field so the backend's turn tracking counts it, with
            // the server's own words kept.
            if (result.value(QStringLiteral("isError")).toBool()) {
                done(mcpJsonError(QStringLiteral("%1/%2 failed: %3")
                                      .arg(serverName, toolName,
                                           text.isEmpty() ? QStringLiteral("no detail given")
                                                          : text)));
                return;
            }

            QJsonObject out{
                {QStringLiteral("ok"), true},
                {QStringLiteral("server"), serverName},
                {QStringLiteral("tool"), toolName},
                {QStringLiteral("result"), text.isEmpty() ? QStringLiteral("(no output)") : text}
            };
            if (truncated) {
                out[QStringLiteral("truncated")] = true;
                out[QStringLiteral("note")] = QStringLiteral(
                    "Output was longer than the cap and cut here. Narrow the "
                    "arguments if you need the rest.");
            }
            done(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        });
    });
}
