#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { PmEvent_OnSleep = 0, PmEvent_OnWakeup = 1, PmEvent_OnReset = 2 } PmEvent;
typedef struct PmEventCookie { struct PmEventCookie *next; void (*handler)(void*, PmEvent); void *user; } PmEventCookie;
typedef void (*PmEventFn)(void *user, PmEvent event);
bool pmMainLoop(void);
bool pmIsSleepAllowed(void);
void pmSetSleepAllowed(bool allowed);
void pmAddEventHandler(PmEventCookie *cookie, PmEventFn handler, void *user);
void pmRemoveEventHandler(PmEventCookie *cookie);
void pmEnterSleep(void);
