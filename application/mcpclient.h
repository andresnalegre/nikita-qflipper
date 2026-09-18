#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QMap>
#include <QList>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>
#include <QNetworkAccessManager>

class QProcess;
class QNetworkReply;
class QTimer;

// MCP (Model Context Protocol) client -- the same tool-plugin protocol Claude
// Code speaks, so Nikita can borrow any server the ecosystem already has
// instead of every capability being another hand-written tool in
// nikitabackend.cpp.
//
// Two transports, which is the whole protocol as far as a desktop app needs:
//
//   stdio  -- a child process, newline-delimited JSON-RPC 2.0 on its stdin and
//             stdout. This is what nearly every published server is.
//   http   -- Streamable HTTP: one POST per request, the reply either a JSON
//             body or an SSE stream carrying it. Session continuity comes from
//             the Mcp-Session-Id header the server hands back on initialize.
//
// Where the servers come from, in priority order (first name wins, so a
// Nikita-specific entry can override an inherited one):
//
//   1. ~/.nikita/mcp.json          -- Nikita's own, if the user writes one
//   2. ~/.claude.json  mcpServers  -- whatever Claude Code is configured with
//   3. <workspace>/.mcp.json       -- the project-scoped file Claude Code reads
//
// Inheriting from Claude Code's config is deliberate and is the point: a
// server the user already set up there works here with nothing to configure.
//
// Tool names are namespaced exactly the way Claude Code namespaces them --
// mcp__<server>__<tool> -- so a name in the log, in a proven move, or in an
// older conversation means the same thing in both programs.
struct McpServerDef {
    QString name;
    QString kind;                    // "stdio" | "http"
    QString command;                 // stdio
    QStringList args;
    QMap<QString, QString> env;
    QString url;                     // http
    QMap<QString, QString> headers;
    // The shared secret for the signed device claim. Read from the config's
    // "token" field; falls back to the Authorization header's value, since a
    // server configured with a bearer token already has exactly one secret.
    QString token;
    QString source;                  // which config file it came from
};

class McpClient : public QObject
{
    Q_OBJECT

    // Everything QML needs to show the panel. No secrets here: a header value
    // can carry a bearer token, so headers are never exposed -- only names,
    // states and tool counts.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(int toolCount READ toolCount NOTIFY changed)
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY changed)
    Q_PROPERTY(QVariantList servers READ serverList NOTIFY changed)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)

public:
    explicit McpClient(QObject *parent = nullptr);
    ~McpClient() override;

    // mcp__<server>__<tool>. Kept as one place so the backend never has to
    // spell the separator itself.
    static QString prefix();
    static bool isMcpTool(const QString &name);

    bool enabled() const;
    void setEnabled(bool on);

    int toolCount() const;
    QString statusLine() const;
    QVariantList serverList() const;
    QString configPath() const;
    QStringList serverNames() const;

    // Re-read the config files and (re)connect. Safe at any time: servers that
    // are already up and unchanged are left running.
    Q_INVOKABLE void reload();

    // Stop every child process. Called from the destructor and when the user
    // switches MCP off, so a server is not left running after Nikita is done
    // with it.
    Q_INVOKABLE void shutdown();

    // The workspace root, for the project-scoped .mcp.json. Set by the backend
    // from its agent directory; changing it re-reads the config.
    void setWorkspace(const QString &dir);

    // ---- Which Flipper is asking, and proving it ---------------------------
    //
    // Every call carries the identity of the device this client is attached to,
    // so a server can keep per-Flipper state instead of treating one user as
    // one undifferentiated blob. Two separate values travel, and they are not
    // the same kind of thing at all -- conflating them is the mistake this
    // comment exists to prevent.
    //
    //   deviceId -- a stable PSEUDONYM: sha256 of the device's own id, first
    //      32 hex. An IDENTIFIER, never a secret. A Flipper's serial is not
    //      secret (it is printed on the device and discoverable over USB), so
    //      anything derived from it alone is forgeable by anyone who has seen
    //      the device. A server must use this only for scoping. The raw serial
    //      is deliberately never sent: a hardware serial handed to every
    //      third-party server is a fingerprint the user never agreed to.
    //
    //   deviceAuth -- a SIGNED claim, and the part that actually proves
    //      something. Shape:
    //
    //          v1:<deviceId>:<unix-seconds>:<nonce>:<hmac>
    //          hmac = HMAC-SHA256(token, "v1|deviceId|ts|nonce|method|tool")
    //
    //      where `token` is the secret configured for THAT server (the config's
    //      "token" field, or its Authorization header value). Four properties
    //      come out of that construction, each from one specific piece:
    //
    //        * it needs the secret, so only a client configured for this
    //          server can produce it -- the device id alone is not enough;
    //        * the nonce and timestamp make a captured value single-use: a
    //          server rejects a stale timestamp and a nonce it has already
    //          seen, so replaying a recording buys nothing;
    //        * the method and tool name are inside the signature, so a
    //          signature lifted from a harmless call cannot be re-attached to a
    //          dangerous one;
    //        * it is per-server by construction -- the same Flipper presents a
    //          different value to each server, so server A cannot replay what
    //          it received at server B.
    //
    //      What it does NOT do, stated plainly: it does not make the channel
    //      confidential (that is TLS's job, and the bearer token already rides
    //      in a header), and it does not bind the ARGUMENTS of the call. It is
    //      device authentication, not a full request signature.
    //
    // For a stdio child none of this is a security boundary -- the process runs
    // locally, under this user, and was started by us -- so it gets the
    // pseudonym for scoping and no signature. Only the identifier goes into its
    // environment; a secret in a child's environment is a secret in `ps`.
    //
    // The values reach a server three ways, because the transports have
    // different shapes: HTTP headers, environment variables for a stdio child,
    // and -- the transport-neutral one -- inside the `_meta` of every
    // tools/call. That last one is also the only one that tracks the device
    // being unplugged and replaced while a long-lived server keeps running.
    void setDeviceIdentity(const QString &rawDeviceId);
    QString deviceId() const;       // the pseudonym, or empty with no device

    // Every ready server's tools, in OpenAI function-calling shape, ready to
    // append to the array the model is offered. `cap` bounds how many travel:
    // a server with two hundred tools would otherwise crowd out Nikita's own
    // and cost more in the prompt than it can possibly return.
    QJsonArray toolSchemas(int cap = 48) const;

    // Call one tool. `done` gets a JSON string in the same shape the rest of
    // the backend's tools return -- {"error":...} on failure, so the turn's
    // error tracking sees it without special-casing, and an ok payload
    // otherwise. Connects the server first if it is not up yet.
    void callTool(const QString &fullName, const QJsonObject &args,
                  std::function<void(const QString &)> done);

