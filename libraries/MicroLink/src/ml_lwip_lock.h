#pragma once
/* Arduino-ESP32 enables CONFIG_LWIP_CHECK_THREAD_SAFETY, so netif state changes
 * from MicroLink tasks must hold the TCPIP core lock (unless already held/in tcpip thread). */
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "arch/sys_arch.h"
#define ML_LOCKED(stmt) do { \
    bool _held = sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER); \
    if (!_held) LOCK_TCPIP_CORE(); stmt; if (!_held) UNLOCK_TCPIP_CORE(); } while (0)
