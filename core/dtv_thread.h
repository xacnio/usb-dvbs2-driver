/* dtv_thread.h - keeping the streaming threads scheduled on a busy machine.
 *
 * A transponder delivers tens of megabits a second and neither the tuner
 * FIFO nor the USB host controller waits: a thread descheduled for a few
 * tens of milliseconds loses packets no buffering downstream can recover.
 * A full-screen game, a compile or a virus scan does exactly that, and the
 * reception counters then blame the dish.
 *
 * So the streaming threads ask for MMCSS on Windows, the scheduler class the
 * audio stack uses, with a plain priority rise beside it; for a low
 * real-time priority on Linux, which only a process allowed to gets; and for
 * the interactive quality-of-service class on macOS. Each is a request, not
 * a guarantee, and a thread that is refused simply runs as before. */
#ifndef DTV_THREAD_H
#define DTV_THREAD_H

/* Marks the calling thread as carrying the live stream. Returns a token
 * for dtv_thread_realtime_end(); NULL is a valid token. */
void *dtv_thread_realtime_begin(void);
void  dtv_thread_realtime_end(void *token);

/* Takes the process out of the power-saving class Windows puts background
 * applications into. Since Windows 10 a non-foreground process can be run
 * at a reduced clock on the efficiency cores -- which is the state this
 * application is always in while a game is on screen, and exactly when the
 * picture broke up. This only asks for normal speed; it does not raise the
 * process above anything else. */
void dtv_thread_process_no_throttle(void);

#endif /* DTV_THREAD_H */
