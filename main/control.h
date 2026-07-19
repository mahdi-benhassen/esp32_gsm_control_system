/**
 * control.h — Irrigation state machine + SMS command interface.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the control task (irrigation state machine + LED heartbeat). */
void control_start(void);

/** Start the SMS polling task (inbox scan + command execution). */
void sms_poll_start(void);

#ifdef __cplusplus
}
#endif
