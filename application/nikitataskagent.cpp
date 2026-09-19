#include "nikitataskagent.h"

#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QFile>
#include <QFileInfo>

static const char *TA_API_URL = "https://api.moonshot.ai/v1/chat/completions";

NikitaTaskAgent::NikitaTaskAgent(int id, const QString &title, const QString &task,
                                 const QString &apiKey, const QString &model,
                                 const QString &braveKey, const QString &nikitaName,
                                 const QString &memory, QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_title(title)
    , m_task(task)
    , m_apiKey(apiKey)
    , m_model(model)
    , m_braveKey(braveKey)
    , m_nikitaName(nikitaName)
    , m_memory(memory)
{
    m_startedAt = QDateTime::currentDateTime();
}

QString NikitaTaskAgent::stateText() const
{
    switch (m_state) {
    case State::Running: return QStringLiteral("running");
    case State::Done:    return QStringLiteral("done");
    case State::Failed:  return QStringLiteral("failed");
    case State::Stopped: return QStringLiteral("stopped");
    }
    return QString();
}

void NikitaTaskAgent::setStatus(const QString &s)
{
    m_status = s;
    emit changed(m_id);
}

void NikitaTaskAgent::start()
{
    if (m_apiKey.isEmpty()) { finish(State::Failed, QStringLiteral("No API key.")); return; }
    m_history.append(QJsonObject{{"role", "system"}, {"content", systemPrompt()}});
    m_history.append(QJsonObject{{"role", "user"}, {"content", m_task}});
    setStatus(QStringLiteral("thinking"));
    dispatch();
}

void NikitaTaskAgent::stop()
{
    if (m_state != State::Running) { return; }
    m_stopping = true;
    finish(State::Stopped, QStringLiteral("Stopped by the user."));
}

QString NikitaTaskAgent::systemPrompt() const
{
    // A fragment of Nikita -- same identity, running one task in parallel.
    QString base = QStringLiteral(
        "You ARE %1 -- the SAME Nikita, not a separate assistant. This is a FRAGMENT of you, "
        "split off to run ONE task in parallel while the rest of you keeps working. Same "
        "identity, same voice, always first person; never forget who you are. You are an "
        "autonomous worker on this computer, with a real shell and the web. Do the whole task "
        "end to end, then report a COMPLETE, well-organized answer -- all the relevant facts, "
        "no filler, no restating the task. Tools: web_search (always use this to search, never "
        "curl a search engine -- they bot-wall), web_fetch (read a page), computer_run (any "
        "shell command here), computer_read / computer_write (files). You have "
        "$HOME/.nikita/venv/bin/python with openpyxl, python-docx, reportlab, matplotlib, "
        "pandas, Pillow, pptx, pdfplumber, pypdf, qrcode; pandoc; ffmpeg; headless Chrome for "
        "HTML->PDF. Install anything missing yourself (pip into that venv, or brew) -- never "
        "ask the user to. Save any deliverable to $HOME/Desktop and report its path. Keep going "
        "until the task is genuinely finished; never stop halfway or just describe what you "
        "would do -- do it. When done, answer in plain text with the result."
        ).arg(m_nikitaName.isEmpty() ? QStringLiteral("Nikita") : m_nikitaName);
    if (!m_memory.trimmed().isEmpty()) {
        base += QStringLiteral("\n\nWhat you (Nikita) remember about the user:\n")
              + m_memory.trimmed();
    }
    return base;
}

QJsonArray NikitaTaskAgent::tools() const
{
    auto fn = [](const QString &name, const QString &desc, const QJsonObject &props,
                 const QJsonArray &req) {
        return QJsonObject{{"type", "function"}, {"function", QJsonObject{
            {"name", name}, {"description", desc},
            {"parameters", QJsonObject{{"type", "object"}, {"properties", props},
                                       {"required", req}}}}}};
    };
    auto str = [](const QString &d) { return QJsonObject{{"type", "string"}, {"description", d}}; };
    return QJsonArray{
        fn("web_search", "Search the web; returns top results (title, url, snippet).",
           QJsonObject{{"query", str("what to search for")}}, QJsonArray{"query"}),
        fn("web_fetch", "Fetch a URL and return its readable text.",
           QJsonObject{{"url", str("the full URL")}}, QJsonArray{"url"}),
        fn("computer_run", "Run a shell command on this computer and return its output.",
           QJsonObject{{"command", str("the shell command")}}, QJsonArray{"command"}),
        fn("python_run", "Run Python 3 in Nikita's env (matplotlib/pandas/Pillow/"
           "cairosvg/pypdf/qrcode...). Best for charts, images, data, PDF, binaries.",
           QJsonObject{{"code", str("the Python 3 source")}}, QJsonArray{"code"}),
        fn("http_request", "HTTP request to any URL (method/headers/body); returns status+body. "
           "A real API client, not just page reading.",
           QJsonObject{{"url", str("full http/https URL")},
                       {"method", str("GET/POST/PUT/PATCH/DELETE, default GET")},
                       {"body", str("optional request body")}}, QJsonArray{"url"}),
        fn("computer_write", "Write a text file on this computer (creates parent folders).",
           QJsonObject{{"path", str("absolute path")}, {"content", str("full contents")}},
           QJsonArray{"path", "content"}),
        fn("computer_read", "Read a text file on this computer.",
           QJsonObject{{"path", str("absolute path")}}, QJsonArray{"path"})
    };
}

void NikitaTaskAgent::dispatch()
{
    if (m_stopping) { return; }
    if (++m_rounds > kMaxRounds) {
        finish(State::Failed, QStringLiteral("Hit the round limit before finishing."));
        return;
    }

    QJsonObject body{
        {"model", m_model},
        {"messages", m_history},
        {"tools", tools()},
        {"stream", false},
        {"max_tokens", 4096}
    };
    QNetworkRequest req{QUrl(QString::fromUtf8(TA_API_URL))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + m_apiKey.toUtf8());
    req.setTransferTimeout(300000);

    QNetworkReply *reply = m_net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onReply(reply); });
}

