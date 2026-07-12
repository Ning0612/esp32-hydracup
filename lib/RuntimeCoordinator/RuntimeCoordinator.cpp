#include "RuntimeCoordinator.h"
#include <cstring>

bool RuntimeCoordinator::begin() {
    _snapshotMutex = xSemaphoreCreateMutex();
    _commandQueue = xQueueCreate(8, sizeof(ControlCommand));
    _resultQueue = xQueueCreate(8, sizeof(ControlResult));
    if (_snapshotMutex && _commandQueue && _resultQueue) return true;

    Serial.println("[RTOS] RuntimeCoordinator allocation failed");
    return false;
}

void RuntimeCoordinator::publishControl(const RuntimeSnapshot& next) {
    if (!_snapshotMutex || xSemaphoreTake(_snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    const bool wifiConnected = _snapshot.wifiConnected;
    char ipAddress[sizeof(_snapshot.ipAddress)];
    std::memcpy(ipAddress, _snapshot.ipAddress, sizeof(ipAddress));
    const uint32_t commandDrops = _snapshot.commandDrops;
    const uint32_t resultDrops = _snapshot.resultDrops;
    const uint32_t sequence = _snapshot.sequence;

    _snapshot = next;
    _snapshot.wifiConnected = wifiConnected;
    std::memcpy(_snapshot.ipAddress, ipAddress, sizeof(_snapshot.ipAddress));
    _snapshot.commandDrops = commandDrops;
    _snapshot.resultDrops = resultDrops;
    _snapshot.sequence = sequence + 1;
    xSemaphoreGive(_snapshotMutex);
    _lastControlPublishTick.store(xTaskGetTickCount(), std::memory_order_release);
}

bool RuntimeCoordinator::isControlHealthy(TickType_t maxAgeTicks) const {
    if (!_controlRunning.load(std::memory_order_acquire)) return false;
    const TickType_t last = _lastControlPublishTick.load(std::memory_order_acquire);
    return last != 0 && (xTaskGetTickCount() - last) <= maxAgeTicks;
}

void RuntimeCoordinator::publishConnectivity(bool wifiConnected, const String& ipAddress) {
    if (!_snapshotMutex || xSemaphoreTake(_snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    _snapshot.wifiConnected = wifiConnected;
    std::strncpy(_snapshot.ipAddress, ipAddress.c_str(), sizeof(_snapshot.ipAddress) - 1);
    _snapshot.ipAddress[sizeof(_snapshot.ipAddress) - 1] = '\0';
    _snapshot.sequence++;
    xSemaphoreGive(_snapshotMutex);
}

RuntimeSnapshot RuntimeCoordinator::snapshot() const {
    RuntimeSnapshot copy;
    if (!_snapshotMutex || xSemaphoreTake(_snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) return copy;
    copy = _snapshot;
    xSemaphoreGive(_snapshotMutex);
    return copy;
}

bool RuntimeCoordinator::request(ControlCommand command, ControlResult& result,
                                 TickType_t timeoutTicks) {
    if (!isControlHealthy() || !_commandQueue || !_resultQueue) {
        result.status = ControlResultStatus::FAILED;
        return false;
    }

    command.requestId = _nextRequestId++;
    if (_nextRequestId == 0) _nextRequestId = 1;

    if (xQueueSend(_commandQueue, &command, 0) != pdTRUE) {
        result.requestId = command.requestId;
        result.status = ControlResultStatus::QUEUE_FULL;
        if (_snapshotMutex && xSemaphoreTake(_snapshotMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            _snapshot.commandDrops++;
            xSemaphoreGive(_snapshotMutex);
        }
        return false;
    }

    const TickType_t start = xTaskGetTickCount();
    TickType_t remaining = timeoutTicks;
    ControlResult candidate;
    while (xQueueReceive(_resultQueue, &candidate, remaining) == pdTRUE) {
        if (candidate.requestId == command.requestId) {
            result = candidate;
            return candidate.status == ControlResultStatus::OK;
        }
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeoutTicks) break;
        remaining = timeoutTicks - elapsed;
    }

    result.requestId = command.requestId;
    result.status = ControlResultStatus::TIMEOUT;
    return false;
}

bool RuntimeCoordinator::receive(ControlCommand& command, TickType_t timeoutTicks) {
    return _commandQueue && xQueueReceive(_commandQueue, &command, timeoutTicks) == pdTRUE;
}

void RuntimeCoordinator::reply(const ControlResult& result) {
    if (!_resultQueue || xQueueSend(_resultQueue, &result, 0) == pdTRUE) return;
    if (_snapshotMutex && xSemaphoreTake(_snapshotMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        _snapshot.resultDrops++;
        xSemaphoreGive(_snapshotMutex);
    }
}
