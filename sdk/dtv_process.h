/* dtv_process.h - starting, watching and stopping the daemon process.
 * Private to the client library. */
#ifndef DTV_PROCESS_H
#define DTV_PROCESS_H

#include <stddef.h>

typedef struct dtv_process dtv_process;
typedef struct dtv_pipe dtv_pipe;

/* Starts program (a path) with argv (NULL-terminated, argv[0] included) in
 * the folder cwd, its standard output and error going to *output, which the
 * caller reads and closes on its own schedule. NULL when it could not be
 * started. */
dtv_process *dtv_process_spawn(const char *program, const char *const *argv,
                               const char *cwd, dtv_pipe **output);

/* 1 once the process has exited, 0 while it runs after waiting up to
 * milliseconds (0 only looks). */
int dtv_process_wait(dtv_process *process, unsigned milliseconds);

/* Ends it at once, as TerminateProcess or SIGKILL do. */
void dtv_process_kill(dtv_process *process);

/* Frees the record; the process itself is not touched. */
void dtv_process_free(dtv_process *process);

/* Ends every other process whose executable is named name (no folder, with
 * the .exe on Windows). Returns how many were ended. */
int dtv_process_kill_named(const char *name);

/* Blocks for output. Returns the byte count, 0 at the end, -1 on error. */
int dtv_pipe_read(dtv_pipe *pipe, char *buffer, size_t size);
void dtv_pipe_close(dtv_pipe *pipe);

#endif
