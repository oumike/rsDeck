#include "CoreSync.h"
#include <string.h>

namespace CoreSync {

NetStatus netStatus;

// Toast bridge: a single-slot mailbox. Producers (any core) write the message
// under a short critical section and bump toastSeq; the UI consumer reads it.
namespace {
    char     s_toastMsg[64] = {0};
    uint32_t s_toastDur = 0;
#if RSDECK_UI_CORE_SPLIT
    portMUX_TYPE s_toastMux = portMUX_INITIALIZER_UNLOCKED;
#endif
    uint32_t s_lastToastSeen = 0;
}

void requestToast(const char* msg, uint32_t durationMs) {
    if (!msg) return;
#if RSDECK_UI_CORE_SPLIT
    portENTER_CRITICAL(&s_toastMux);
#endif
    strlcpy(s_toastMsg, msg, sizeof(s_toastMsg));
    s_toastDur = durationMs;
#if RSDECK_UI_CORE_SPLIT
    portEXIT_CRITICAL(&s_toastMux);
#endif
    netStatus.toastSeq.fetch_add(1);
}

uint32_t takePendingToast(char* out, uint32_t outSize) {
    uint32_t seq = netStatus.toastSeq.load();
    if (seq == s_lastToastSeen) return 0;
    s_lastToastSeen = seq;
    uint32_t dur;
#if RSDECK_UI_CORE_SPLIT
    portENTER_CRITICAL(&s_toastMux);
#endif
    strlcpy(out, s_toastMsg, outSize);
    dur = s_toastDur;
#if RSDECK_UI_CORE_SPLIT
    portEXIT_CRITICAL(&s_toastMux);
#endif
    return dur == 0 ? 1 : dur;  // never return 0 for a real toast
}

#if RSDECK_UI_CORE_SPLIT

SemaphoreHandle_t spiBusMutex = nullptr;
SemaphoreHandle_t rnsMutex = nullptr;

void begin() {
    if (!spiBusMutex) spiBusMutex = xSemaphoreCreateRecursiveMutex();
    if (!rnsMutex)    rnsMutex = xSemaphoreCreateRecursiveMutex();
}

#else

void begin() {}

#endif

}  // namespace CoreSync
