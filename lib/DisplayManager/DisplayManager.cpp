#include "DisplayManager.h"

// 128×32 layout constants
// textSize(1): 6×8 px per char
// textSize(2): 12×16 px per char

bool DisplayManager::init() {
    if (!_display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
        return false;
    }
    _display.clearDisplay();
    _display.display();
    _available     = true;
    _pageChangedMs = 0;
    return true;
}

// ── Static screens ─────────────────────────────────────────────────────────

void DisplayManager::showBootScreen() {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 4);
    _display.println("Water Tracker");
    _display.setCursor(0, 18);
    _display.println("Booting...");
    _display.display();
    _lastUpdateMs = millis();
}

void DisplayManager::showError(const char* msg) {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 4);
    _display.println("Error:");
    _display.setCursor(0, 18);
    char buf[22];
    strncpy(buf, msg, 21);
    buf[21] = '\0';
    _display.println(buf);
    _display.display();
    _lastUpdateMs = millis();
}

void DisplayManager::showWifiConnecting(const String& ssid) {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 4);
    _display.println("Connecting WiFi...");
    _display.setCursor(0, 18);
    String s = ssid.length() > 21 ? ssid.substring(0, 18) + "..." : ssid;
    _display.println(s);
    _display.display();
    _lastUpdateMs = millis();
}

void DisplayManager::showAPMode(const String& apSsid, const String& apPassword, const String& ip) {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    String s = apSsid.length() > 21 ? apSsid.substring(0, 18) + "..." : apSsid;
    _display.setCursor(0, 0);
    _display.println(s);
    String pw = String("PW:") + (apPassword.length() > 17 ? apPassword.substring(0, 14) + "..." : apPassword);
    _display.setCursor(0, 11);
    _display.println(pw);
    _display.setCursor(0, 22);
    _display.println(String("Open: ") + ip);
    _display.display();
    _lastUpdateMs = millis();
}

// ── Normal Mode rotating pages ─────────────────────────────────────────────

void DisplayManager::showNormalMode(float weightG, bool stable,
                                    float todayMl, uint32_t goalMl, uint32_t drinkCount,
                                    float lastDrinkMl, uint32_t nextRemSec,
                                    bool wifiOk, const String& ip, bool ntpSynced) {
    (void)stable; (void)wifiOk; (void)ntpSynced;
    if (!_available) return;
    if (millis() - _lastUpdateMs < OLED_UPDATE_INTERVAL_MS) return;
    _lastUpdateMs = millis();

    if (_pageChangedMs == 0) {
        _page          = 0;
        _pageChangedMs = millis();
    } else if (millis() - _pageChangedMs >= PAGE_INTERVAL_MS) {
        _page = (_page + 1) % PAGE_COUNT;
        _pageChangedMs = millis();
    }

    _display.clearDisplay();
    _display.setTextColor(SSD1306_WHITE);

    switch (_page) {
        case 0: _drawPage0Weight(weightG, ip);                                              break;
        case 1: _drawPage1Hydration(todayMl, goalMl, drinkCount, lastDrinkMl, nextRemSec); break;
        default: break;
    }
    _display.display();
}

// ── Helper ─────────────────────────────────────────────────────────────────

void DisplayManager::_centerPrint(const char* text, int16_t y) {
    int16_t x1, y1;
    uint16_t w, h;
    _display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    _display.setCursor((OLED_SCREEN_WIDTH - (int16_t)w) / 2 - x1, y);
    _display.print(text);
}

// ── Page renderers ─────────────────────────────────────────────────────────

void DisplayManager::_drawPage0Weight(float weightG, const String& ip) {
    // Row 1 (y=2): IP address, textSize 1, centered
    _display.setTextSize(1);
    String ipStr = ip.length() > 0 ? ip : "--";
    _centerPrint(ipStr.c_str(), 2);

    // Row 2 (y=14): weight value, textSize 2, centered
    _display.setTextSize(2);
    char buf[12];
    snprintf(buf, sizeof(buf), "%.0f g", weightG);
    _centerPrint(buf, 14);
    _display.setTextSize(1);
}

void DisplayManager::_drawPage1Hydration(float todayMl, uint32_t goalMl, uint32_t drinkCount,
                                          float lastDrinkMl, uint32_t nextRemSec) {
    if (!isfinite(todayMl) || todayMl < 0.0f) todayMl = 0.0f;
    char buf[32];
    const uint32_t pct    = (goalMl > 0) ? (uint32_t)(todayMl * 100.0f / goalMl) : 0;
    const uint32_t barPct = pct > 100 ? 100 : pct;

    // Row 1 (y=0): today amount + percentage, centered
    snprintf(buf, sizeof(buf), "%.0f ml  %lu%%", todayMl, (unsigned long)barPct);
    _display.setTextSize(1);
    _centerPrint(buf, 0);

    // Progress bar (y=9, height=3)
    _display.drawRect(0, 9, 128, 3, SSD1306_WHITE);
    const int fillW = (int)(126 * barPct / 100);
    if (fillW > 0) _display.fillRect(1, 10, fillW, 1, SSD1306_WHITE);

    // Row 3 (y=14): goal + drink count, centered
    snprintf(buf, sizeof(buf), "Goal:%lu ml  Dr:%lu", (unsigned long)goalMl, (unsigned long)drinkCount);
    _centerPrint(buf, 14);

    // Row 4 (y=23): last drink + next reminder, centered
    if (nextRemSec == 0) {
        _centerPrint("Drink now!", 23);
    } else {
        char lastBuf[10];
        if (lastDrinkMl > 0.0f) {
            snprintf(lastBuf, sizeof(lastBuf), "%.0f ml", lastDrinkMl);
        } else {
            snprintf(lastBuf, sizeof(lastBuf), "--");
        }
        const uint32_t m = nextRemSec / 60;
        const uint32_t s = nextRemSec % 60;
        if (m > 0) {
            snprintf(buf, sizeof(buf), "L:%s  N:%lu min", lastBuf, (unsigned long)m);
        } else {
            snprintf(buf, sizeof(buf), "L:%s  N:%lu sec", lastBuf, (unsigned long)s);
        }
        _centerPrint(buf, 23);
    }
}

// ── Periodic re-flush (AP mode idle refresh) ───────────────────────────────

void DisplayManager::update() {
    if (!_available) return;
    if (millis() - _lastUpdateMs < OLED_UPDATE_INTERVAL_MS) return;
    _lastUpdateMs = millis();
    _display.display();
}