void NikitaTaskAgent::onReply(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_stopping) { return; }
    if (reply->error() != QNetworkReply::NoError) {
        // One retry-ish: surface the error as the result rather than hanging.
        finish(State::Failed, QStringLiteral("API error: %1").arg(reply->errorString()));
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) { finish(State::Failed, QStringLiteral("Empty API reply.")); return; }
    const QJsonObject msg = choices.first().toObject().value(QStringLiteral("message")).toObject();
    const QString content = msg.value(QStringLiteral("content")).toString();
    const QJsonArray toolCalls = msg.value(QStringLiteral("tool_calls")).toArray();

    // Record the assistant message verbatim (with tool_calls) for the wire.
    m_history.append(msg);

    if (toolCalls.isEmpty()) {
        finish(State::Done, content.trimmed().isEmpty() ? QStringLiteral("(done)") : content.trimmed());
        return;
    }
    runToolCalls(toolCalls, 0);
}

void NikitaTaskAgent::runToolCalls(const QJsonArray &calls, int index)
{
    if (m_stopping) { return; }
    if (index >= calls.size()) { dispatch(); return; }

    const QJsonObject call = calls.at(index).toObject();
    const QString id = call.value(QStringLiteral("id")).toString();
    const QJsonObject fn = call.value(QStringLiteral("function")).toObject();
    const QString name = fn.value(QStringLiteral("name")).toString();
    QJsonObject args = QJsonDocument::fromJson(
        fn.value(QStringLiteral("arguments")).toString().toUtf8()).object();

    setStatus(name);
    runOneTool(name, args, [this, calls, index, id](const QString &result) {
        m_history.append(QJsonObject{
            {"role", "tool"}, {"tool_call_id", id}, {"content", result}});
        runToolCalls(calls, index + 1);
    });
}

void NikitaTaskAgent::runOneTool(const QString &name, const QJsonObject &args,
                                 std::function<void(const QString &)> done)
{
    if (name == QLatin1String("web_search")) {
        toolWebSearch(args.value(QStringLiteral("query")).toString(), done);
    } else if (name == QLatin1String("web_fetch")) {
        toolWebFetch(args.value(QStringLiteral("url")).toString(), done);
    } else if (name == QLatin1String("computer_run")) {
        toolComputerRun(args.value(QStringLiteral("command")).toString(), done);
    } else if (name == QLatin1String("python_run")) {
        toolPythonRun(args.value(QStringLiteral("code")).toString(), done);
    } else if (name == QLatin1String("http_request")) {
        toolHttpRequest(args, done);
    } else if (name == QLatin1String("computer_write")) {
        toolComputerWrite(args.value(QStringLiteral("path")).toString(),
                          args.value(QStringLiteral("content")).toString(), done);
    } else if (name == QLatin1String("computer_read")) {
        toolComputerRead(args.value(QStringLiteral("path")).toString(), done);
    } else {
        done(QStringLiteral("{\"error\":\"unknown tool %1\"}").arg(name));
    }
}

void NikitaTaskAgent::finish(State s, const QString &text)
{
    if (m_state != State::Running) { return; }
    m_state = s;
    if (s == State::Done) { m_result = text; }
    else if (m_result.isEmpty()) { m_result = text; }
    setStatus(stateText());
    emit finished(m_id);
}

// ---- Tools (self-contained) ----------------------------------------------

