#include "DisplayManager.h"

// 128×32 layout constants
// textSize(1): 6×8 px per char → 4 lines per screen
// textSize(2): 12×16 px per char → used only for large weight value
// Content area: y=0..27; page indicator dots: y=29..30

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
    // Truncate to 21 chars (128px / 6px per char)
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
    // Line 1: SSID
    String s = apSsid.length() > 21 ? apSsid.substring(0, 18) + "..." : apSsid;
    _display.setCursor(0, 0);
    _display.println(s);
    // Line 2: Password
    String pw = String("PW:") + (apPassword.length() > 17 ? apPassword.substring(0, 14) + "..." : apPassword);
    _display.setCursor(0, 11);
    _display.println(pw);
    // Line 3: IP with open hint
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
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);

    switch (_page) {
        case 0: _drawPage0Weight(weightG, stable);                             break;
        case 1: _drawPage1Hydration(todayMl, goalMl, drinkCount);             break;
        case 2: _drawPage2Reminder(lastDrinkMl, nextRemSec);                  break;
        case 3: _drawPage3System(wifiOk, ip, ntpSynced);                      break;
        default: break;
    }
    _drawPageIndicator(_page);
    _display.display();
}

// ── Page renderers ─────────────────────────────────────────────────────────

void DisplayManager::_drawPage0Weight(float weightG, bool stable) {
    // Line 1 (y=0): label + stability
    _display.setCursor(0, 0);
    _display.print("Weight  ");
    _display.println(stable ? "Stable" : "Moving");
    // Line 2 (y=9): large weight value, textSize 2 (12×16 px)
    _display.setTextSize(2);
    char buf[12];
    snprintf(buf, sizeof(buf), "%.0f g", weightG);
    _display.setCursor(0, 9);
    _display.println(buf);
    _display.setTextSize(1);
}

void DisplayManager::_drawPage1Hydration(float todayMl, uint32_t goalMl, uint32_t drinkCount) {
    char buf[32];
    const uint32_t pct    = (goalMl > 0) ? (uint32_t)(todayMl * 100.0f / goalMl) : 0;
    const uint32_t barPct = pct > 100 ? 100 : pct;

    // Line 1 (y=0): today amount + percentage
    snprintf(buf, sizeof(buf), "%.0f ml  %lu%%", todayMl, (unsigned long)barPct);
    _display.setCursor(0, 0);
    _display.println(buf);

    // Progress bar (y=9, height=3)
    _display.drawRect(0, 9, 128, 3, SSD1306_WHITE);
    const int fillW = (int)(126 * barPct / 100);
    if (fillW > 0) _display.fillRect(1, 10, fillW, 1, SSD1306_WHITE);

    // Line 2 (y=14): goal
    snprintf(buf, sizeof(buf), "Goal: %lu ml", (unsigned long)goalMl);
    _display.setCursor(0, 14);
    _display.println(buf);

    // Line 3 (y=22): drink count — short format to keep clear of indicator at y=30
    snprintf(buf, sizeof(buf), "Drinks: %lu", (unsigned long)drinkCount);
    _display.setCursor(0, 22);
    _display.print(buf);
}

void DisplayManager::_drawPage2Reminder(float lastDrinkMl, uint32_t nextRemSec) {
    char buf[32];

    // Line 1 (y=0): last drink
    if (lastDrinkMl > 0.0f) {
        snprintf(buf, sizeof(buf), "Last: %.0f ml", lastDrinkMl);
    } else {
        snprintf(buf, sizeof(buf), "Last: --");
    }
    _display.setCursor(0, 0);
    _display.println(buf);

    // Line 2 (y=12): next reminder countdown
    if (nextRemSec > 0) {
        const uint32_t m = nextRemSec / 60;
        const uint32_t s = nextRemSec % 60;
        if (m > 0) {
            snprintf(buf, sizeof(buf), "Next: %lu min", (unsigned long)m);
        } else {
            snprintf(buf, sizeof(buf), "Next: %lu sec", (unsigned long)s);
        }
    } else {
        snprintf(buf, sizeof(buf), "Drink now!");
    }
    _display.setCursor(0, 12);
    _display.println(buf);
}

void DisplayManager::_drawPage3System(bool wifiOk, const String& ip, bool ntpSynced) {
    // Line 1 (y=0): IP address
    _display.setCursor(0, 0);
    String ipStr = ip.length() > 0 ? ip : "--";
    _display.println(ipStr);

    // Line 2 (y=12): WiFi + NTP status
    char buf[22];
    snprintf(buf, sizeof(buf), "%s  NTP:%s",
             wifiOk ? "WiFi:OK" : "WiFi:XX",
             ntpSynced ? "Y" : "N");
    _display.setCursor(0, 12);
    _display.println(buf);
}

void DisplayManager::_drawPageIndicator(uint8_t page) {
    // 4 dots (2×2 px each, gap 3 px) centered at y=29
    const int dotW = 2, dotH = 2, gap = 3;
    const int total = PAGE_COUNT * (dotW + gap) - gap;
    int x = (OLED_SCREEN_WIDTH - total) / 2;
    const int y = 30;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == (int)page) {
            _display.fillRect(x, y, dotW, dotH, SSD1306_WHITE);
        } else {
            _display.drawPixel(x, y, SSD1306_WHITE);
        }
        x += dotW + gap;
    }
}

// ── Periodic re-flush (AP mode idle refresh) ───────────────────────────────

void DisplayManager::update() {
    if (!_available) return;
    if (millis() - _lastUpdateMs < OLED_UPDATE_INTERVAL_MS) return;
    _lastUpdateMs = millis();
    _display.display();
}
