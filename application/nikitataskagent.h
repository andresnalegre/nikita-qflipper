#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QDateTime>

class QNetworkReply;

// One independent, headless task agent -- a whole separate "worker" that runs
// its own turn loop against the Kimi API, with its own conversation history and
// its own tool execution, concurrently with the main chat and with other task
// agents. This is how Nikita runs more than one thing at once.
//
// Deliberately isolated from NikitaBackend: it does NOT share the backend's
// pending-operation members (which are single-slot and would corrupt under
// concurrency). It carries its own QNetworkAccessManager and runs the shell
// itself, so N of these can run at the same time without stepping on each other.
//
// Tools are the ones that genuinely parallelize -- the web and this computer.
// The Flipper is a single device whose link cannot be shared, so device work
// stays on the main agent; a task agent focused on research, files, documents
// and shell commands is exactly the part that benefits from running in parallel.
class NikitaTaskAgent : public QObject
{
    Q_OBJECT
public:
    enum class State { Running, Done, Failed, Stopped };

    NikitaTaskAgent(int id,
                    const QString &title,
                    const QString &task,
                    const QString &apiKey,
                    const QString &model,
                    const QString &braveKey,
                    const QString &nikitaName,
                    const QString &memory,
                    QObject *parent = nullptr);

    int id() const { return m_id; }
    QString title() const { return m_title; }
    QString task() const { return m_task; }
    State state() const { return m_state; }
    QString stateText() const;
    QString status() const { return m_status; }   // current step, human words
    QString result() const { return m_result; }   // final answer when Done
    int rounds() const { return m_rounds; }
    QDateTime startedAt() const { return m_startedAt; }

    void start();
    void stop();

signals:
    void changed(int id);            // status/state moved
    void finished(int id);           // reached Done/Failed/Stopped

private:
    void dispatch();                 // one model round
    void onReply(QNetworkReply *reply);
    void runToolCalls(const QJsonArray &calls, int index);
    void runOneTool(const QString &name, const QJsonObject &args,
                    std::function<void(const QString &)> done);
    void finish(State s, const QString &text);
    void setStatus(const QString &s);
    QJsonArray tools() const;
    QString systemPrompt() const;

    // Tool implementations -- self-contained, no shared backend state.
    void toolWebSearch(const QString &query, std::function<void(const QString &)> done);
    void toolWebFetch(const QString &url, std::function<void(const QString &)> done);
    void toolComputerRun(const QString &cmd, std::function<void(const QString &)> done);
    void toolPythonRun(const QString &code, std::function<void(const QString &)> done);
    void toolHttpRequest(const QJsonObject &args, std::function<void(const QString &)> done);
    void toolComputerWrite(const QString &path, const QString &content,
                           std::function<void(const QString &)> done);
    void toolComputerRead(const QString &path, std::function<void(const QString &)> done);
    void webSearchJson(const QString &query, std::function<void(const QString &)> done);

    const int m_id;
    QString m_title;
    QString m_task;
    QString m_apiKey;
    QString m_model;
    QString m_braveKey;
    QString m_nikitaName;   // she is still Nikita, even fragmented
    QString m_memory;       // the facts Nikita knows -- shared with the fragment

    QNetworkAccessManager m_net;
    QJsonArray m_history;            // this agent's own conversation
    State m_state = State::Running;
    QString m_status;
    QString m_result;
    int m_rounds = 0;
    QDateTime m_startedAt;
    bool m_stopping = false;

    static const int kMaxRounds = 40;
};
