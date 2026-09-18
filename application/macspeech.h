#pragma once

#include <QObject>
#include <QString>

// Native macOS speech-to-text + live microphone level, for Nikita's "send
// audio" button on the desktop. Implemented in macspeech.mm with AVAudioEngine
// and SFSpeechRecognizer (on-device where the Mac supports it) -- no local LLM,
// no network transcription, matching the iOS dictation path. On non-macOS this
// class exists but does nothing (the .mm is only compiled on macOS; a stub
// covers the rest), so callers never need a platform #ifdef.
class MacSpeech : public QObject
{
    Q_OBJECT
public:
    explicit MacSpeech(QObject *parent = nullptr);
    ~MacSpeech() override;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    bool listening() const { return m_listening; }

signals:
    // 0..1 microphone loudness, emitted per audio buffer, for the waveform.
    void level(float value);
    // Live partial transcript, then the settled final one.
    void partial(const QString &text);
    void finalText(const QString &text);
    void errorText(const QString &message);
    void listeningChanged(bool listening);

private:
    void setListening(bool on);
    bool m_listening = false;
    void *m_impl = nullptr;   // opaque Obj-C state on macOS, unused elsewhere
};
