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

// The whole namespace, not named keys: drink_ctr holds nothing but the counters, and this
// way a later schema addition cannot leave a stale field behind after a wipe.
bool eraseNamespace(const char* name) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(name, NVS_READWRITE, &handle);
    bool ok = opened == ESP_ERR_NVS_NOT_FOUND;  // nothing stored counts as cleared
    if (opened == ESP_OK) {
        ok = nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    return ok;
}

bool eraseKey(const char* name, const char* key) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(name, NVS_READWRITE, &handle);
    bool ok = opened == ESP_ERR_NVS_NOT_FOUND;
    if (opened == ESP_OK) {
        const esp_err_t erased = nvs_erase_key(handle, key);
        ok = (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
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
    uint8_t pending = 0;
    if (lockNvs()) {
        nvs_handle_t handle = 0;
        if (nvs_open(MAINT_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
            if (nvs_get_u8(handle, CLEAR_KEY, &pending) != ESP_OK) pending = 0;
            nvs_close(handle);
        }
        unlockNvs();
    }
    if (!pending) return;

    LOG_INFO(TAG, "clearing drink history before mounting filesystems");
    // NVS before the format, so the step that cannot be undone runs last. A failure here
    // leaves everything intact and the flag set, and the next boot simply tries again.
    // Today's totals live in NVS, not logfs; leaving them would show a total with no records.
    const bool counters = eraseNamespace("drink_ctr");
    // The cloud overflow store would otherwise re-inject the very events about to be deleted.
    const bool overflow = eraseKey("cloud_sync", "evt_overflow");
    if (!counters || !overflow) {
        LOG_ERROR(TAG, "history clear aborted (counters=%d overflow=%d); logfs untouched, "
                       "retrying on the next boot", counters, overflow);
        return;
    }

    // Formats an unmounted partition too: esp_littlefs_format() builds a temporary context
    // when the label is not registered yet.
    const esp_err_t formatted = esp_littlefs_format(LOGFS_LABEL);
    if (formatted != ESP_OK) {
        LOG_ERROR(TAG, "logfs format failed (%s); counters are already cleared, retrying on "
                       "the next boot", esp_err_to_name(formatted));
        return;
    }

    // Cleared last, so an interrupted wipe - including a power cut - simply runs again.
    if (eraseKey(MAINT_NAMESPACE, CLEAR_KEY)) LOG_INFO(TAG, "drink history cleared");
    else LOG_WARN(TAG, "history cleared but the request flag remains; it will repeat on boot");
}
