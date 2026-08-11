#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "app_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class ConfigManager;
class EventLogger;
class AppState;

enum class CloudCommandType : uint8_t {
    SET_SETTINGS,
    SET_REMINDER_ENABLED,
    SET_REMINDER_INTERVAL_MIN,
    SET_DAILY_GOAL_ML,
    SNOOZE_MINUTES,
    PAUSE_TODAY,
    UNKNOWN
};

enum class CloudHistoryBackfillState : uint8_t {
    IDLE,
    QUEUED,
    UPLOADING,
    RETRYING,
    COMPLETE
};

struct CloudCommand {
    char id[48] = {};
    CloudCommandType type = CloudCommandType::UNKNOWN;
    uint32_t revision = 0;
    uint32_t uintValue = 0;
    bool boolValue = false;
    bool reminderEnabled = true;
    uint32_t reminderIntervalMin = 60;
    uint32_t dailyGoalMl = 2000;
    char stringValue[16] = {};
};

struct CloudDeviceStatus {
    float todayTotalMl = 0.0f;
    float lastDrinkMl = 0.0f;
    uint32_t drinkCount = 0;
    uint32_t reminderRemainingSec = 0;
    uint32_t alertEpisodeUptimeMs = 0;
    uint32_t dailyGoalMl = 0;
    char cupState[24] = {};
    char reminderState[24] = {};
    char lastDrinkAt[32] = {};
};

struct CloudAppliedSettings {
    bool reminderEnabled = true;
    uint32_t reminderIntervalMin = 60;
    uint32_t dailyGoalMl = 2000;
    char pausedUntilDate[16] = {};
};

class CloudSyncClient {
public:
    bool init(AppState& appState, AppConfig& config, ConfigManager& configManager,
              EventLogger& eventLogger, bool logFsOk);
    void updateStatus(const CloudDeviceStatus& status);
    void enqueueDrink(const char* timestamp, uint32_t uptimeMs, float amountMl,
                      float totalMl, uint32_t drinkCount);
    void enqueueRefill(const char* timestamp, uint32_t uptimeMs, float amountMl);
    bool receiveCommand(CloudCommand& command);
    void acknowledgeCommand(const CloudCommand& command, bool ok,
                            const CloudAppliedSettings& currentSettings);
    void persistSettings(const CloudAppliedSettings& settings);
    void configure(const AppConfig& config);
    void setConnectivity(bool connected) { _wifiConnected.store(connected); }

    bool isConfigured() const;
    bool lastSyncOk() const { return _lastSyncOk.load(); }
    uint32_t pendingEvents() const { return _pendingEvents.load(); }
    uint32_t lastSyncAgeSec() const;
    int lastHttpStatus() const { return _lastHttpStatus.load(); }
    uint32_t droppedEvents() const { return _droppedEvents.load(); }
    std::string pairingCode() const;
    std::string tokenHash() const;
    bool requestHistoryBackfill();
    bool historyBackfillRunning() const { return _historyBackfillActive.load(); }
    const char* historyBackfillState() const;
    uint32_t historyBackfillUploadedDays() const { return _historyBackfillUploadedDays.load(); }
    int historyBackfillHttpStatus() const { return _historyBackfillHttpStatus.load(); }

private:
    struct EventMessage {
        uint64_t sequence;
        char type[16];
        char timestamp[32];
        uint32_t uptimeMs;
        float amountMl;
        float totalMl;
        uint32_t drinkCount;
    };

    struct OverflowStore {
        uint32_t version = 1;
        uint32_t count = 0;
        EventMessage events[8] = {};
    };

    struct CommandAck {
        char id[48];
        uint32_t revision;
        bool ok;
    };

    struct CommandRecord {
        char id[48] = {};
        uint32_t revision = 0;
    };

    struct ConnectionConfig {
        bool enabled = false;
        int32_t timezoneOffsetSec = 0;
        char baseUrl[192] = {};
        char deviceId[64] = {};
        char token[65] = {};
    };

    struct HistoryDay {
        char localDate[11] = {};
        float totalMl = 0.0f;
        uint32_t drinkCount = 0;
        char lastDrinkAt[41] = {};
    };

