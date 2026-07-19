#include "BuzzerController.h"

#include "config.h"
#include "pins.h"
#include "hal_log.h"
#include "hal_time.h"
#include "driver/ledc.h"

namespace {
constexpr const char* TAG = "Buzzer";
constexpr ledc_mode_t SPEED_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t TIMER = LEDC_TIMER_0;
constexpr ledc_channel_t CHANNEL = static_cast<ledc_channel_t>(BUZZER_LEDC_CHANNEL);
}

bool BuzzerController::init(uint32_t freqHz, uint8_t volumePct) {
    _freqHz = freqHz;
    _volumePct = volumePct > 100 ? 100 : volumePct;
    ledc_timer_config_t timer = {};
    timer.speed_mode = SPEED_MODE;
    timer.timer_num = TIMER;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.freq_hz = static_cast<int>(freqHz);
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) return false;

    ledc_channel_config_t channel = {};
    channel.gpio_num = PIN_BUZZER;
    channel.speed_mode = SPEED_MODE;
    channel.channel = CHANNEL;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = TIMER;
    channel.duty = 0;
    channel.hpoint = 0;
    if (ledc_channel_config(&channel) != ESP_OK) return false;
    _initialized = true;
    return true;
}

void BuzzerController::setFrequency(uint32_t hz) {
    _freqHz = hz == 0 ? 1 : hz;
    if (_initialized) ledc_set_freq(SPEED_MODE, TIMER, _freqHz);
}

void BuzzerController::setEnabled(bool en) {
    _enabled = en;
    if (!en) stop();
}

void BuzzerController::stop() {
    _stopBeep();
    _queueLen = 0;
    _queueIdx = 0;
    _inGap = false;
}

void BuzzerController::play(BeepPattern pattern) {
    if (!_enabled || !_initialized) return;
    _queueIdx = 0;
    _queueLen = 0;
    _beeping = false;
    _inGap = false;
    const uint32_t on = _durationMs;
    const uint32_t gap = 100;
    switch (pattern) {
        case BeepPattern::BOOT_OK:
        case BeepPattern::DRINK:
        case BeepPattern::CALIBRATION_OK:
            _queue[0] = {on, 0}; _queueLen = 1; break;
        case BeepPattern::AP_MODE:
            _queue[0] = {600, 0}; _queueLen = 1; break;
        case BeepPattern::WIFI_CONNECTED:
            _queue[0] = {on, gap}; _queue[1] = {on, 0}; _queueLen = 2; break;
        case BeepPattern::REMINDER:
            _queue[0] = {on, gap}; _queue[1] = {on, 500};
            _queue[2] = {on, gap}; _queue[3] = {on, 0}; _queueLen = 4; break;
        case BeepPattern::ERROR_BEEP:
            _queue[0] = {on, gap}; _queue[1] = {on, gap};
            _queue[2] = {on, 0}; _queueLen = 3; break;
    }
}

void BuzzerController::update() {
    if (_queueIdx >= _queueLen) return;
    const uint32_t now = hal_millis();
    const uint32_t elapsed = now - _stepStartMs;
    if (_beeping) {
        if (elapsed >= _stepDurMs) {
            _stopBeep();
            const uint32_t offMs = _queue[_queueIdx].offMs;
            _queueIdx++;
            if (offMs > 0 && _queueIdx < _queueLen) {
                _inGap = true; _stepStartMs = now; _stepDurMs = offMs;
            }
        }
        return;
    }
    if (_inGap) {
        if (elapsed >= _stepDurMs) _inGap = false;
        return;
    }
    _startBeep(_queue[_queueIdx].onMs);
}

void BuzzerController::_startBeep(uint32_t durationMs) {
    const uint32_t duty = (255U * _volumePct) / 100U;
    ledc_set_duty(SPEED_MODE, CHANNEL, duty);
    ledc_update_duty(SPEED_MODE, CHANNEL);
    _beeping = true;
    _stepStartMs = hal_millis();
    _stepDurMs = durationMs;
}

void BuzzerController::_stopBeep() {
    if (_initialized) {
        ledc_set_duty(SPEED_MODE, CHANNEL, 0);
        ledc_update_duty(SPEED_MODE, CHANNEL);
    }
    _beeping = false;
}
