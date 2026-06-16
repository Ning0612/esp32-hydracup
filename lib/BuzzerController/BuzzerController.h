#pragma once
#include <Arduino.h>

enum class BeepPattern : uint8_t {
    BOOT_OK,
    AP_MODE,
    WIFI_CONNECTED,
    DRINK,
    REMINDER,
    ERROR_BEEP,
    CALIBRATION_OK
};

class BuzzerController {
public:
    bool init(uint32_t freqHz = 2000, uint8_t volumePct = 50);
    void play(BeepPattern pattern);
    void update();

    void setFrequency(uint32_t hz);
    void setVolume(uint8_t pct)    { _volumePct = constrain(pct, 0, 100); }
    void setDuration(uint32_t ms)  { _durationMs = ms; }
    void setEnabled(bool en);
    bool isPlaying() const         { return _queueIdx < _queueLen; }

private:
    struct BeepStep { uint32_t onMs; uint32_t offMs; };
    static const uint8_t MAX_BEEPS = 6;

    void _startBeep(uint32_t durationMs);
    void _stopBeep();

    uint32_t _freqHz     = 2000;
    uint8_t  _volumePct  = 50;
    uint32_t _durationMs = 150;
    bool     _enabled    = true;

    BeepStep _queue[MAX_BEEPS];
    uint8_t  _queueLen = 0;
    uint8_t  _queueIdx = 0;

    bool     _beeping      = false;
    bool     _inGap        = false;
    uint32_t _stepStartMs  = 0;
    uint32_t _stepDurMs    = 0;
};
