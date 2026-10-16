#ifndef SNESRECOMP_HOST_LOGS_H
#define SNESRECOMP_HOST_LOGS_H
/* The logs folder a player can send when something goes wrong:
 *   logs/babel-recomp.log        this session (debug log, header first)
 *   logs/previous-session.log    the one before
 *   logs/unclean-previous.log    the previous session neither exited nor crashed cleanly
 *   logs/crash-latest.log        the session log at the moment of a crash
 *   logs/crash-YYYYmmdd_HHMMSS.log, crash-latest-report.json, crash_report_*.json,
 *   crash_minidump_*.dmp (Windows)
 * SNESRECOMP_LOG_FILE=0 leaves stderr alone. */
#ifdef __cplusplus
extern "C" {
#endif
void HostLogs_Init(const char *game_name, const char *build);
void HostLogs_OnCrash(const char *what);
const char *HostLogs_Dir(void);
#ifdef __cplusplus
}
#endif
#endif
