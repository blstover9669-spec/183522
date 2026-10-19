/* svc_internal.h - internals shared by the service manager and built-in services. */
#ifndef SVC_INTERNAL_H
#define SVC_INTERNAL_H

#include "shell/shell_internal.h"
#include <nova/service.h>

/* syslog -> per-service log rings (manager.c) */
void svc_note_log(const log_entry_t *e);

/* main-loop statistics kept by wm.c (for sysmon and the screensaver) */
extern volatile uint32_t wm_stat_idle_ms;     /* total ms spent halted waiting for interrupts */
extern uint32_t wm_stat_events;               /* events delivered to windows (ever)           */
extern uint32_t wm_last_input_tick;           /* g_ticks of the last keyboard/mouse input     */
/* Input filter: returns true to swallow a raw input event (screensaver). */
extern bool (*wm_input_filter)(const input_event_t *ev);

/* notifyd (dialogs.c ui_notify uses it) */
bool notifyd_running(void);

#endif
