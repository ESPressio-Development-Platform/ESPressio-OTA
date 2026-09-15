#include <sdkconfig.h>

// Arduino-ESP32 validates ESP_OTA_IMG_PENDING_VERIFY during initArduino()
// before setup() runs unless this weak hook is overridden. The Coordinator
// recovery lab must keep the candidate in Trial until ESPressio-OTA itself
// evaluates health and owns the Commit/Rollback decision.
#if defined(CONFIG_APP_ROLLBACK_ENABLE)
extern "C" bool verifyRollbackLater() {
    return true;
}
#endif
