#include "macspeech.h"

// Non-macOS builds: dictation is a macOS-native feature (Speech.framework), so
// here MacSpeech is a no-op. The UI simply never shows the mic as available;
// start() reports that plainly rather than pretending to listen.

MacSpeech::MacSpeech(QObject *parent) : QObject(parent) {}
MacSpeech::~MacSpeech() {}

void MacSpeech::start()
{
    emit errorText(QStringLiteral(
        "Voice input is only available on macOS in this build."));
}

void MacSpeech::stop() {}

void MacSpeech::setListening(bool on)
{
    if (m_listening == on) { return; }
    m_listening = on;
    emit listeningChanged(on);
}
