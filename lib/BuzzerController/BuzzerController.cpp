#include "BuzzerController.h"
#include "pins.h"
#include "config.h"

bool BuzzerController::init(uint32_t freqHz, uint8_t volumePct) {
    _freqHz    = freqHz;
    _volumePct = volumePct;
    ledcSetup(BUZZER_LEDC_CHANNEL, _freqHz, BUZZER_LEDC_RESOLUTION);
    ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CHANNEL);
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
    return true;
}

void BuzzerController::setFrequency(uint32_t hz) {
    _freqHz = hz;
    ledcSetup(BUZZER_LEDC_CHANNEL, _freqHz, BUZZER_LEDC_RESOLUTION);
}

void BuzzerController::setEnabled(bool en) {
    _enabled = en;
    if (!en) stop();
}

void BuzzerController::stop() {
    _stopBeep();
    _queueLen = 0;
    _queueIdx = 0;
    _inGap    = false;
}

void BuzzerController::play(BeepPattern pattern) {
    if (!_enabled) return;

    _queueIdx = 0;
    _queueLen = 0;
    _beeping  = false;
    _inGap    = false;

    const uint32_t on  = _durationMs;
    const uint32_t gap = 100;

    switch (pattern) {
        case BeepPattern::BOOT_OK:
        case BeepPattern::DRINK:
        case BeepPattern::CALIBRATION_OK:
            _queue[0] = { on, 0 };
            _queueLen = 1;
            break;

        case BeepPattern::AP_MODE:
            _queue[0] = { 600, 0 };
            _queueLen = 1;
            break;

        case BeepPattern::WIFI_CONNECTED:
            _queue[0] = { on, gap };
            _queue[1] = { on, 0 };
            _queueLen = 2;
            break;

        case BeepPattern::REMINDER:
            _queue[0] = { on, gap };
            _queue[1] = { on, 500 };
            _queue[2] = { on, gap };
            _queue[3] = { on, 0 };
            _queueLen = 4;
            break;

        case BeepPattern::ERROR_BEEP:
            _queue[0] = { on, gap };
            _queue[1] = { on, gap };
            _queue[2] = { on, 0 };
            _queueLen = 3;
            break;
    }
}

void BuzzerController::update() {
    if (_queueIdx >= _queueLen) return;

    const uint32_t elapsed = millis() - _stepStartMs;

    if (_beeping) {
        if (elapsed >= _stepDurMs) {
            _stopBeep();
            const uint32_t offMs = _queue[_queueIdx].offMs;
            _queueIdx++;
            if (offMs > 0 && _queueIdx < _queueLen) {
                _inGap       = true;
                _stepStartMs = millis();
                _stepDurMs   = offMs;
            }
        }
        return;
    }

    if (_inGap) {
        if (elapsed >= _stepDurMs) {
            _inGap = false;
        }
        return;
    }

    _startBeep(_queue[_queueIdx].onMs);
}

void BuzzerController::_startBeep(uint32_t durationMs) {
    const uint32_t duty = ((uint32_t)255 * _volumePct) / 100;
    ledcWrite(BUZZER_LEDC_CHANNEL, duty);
    _beeping     = true;
    _stepStartMs = millis();
    _stepDurMs   = durationMs;
}

void BuzzerController::_stopBeep() {
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
    _beeping = false;
}