    static void _taskFunc(void* param);
    void _taskLoop();
    bool _appendEvent(const EventMessage& event, bool allowOverflow = true);
    bool _saveOverflowEvent(const EventMessage& event);
    void _drainOverflowEvents();
    bool _loadOverflowStore(OverflowStore& store);
    bool _saveOverflowStore(const OverflowStore& store);
    bool _syncOnce();
    bool _post(const ConnectionConfig& config, const std::string& body,
               std::string& response, int& statusCode, const char* path);
    bool _historyBackfillOnce();
    bool _nextHistoryMonth(const char* afterMonth, std::string& month);
    bool _buildHistoryBatch(const std::string& month, const char* today,
                            HistoryDay* days, uint8_t& dayCount);
    bool _loadHistoryBackfillProgress();
    bool _saveHistoryBackfillProgress(bool active, const char* cursorMonth);
    std::string _historyBackfillIdentity() const;
    bool _applyResponse(const std::string& response, uint64_t maxSentSequence);
    bool _rewriteOutbox(uint64_t ackedThroughSeq);
    void _recoverOutbox();
    void _loadSequenceAndCount();
    void _saveSequence(uint64_t sequence);
    bool _saveAckedSequence(uint64_t sequence);
    ConnectionConfig _connectionConfig() const;
    bool _persistPendingConfig();
    bool _isCommandApplied(const char* id, uint32_t revision) const;
    bool _isCommandInFlight(const char* id, uint32_t revision) const;
    bool _recordCommandApplied(const char* id, uint32_t revision);
    void _recordCommandInFlight(const char* id, uint32_t revision);
    void _clearCommandInFlight(const char* id, uint32_t revision);
    bool _storeDeferredAck(const CommandAck& ack);
    void _drainDeferredAcks();
    void _enqueue(const char* type, const char* timestamp, uint32_t uptimeMs,
                  float amountMl, float totalMl, uint32_t drinkCount);
    static CloudCommandType _commandType(const char* value);

    ConfigManager* _configManager = nullptr;
    EventLogger* _eventLogger = nullptr;
    bool _logFsOk = false;
    QueueHandle_t _commandQueue = nullptr;
    QueueHandle_t _ackQueue = nullptr;
    SemaphoreHandle_t _statusMutex = nullptr;
    SemaphoreHandle_t _configMutex = nullptr;
    SemaphoreHandle_t _commandMutex = nullptr;
    SemaphoreHandle_t _overflowMutex = nullptr;
    SemaphoreHandle_t _historyBackfillMutex = nullptr;
    TaskHandle_t _task = nullptr;
    CloudDeviceStatus _status;
    ConnectionConfig _connection;
    CloudAppliedSettings _pendingSettings;
    bool _configPersistPending = false;
    uint32_t _configGeneration = 0;
    char _pairingCode[9] = {};
    uint64_t _nextSequence = 1;
    uint64_t _ackedSequence = 0;
    CommandRecord _appliedCommands[16] = {};
    CommandRecord _inflightCommands[8] = {};
    CommandAck _deferredAcks[8] = {};
    uint8_t _appliedCommandIndex = 0;
    uint8_t _inflightCommandIndex = 0;
    std::atomic<bool> _wifiConnected{false};
    std::atomic<bool> _syncRequested{false};
    std::atomic<bool> _workerReady{false};
    std::atomic<bool> _lastSyncOk{false};
    std::atomic<uint32_t> _pendingEvents{0};
    std::atomic<uint32_t> _lastSyncMs{0};
    std::atomic<int> _lastHttpStatus{0};
    std::atomic<uint32_t> _droppedEvents{0};
    std::atomic<bool> _historyBackfillActive{false};
    std::atomic<CloudHistoryBackfillState> _historyBackfillState{CloudHistoryBackfillState::IDLE};
    std::atomic<uint32_t> _historyBackfillUploadedDays{0};
    std::atomic<int> _historyBackfillHttpStatus{0};
    char _historyBackfillCursor[8] = {};
    char _historyBackfillIdentityTag[65] = {};
};
