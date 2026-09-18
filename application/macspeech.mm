#include "macspeech.h"

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>
#import <cmath>

// Obj-C++ implementation of MacSpeech. Owns an AVAudioEngine tap that both
// feeds the recognizer and computes a per-buffer RMS level for the waveform.
// All Qt signal emission hops back via the captured MacSpeech* (Qt signals are
// thread-safe to emit; the tap runs on an audio thread).

namespace {
struct Impl {
    AVAudioEngine *engine = nil;
    SFSpeechRecognizer *recognizer = nil;
    SFSpeechAudioBufferRecognitionRequest *request = nil;
    SFSpeechRecognitionTask *task = nil;
};
}

MacSpeech::MacSpeech(QObject *parent) : QObject(parent)
{
    m_impl = new Impl();
}

MacSpeech::~MacSpeech()
{
    stop();
    delete static_cast<Impl *>(m_impl);
    m_impl = nullptr;
}

void MacSpeech::setListening(bool on)
{
    if (m_listening == on) { return; }
    m_listening = on;
    emit listeningChanged(on);
}

void MacSpeech::start()
{
    if (m_listening) { return; }
    Impl *d = static_cast<Impl *>(m_impl);

    // Ask for speech permission first, then run. Both prompts carry the app's
    // Info.plist usage strings.
    [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (status != SFSpeechRecognizerAuthorizationStatusAuthorized) {
                emit this->errorText(QStringLiteral(
                    "Speech recognition is off. Enable it in System Settings > "
                    "Privacy & Security > Speech Recognition."));
                return;
            }

            NSLocale *loc = [NSLocale currentLocale];
            d->recognizer = [[SFSpeechRecognizer alloc] initWithLocale:loc];
            if (!d->recognizer) {
                d->recognizer = [[SFSpeechRecognizer alloc] init];
            }
            if (!d->recognizer || !d->recognizer.isAvailable) {
                emit this->errorText(QStringLiteral(
                    "Speech recognition is not available right now."));
                return;
            }

            d->engine = [[AVAudioEngine alloc] init];
            d->request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
            d->request.shouldReportPartialResults = YES;

            AVAudioInputNode *input = d->engine.inputNode;
            AVAudioFormat *fmt = [input outputFormatForBus:0];
            __block MacSpeech *self_ = this;
            [input installTapOnBus:0 bufferSize:1024 format:fmt
                             block:^(AVAudioPCMBuffer *buffer, AVAudioTime *when) {
                (void)when;
                [d->request appendAudioPCMBuffer:buffer];
                // RMS -> lively 0..1 for the bars.
                float *ch = buffer.floatChannelData ? buffer.floatChannelData[0] : NULL;
                if (ch) {
                    const int n = (int)buffer.frameLength;
                    float sum = 0.0f;
                    for (int i = 0; i < n; ++i) { sum += ch[i] * ch[i]; }
                    float rms = n > 0 ? sqrtf(sum / (float)n) : 0.0f;
                    float scaled = rms * 14.0f;
                    if (scaled > 1.0f) scaled = 1.0f;
                    float shaped = powf(scaled, 0.6f);
                    emit self_->level(shaped);
                }
            }];

            [d->engine prepare];
            NSError *err = nil;
            if (![d->engine startAndReturnError:&err]) {
                emit this->errorText(QStringLiteral("Could not start the microphone."));
                [input removeTapOnBus:0];
                return;
            }

            this->setListening(true);

            d->task = [d->recognizer recognitionTaskWithRequest:d->request
                       resultHandler:^(SFSpeechRecognitionResult *result, NSError *error) {
                if (result) {
                    QString text = QString::fromNSString(
                        result.bestTranscription.formattedString);
                    if (result.isFinal) {
                        emit self_->finalText(text);
                        self_->stop();
                    } else {
                        emit self_->partial(text);
                    }
                }
                if (error) { self_->stop(); }
            }];
        });
    }];
}

void MacSpeech::stop()
{
    Impl *d = static_cast<Impl *>(m_impl);
    if (!d) { return; }
    if (d->engine) {
        [d->engine.inputNode removeTapOnBus:0];
        [d->engine stop];
    }
    if (d->request) { [d->request endAudio]; }
    if (d->task) { [d->task finish]; }
    d->task = nil;
    d->request = nil;
    d->engine = nil;
    d->recognizer = nil;
    setListening(false);
}
