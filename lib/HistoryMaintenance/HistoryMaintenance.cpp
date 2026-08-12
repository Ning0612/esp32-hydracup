#include "HistoryMaintenance.h"

#include "StorageLock.h"
#include "esp_littlefs.h"
#include "hal_log.h"
#include "nvs.h"

namespace {

constexpr const char* TAG = "History";
constexpr const char* MAINT_NAMESPACE = "maint";
constexpr const char* CLEAR_KEY = "clear_logs";
constexpr const char* LOGFS_LABEL = "logfs";

bool eraseNamespace(const char* name) {
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(name, NVS_READWRITE, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return true;  // nothing stored
    if (opened != ESP_OK) return false;
    const bool ok = nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool eraseKey(const char* name, const char* key) {
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(name, NVS_READWRITE, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
    if (opened != ESP_OK) return false;
    const esp_err_t erased = nvs_erase_key(handle, key);
    const bool ok = (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

}  // namespace

bool history_request_clear() {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open(MAINT_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_u8(handle, CLEAR_KEY, 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    return ok;
}

void history_apply_pending_clear() {
    // No lockNvs() anywhere in here: the tasks that contend for it do not exist yet.
    nvs_handle_t handle = 0;
    uint8_t pending = 0;
    if (nvs_open(MAINT_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, CLEAR_KEY, &pending) != ESP_OK) pending = 0;
        nvs_close(handle);
    }
    if (!pending) return;

    LOG_INFO(TAG, "clearing drink history before mounting filesystems");
    // Formats an unmounted partition too - esp_littlefs_format() builds a temporary context
    // when the label is not registered yet.
    const esp_err_t formatted = esp_littlefs_format(LOGFS_LABEL);
    // Today's totals are in NVS, not logfs; leaving them would show a total with no records.
    const bool counters = eraseNamespace("drink_ctr");
    // The cloud overflow store would otherwise re-inject the very events just deleted.
    const bool overflow = eraseKey("cloud_sync", "evt_overflow");

    if (formatted == ESP_OK && counters && overflow) {
        // Cleared last, so an interrupted wipe simply runs again on the next boot.
        if (!eraseKey(MAINT_NAMESPACE, CLEAR_KEY))
            LOG_WARN(TAG, "history cleared but the request flag remains; it will repeat");
        else
            LOG_INFO(TAG, "drink history cleared");
        return;
    }
    LOG_ERROR(TAG, "history clear incomplete (format=%s counters=%d overflow=%d), retrying next boot",
              esp_err_to_name(formatted), counters, overflow);
}