void NikitaTaskAgent::toolComputerRun(const QString &cmd,
                                      std::function<void(const QString &)> done)
{
    QProcess *p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [p, done](int code, QProcess::ExitStatus) {
        QString out = QString::fromUtf8(p->readAll());
        if (out.size() > 60000) { out = out.left(60000) + QStringLiteral("\n...(truncated)"); }
        p->deleteLater();
        QJsonObject o{{"exit_code", code}, {"output", out}};
        done(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    });
    connect(p, &QProcess::errorOccurred, this, [p, done](QProcess::ProcessError) {
        if (p->state() == QProcess::NotRunning) {
            done(QStringLiteral("{\"error\":\"%1\"}").arg(p->errorString()));
            p->deleteLater();
        }
    });
    p->start(QStringLiteral("/bin/zsh"), {QStringLiteral("-lc"), cmd});
}

void NikitaTaskAgent::toolPythonRun(const QString &code,
                                    std::function<void(const QString &)> done)
{
    const QString b64 = QString::fromLatin1(code.toUtf8().toBase64());
    const QString cmd = QStringLiteral(
        "PY=\"$HOME/.nikita/venv/bin/python3\"; [ -x \"$PY\" ] || "
        "PY=\"$HOME/.nikita/venv/bin/python\"; [ -x \"$PY\" ] || PY=python3; "
        "echo %1 | base64 -d | \"$PY\" -").arg(b64);
    toolComputerRun(cmd, done);
}

void NikitaTaskAgent::toolHttpRequest(const QJsonObject &args,
                                      std::function<void(const QString &)> done)
{
    const QUrl url(args.value(QStringLiteral("url")).toString().trimmed());
    if (!url.isValid() || !(url.scheme() == QLatin1String("http")
                            || url.scheme() == QLatin1String("https"))) {
        done(QStringLiteral("{\"error\":\"url must be http/https\"}"));
        return;
    }
    const QString method = args.value(QStringLiteral("method")).toString(QStringLiteral("GET")).toUpper();
    const QByteArray body = args.value(QStringLiteral("body")).toString().toUtf8();
    QNetworkRequest req{url};
    req.setRawHeader("User-Agent", "nikita");
    if (!body.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(30000);
    QNetworkReply *reply = method == QLatin1String("POST") ? m_net.post(req, body)
        : method == QLatin1String("PUT") ? m_net.put(req, body)
        : method == QLatin1String("DELETE") ? m_net.deleteResource(req)
        : method == QLatin1String("PATCH") ? m_net.sendCustomRequest(req, QByteArrayLiteral("PATCH"), body)
        : m_net.get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString text = QString::fromUtf8(reply->readAll()).left(12000);
        QJsonObject o{{"status", status}, {"body", text}};
        done(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    });
}

void NikitaTaskAgent::toolComputerWrite(const QString &path, const QString &content,
                                        std::function<void(const QString &)> done)
{
    QString abs = path;
    if (abs.startsWith(QLatin1String("~/"))) {
        abs = QDir::homePath() + abs.mid(1);
    }
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        done(QStringLiteral("{\"error\":\"can't write: %1\"}").arg(f.errorString()));
        return;
    }
    f.write(content.toUtf8());
    f.close();
    done(QStringLiteral("{\"wrote\":\"%1\",\"bytes\":%2}")
             .arg(abs).arg(content.toUtf8().size()));
}

