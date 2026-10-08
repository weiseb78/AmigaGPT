#ifndef STREAMLOG_H
#define STREAMLOG_H

#include <exec/types.h>

#define STREAMLOG_SSE_SNIPPET_MAX 200

/*
 * MorphOS: Work:Tmp survives hard reset (T: is RAM and gone after reboot).
 * Other targets keep T: for lightweight debug dumps.
 */
#ifdef __MORPHOS__
#define STREAMLOG_STREAM_PATH "Work:Tmp/amigagpt_stream.log"
#define STREAMLOG_UTF8_PATH "Work:Tmp/amigagpt_utf8.log"
#define STREAMLOG_LIFECYCLE_PATH "AMIGAGPT:amigagpt_lifecycle.log"
#define STREAMLOG_LIFECYCLE_MIRROR_PATH "Work:Tmp/amigagpt_lifecycle.log"
#define STREAMLOG_SHUTDOWN_LAST_PATH "Work:Tmp/amigagpt_shutdown.last"
#define STREAMLOG_STARTUP_LAST_PATH "Work:Tmp/amigagpt_startup.last"
#else
#define STREAMLOG_STREAM_PATH "T:amigagpt_stream.log"
#define STREAMLOG_UTF8_PATH "T:amigagpt_utf8.log"
#define STREAMLOG_LIFECYCLE_PATH "AMIGAGPT:amigagpt_lifecycle.log"
#define STREAMLOG_LIFECYCLE_MIRROR_PATH "T:amigagpt_lifecycle.log"
#define STREAMLOG_SHUTDOWN_LAST_PATH "T:amigagpt_shutdown.last"
#define STREAMLOG_STARTUP_LAST_PATH "T:amigagpt_startup.last"
#endif

void streamLogSyncFromConfig(void);

/** Apply debug flags directly (used during loadConfig before configObj exists). */
void streamLogSyncFromFlags(BOOL debugStreamLog, BOOL debugLifecycleLog);

BOOL streamLogIsEnabled(void);

void streamLogChatEnd(CONST_STRPTR outcome, ULONG messageLen,
                      BOOL completedOk, BOOL truncated,
                      CONST_STRPTR lastSseSnippet);

/** Chat/API errors that return before finishChatStream (e.g. web_search + GPT-3.5). */
void streamLogApiError(CONST_STRPTR kind, CONST_STRPTR detail);

void streamLogUtf8(CONST_STRPTR detail);

/** Startup trace (only when debugStreamLog is on); helps diagnose silent WB start. */
void streamLogBootPhase(CONST_STRPTR phase);

/** Lifecycle trace when config.debugLifecycleLog is on (startup/shutdown races). */
void streamLogLifecycle(CONST_STRPTR phase);

/** MorphOS: overwrite amigagpt_shutdown.last with last shutdown phase (always on). */
void streamLogShutdownPhase(CONST_STRPTR phase);

/** MorphOS: overwrite amigagpt_startup.last with last startup phase (always on). */
void streamLogStartupPhase(CONST_STRPTR phase);

#endif
