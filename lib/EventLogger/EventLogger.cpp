#include "EventLogger.h"
#include "TimeManager.h"
#include <LittleFS.h>

void EventLogger::init(bool fsOk) {
    _fsOk = fsOk;
    if (_fsOk && !LittleFS.exists("/logs")) {
        if (!LittleFS.mkdir("/logs")) {
            Serial.println("[EventLog] Failed to create /logs — logging disabled");
            _fsOk = false;
        }
    }
}

void EventLogger::logDrink(const String& timestamp, float amountMl, float totalMl, TimeManager* tm) {
    if (!_fsOk) return;

    const String ym   = (tm && tm->isSynced()) ? tm->getYearMonth() : String("unsynced");
    const String path = String("/logs/drink-") + ym + ".jsonl";

    File f = LittleFS.open(path, "a");
    if (!f) {
        Serial.printf("[EventLog] Failed to open %s\n", path.c_str());
        return;
    }

    char line[128];
    snprintf(line, sizeof(line),
             "{\"ts\":\"%s\",\"ml\":%.0f,\"total\":%.0f}\n",
             timestamp.c_str(), amountMl, totalMl);
    f.print(line);
    f.close();

    Serial.printf("[EventLog] %s\n", path.c_str());
}