void NikitaTaskAgent::toolComputerRead(const QString &path,
                                       std::function<void(const QString &)> done)
{
    QString abs = path;
    if (abs.startsWith(QLatin1String("~/"))) { abs = QDir::homePath() + abs.mid(1); }
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) {
        done(QStringLiteral("{\"error\":\"can't read: %1\"}").arg(f.errorString()));
        return;
    }
    QByteArray d = f.read(120000);
    f.close();
    QJsonObject o{{"path", abs}, {"content", QString::fromUtf8(d)}};
    done(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

static QString taHtmlToText(QString html)
{
    html.remove(QRegularExpression(QStringLiteral("<script\\b[^>]*>.*?</script>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    html.remove(QRegularExpression(QStringLiteral("<style\\b[^>]*>.*?</style>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    html.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    html.replace(QLatin1String("&amp;"), QLatin1String("&"));
    html.replace(QLatin1String("&lt;"), QLatin1String("<"));
    html.replace(QLatin1String("&gt;"), QLatin1String(">"));
    html.replace(QLatin1String("&quot;"), QLatin1String("\""));
    html.replace(QLatin1String("&#39;"), QLatin1String("'"));
    html.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    html.replace(QRegularExpression(QStringLiteral("[ \\t]+")), QStringLiteral(" "));
    html.replace(QRegularExpression(QStringLiteral("\\n\\s*\\n\\s*\\n+")), QStringLiteral("\n\n"));
    return html.trimmed();
}

void NikitaTaskAgent::toolWebSearch(const QString &query,
                                    std::function<void(const QString &)> done)
{
    if (query.trimmed().isEmpty()) { done(QStringLiteral("{\"error\":\"no query\"}")); return; }
    // Brave first when a key is set.
    if (!m_braveKey.isEmpty()) {
        QUrl u(QStringLiteral("https://api.search.brave.com/res/v1/web/search"));
        QUrlQuery q; q.addQueryItem(QStringLiteral("q"), query);
        q.addQueryItem(QStringLiteral("count"), QStringLiteral("8"));
        u.setQuery(q);
        QNetworkRequest r(u);
        r.setRawHeader("Accept", "application/json");
        r.setRawHeader("X-Subscription-Token", m_braveKey.toUtf8());
        r.setTransferTimeout(20000);
        QNetworkReply *br = m_net.get(r);
        connect(br, &QNetworkReply::finished, this, [this, br, query, done]() {
            br->deleteLater();
            if (br->error() == QNetworkReply::NoError) {
                const QJsonArray res = QJsonDocument::fromJson(br->readAll()).object()
                    .value(QStringLiteral("web")).toObject()
                    .value(QStringLiteral("results")).toArray();
                QJsonArray results;
                for (const QJsonValue &v : res) {
                    const QJsonObject o = v.toObject();
                    results.append(QJsonObject{
                        {"title", o.value(QStringLiteral("title")).toString().left(200)},
                        {"url", o.value(QStringLiteral("url")).toString()},
                        {"snippet", o.value(QStringLiteral("description")).toString().left(300)}});
                    if (results.size() >= 8) { break; }
                }
                if (!results.isEmpty()) {
                    done(QString::fromUtf8(QJsonDocument(QJsonObject{
                        {"query", query}, {"results", results}, {"source", "brave"}})
                        .toJson(QJsonDocument::Compact)));
                    return;
                }
            }
            webSearchJson(query, done);
        });
        return;
    }
    webSearchJson(query, done);
}

void NikitaTaskAgent::webSearchJson(const QString &query,
                                    std::function<void(const QString &)> done)
{
    QUrl u(QStringLiteral("https://api.duckduckgo.com/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("no_html"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("no_redirect"), QStringLiteral("1"));
    u.setQuery(q);
    QNetworkRequest r(u);
    r.setRawHeader("User-Agent", "nikita-task-agent");
    r.setTransferTimeout(20000);
    QNetworkReply *reply = m_net.get(r);
    connect(reply, &QNetworkReply::finished, this, [reply, query, done]() {
        reply->deleteLater();
        QJsonArray results;
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
            const QString abs = o.value(QStringLiteral("AbstractText")).toString();
            if (!abs.isEmpty()) {
                results.append(QJsonObject{
                    {"title", o.value(QStringLiteral("Heading")).toString()},
                    {"url", o.value(QStringLiteral("AbstractURL")).toString()},
                    {"snippet", abs.left(500)}});
            }
            for (const QJsonValue &v : o.value(QStringLiteral("RelatedTopics")).toArray()) {
                if (results.size() >= 8) { break; }
                const QJsonObject t = v.toObject();
                const QString text = t.value(QStringLiteral("Text")).toString();
                if (text.isEmpty()) { continue; }
                results.append(QJsonObject{
                    {"title", text.left(80)},
                    {"url", t.value(QStringLiteral("FirstURL")).toString()},
                    {"snippet", text.left(300)}});
            }
        }
        QJsonObject out{{"query", query}, {"results", results}};
        if (results.isEmpty()) {
            out[QStringLiteral("note")] = QStringLiteral(
                "Search endpoints are bot-walled for scripted requests and the keyless answer "
                "API had nothing. Set a Brave key for real ranked results, or open the query in "
                "the browser for the user.");
        }
        done(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    });
}

void NikitaTaskAgent::toolWebFetch(const QString &url,
                                   std::function<void(const QString &)> done)
{
    QUrl u = QUrl::fromUserInput(url.trimmed());
    if (!u.isValid() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https"))) {
        done(QStringLiteral("{\"error\":\"bad url\"}"));
        return;
    }
    QNetworkRequest r(u);
    r.setRawHeader("User-Agent",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
        "(KHTML, like Gecko) Version/17.0 Safari/605.1.15");
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::NoLessSafeRedirectPolicy);
    r.setTransferTimeout(20000);
    QNetworkReply *reply = m_net.get(r);
    connect(reply, &QNetworkReply::finished, this, [reply, u, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(QStringLiteral("{\"error\":\"fetch failed: %1\"}").arg(reply->errorString()));
            return;
        }
        QString text = taHtmlToText(QString::fromUtf8(reply->read(2 * 1024 * 1024)));
        if (text.size() > 12000) { text = text.left(12000); }
        done(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"url", u.toString()}, {"content", text.isEmpty() ? QStringLiteral("(no text)") : text}})
            .toJson(QJsonDocument::Compact)));
    });
}
