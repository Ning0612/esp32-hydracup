#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class AppState;
class RuntimeCoordinator;

// Version check and firmware download are separate failure domains, so they get
// separate states: a failed check must not look like a failed download.
enum class OtaCheckState : uint8_t { UNKNOWN, CHECKING, UP_TO_DATE, UPDATE_AVAILABLE, CHECK_FAILED };
enum class OtaUpdateState : uint8_t { IDLE, DOWNLOADING, WRITING, READY_PENDING_REBOOT, FAILED };

enum class OtaRequestResult : uint8_t {
    ACCEPTED,
    BUSY,
    NO_UPDATE_AVAILABLE,
    INSUFFICIENT_MEMORY,
    OFFLINE,
};

struct OtaStatus {
    OtaCheckState checkState = OtaCheckState::UNKNOWN;
    OtaUpdateState updateState = OtaUpdateState::IDLE;
    char runningVersion[16] = {};
    char latestVersion[16] = {};
    char runningPartition[12] = {};
    char stage[10] = {};  // "firmware" or "webfs" while an update runs
    uint8_t progressPercent = 0;
    uint32_t imageSize = 0;
    uint32_t bytesRead = 0;
    int lastHttpStatus = 0;
    // Human-readable and produced by the firmware on purpose: a device can be left running a
    // UI older than its firmware (an interrupted or skipped webfs write, or a USB-flashed
    // image), so the page must be able to render states and failures it has never heard of.
    char message[96] = {};
    bool pendingVerify = false;
};

// Boot confirmation deliberately lives outside OtaUpdater: a device with no WiFi
// configured never constructs one, and gating the confirmation on the updater (or on
// connectivity) would leave those devices rolled back on every boot after an update.
// Call ota_boot_check() once during startup, then ota_mark_app_valid_if_due() from a
// periodic task in every mode.
void ota_boot_check();
bool ota_pending_verify();
void ota_mark_app_valid_if_due(bool controlHealthy);

class OtaUpdater {
public:
    bool init(AppState& state, RuntimeCoordinator& runtime);
    OtaRequestResult requestCheck();
    OtaRequestResult requestUpdate();
    OtaStatus snapshot() const;

    static const char* checkStateName(OtaCheckState state);
    static const char* updateStateName(OtaUpdateState state);

private:
    enum class Command : uint8_t { NONE, CHECK, UPDATE };

    static void _taskFunc(void* param);
    void _taskLoop();
    void _runCheck();
    void _runUpdate();
    bool _downloadAndWrite();
    bool _updateWebfs(const char* expectedSha);
    bool _fetchText(const char* url, char* out, size_t outLength, int& statusCode);
    bool _fetchLatestVersion(char* out, size_t outLength, int& statusCode);
    void _setStage(const char* stage);
    void _releasePrimitives();
    void _setMessage(const char* format, ...);
    void _publishRunningPartition();

    AppState* _state = nullptr;
    RuntimeCoordinator* _runtime = nullptr;
    SemaphoreHandle_t _wake = nullptr;
    SemaphoreHandle_t _statusMutex = nullptr;
    TaskHandle_t _task = nullptr;
    OtaStatus _status;
    std::atomic<bool> _busy{false};
    std::atomic<Command> _pending{Command::NONE};
};