signals:
    void changed();
    void logLine(const QString &text);

private:
    struct Pending {
        std::function<void(const QJsonObject &result, const QString &error)> cb;
        QTimer *timer = nullptr;
    };

    struct Server {
        McpServerDef def;
        QProcess *proc = nullptr;
        QByteArray buf;                       // partial stdout line
        QString sessionId;                    // Mcp-Session-Id, http only
        int nextId = 1;
        QHash<int, Pending> pending;
        QJsonArray tools;                     // namespaced, OpenAI shape
        QString state = QStringLiteral("idle");   // idle|starting|ready|failed
        QString lastError;
        QString serverLabel;                  // name+version the server reports
        int failures = 0;
        QList<std::function<void(bool ok, const QString &error)>> waiters;
    };

    QList<McpServerDef> readConfig() const;
    static void parseServerMap(const QJsonObject &map, const QString &source,
                               QList<McpServerDef> *out);
    static QString expandVars(const QString &in);
    // The signed claim for one server, or empty when it has no secret to sign
    // with. `method` and `tool` go into the signature, so the caller passes
    // what it is about to send.
    QString deviceAuthFor(const Server *s, const QString &method,
                          const QString &tool) const;
    // The transport-neutral carrier: what goes in a tools/call's `_meta`.
    QJsonObject callMeta(const Server *s, const QString &tool) const;
    static QString serverSecret(const McpServerDef &def);

    Server *server(const QString &name);
    void ensureReady(const QString &name,
                     std::function<void(bool ok, const QString &error)> done);
    void startStdio(Server *s);
    void handshake(Server *s);
    void listTools(Server *s, const QString &cursor);
    void request(Server *s, const QString &method, const QJsonObject &params,
                 int timeoutMs,
                 std::function<void(const QJsonObject &, const QString &)> cb);
    void notify(Server *s, const QString &method, const QJsonObject &params);
    void sendStdio(Server *s, const QJsonObject &message);
    void sendHttp(Server *s, const QJsonObject &message, bool expectsReply);
    void onStdioReadable(Server *s);
    void deliver(Server *s, const QJsonObject &message);
    void fail(Server *s, const QString &why);
    void resolveWaiters(Server *s, bool ok, const QString &error);
    void log(const QString &text) const;

    bool m_enabled = true;
    QString m_workspace;
    QString m_rawDeviceId;   // the device's own id, never sent anywhere
    QString m_deviceId;      // sha256(m_rawDeviceId), first 32 hex
    QList<Server *> m_servers;
    QNetworkAccessManager m_net;
    QHash<QNetworkReply *, QString> m_replyServer;
};
