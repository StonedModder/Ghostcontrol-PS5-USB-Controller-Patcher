/* One log line per call: to a file when opened with a path, else stderr. */
#ifndef PH_LOG_H
#define PH_LOG_H

int  log_open(const char *path);
void log_close(void);
/* Current uncertainty, never a guess based on whether a stream is visible.
 * Checked close returns 0 only for a known-empty or successfully closed owner.
 * A failed fclose invalidates FILE*, but does not authorize another owner;
 * its ambiguity is sticky and the descriptor/stream is never retried. */
const char *log_release_blocker(void);
int log_close_checked(void);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
