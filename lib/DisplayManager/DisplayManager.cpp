#include "DisplayManager.h"

bool DisplayManager::init() {
    if (!_display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
        return false;
    }
    _display.clearDisplay();
    _display.display();
    _available    = true;
    _pageChangedMs = 0;  // lazy-init: anchored on first showNormalMode() call
    return true;
}

void DisplayManager::showBootScreen() {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 10);
    _display.println("Water Tracker");
    _display.setCursor(0, 26);
    _display.println("Booting...");
    _display.display();
    _lastUpdateMs = millis();
}

void DisplayManager::showError(const char* msg) {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 10);
    _display.println("Error");
    _display.setCursor(0, 26);
    _display.println(msg);
    _display.display();
    _lastUpdateMs = millis();
}

void DisplayManager::showWifiConnecting(const String& ssid) {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 0);
    _display.println("Connecting WiFi");
    _display.setCursor(0, 16);
    String s = ssid.length() > 16 ? ssid.substring(0, 13) + "..." : ssid;
    _display.println(s);
    _display.display();
    _lastUpdateMs = millis();
}

void DisplayManager::showAPMode(const String& apSsid, const String& ip) {
    if (!_available) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 0);
    _display.println("Setup Mode");
    _display.setCursor(0, 14);
    String s = apSsid.length() > 16 ? apSsid.substring(0, 13) + "..." : apSsid;
    _display.println(s);
    _display.setCursor(0, 28);
    _display.println(ip);
    _display.setCursor(0, 42);
    _display.println("Open setup page");
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

    // Lazy-init: anchor page timer on first Normal Mode entry so page 0 always shows first
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
        case 0: _drawPage0Weight(weightG, stable);                               break;
        case 1: _drawPage1Hydration(todayMl, goalMl, drinkCount);               break;
        case 2: _drawPage2Reminder(lastDrinkMl, nextRemSec);                    break;
        case 3: _drawPage3System(wifiOk, ip, ntpSynced);                        break;
        default: break;
    }
    _drawPageIndicator(_page);
    _display.display();
}

void DisplayManager::_drawPage0Weight(float weightG, bool stable) {
    char buf[32];
    _display.setCursor(0, 2);
    _display.println("[   Weight   ]");
    snprintf(buf, sizeof(buf), "%.0f g", weightG);
    _display.setCursor(0, 16);
    _display.println(buf);
    snprintf(buf, sizeof(buf), "Status: %s", stable ? "Stable" : "Moving");
    _display.setCursor(0, 30);
    _display.println(buf);
}

void DisplayManager::_drawPage1Hydration(float todayMl, uint32_t goalMl, uint32_t drinkCount) {
    char buf[32];
    _display.setCursor(0, 2);
    _display.println("[ Hydration  ]");
    snprintf(buf, sizeof(buf), "Today: %.0f ml", todayMl);
    _display.setCursor(0, 16);
    _display.println(buf);
    const uint32_t pct = (goalMl > 0) ? (uint32_t)(todayMl * 100.0f / goalMl) : 0;
    snprintf(buf, sizeof(buf), "Goal: %lu ml %lu%%",
             (unsigned long)goalMl, (unsigned long)(pct > 100 ? 100 : pct));
    _display.setCursor(0, 28);
    _display.println(buf);
    snprintf(buf, sizeof(buf), "Drinks: %lu", (unsigned long)drinkCount);
    _display.setCursor(0, 42);
    _display.println(buf);
}

void DisplayManager::_drawPage2Reminder(float lastDrinkMl, uint32_t nextRemSec) {
    char buf[32];
    _display.setCursor(0, 2);
    _display.println("[  Reminder  ]");
    snprintf(buf, sizeof(buf), "Last: %.0f ml", lastDrinkMl);
    _display.setCursor(0, 16);
    _display.println(buf);
    if (nextRemSec > 0) {
        const uint32_t m = nextRemSec / 60;
        const uint32_t s = nextRemSec % 60;
        if (m > 0) {
            snprintf(buf, sizeof(buf), "Next in: %lu min", (unsigned long)m);
        } else {
            snprintf(buf, sizeof(buf), "Next in: %lu sec", (unsigned long)s);
        }
    } else {
        snprintf(buf, sizeof(buf), "Drink now!");
    }
    _display.setCursor(0, 30);
    _display.println(buf);
}

void DisplayManager::_drawPage3System(bool wifiOk, const String& ip, bool ntpSynced) {
    _display.setCursor(0, 2);
    _display.println("[   System   ]");
    _display.setCursor(0, 16);
    _display.println(wifiOk ? "WiFi: OK" : "WiFi: Lost");
    _display.setCursor(0, 30);
    String ipTrunc = ip.length() > 15 ? ip.substring(0, 15) : ip;
    _display.println(ipTrunc.length() > 0 ? ipTrunc : "--");
    _display.setCursor(0, 44);
    _display.println(ntpSynced ? "NTP: Synced" : "NTP: Pending");
}

void DisplayManager::_drawPageIndicator(uint8_t page) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(page + 1), (unsigned)PAGE_COUNT);
    // Right-align at x=104 (128 - ~4 chars * 6px)
    _display.setCursor(104, 56);
    _display.println(buf);
}

// ── Periodic re-flush (AP mode idle refresh) ───────────────────────────────

void DisplayManager::update() {
    if (!_available) return;
    if (millis() - _lastUpdateMs < OLED_UPDATE_INTERVAL_MS) return;
    _lastUpdateMs = millis();
    _display.display();
}
