#ifndef PUDDLE_PROCESS_H
#define PUDDLE_PROCESS_H

/* The running program and its terminal, the same on every OS. */

#ifdef __cplusplus
extern "C" {
#endif

long puddle_process_id(void);

/* Are both stdin and stdout a terminal (someone typing), not a pipe or a file? 1 or 0. */
int puddle_console_interactive(void);

/* Stop showing what is typed (for a recovery phrase), or show it again. 1 if the terminal did as asked, 0 if there is
 * no terminal to ask. */
int puddle_console_echo(int on);

/* Call `fn` when the program is asked to stop: Ctrl-C, or the service manager stopping it (SIGTERM; on Windows a
 * console close). `fn` may run at any moment, on whatever thread the OS chooses, so it must only do what is safe there:
 * set a flag, or wake a waker (platform/net.h). */
void puddle_on_stop_signal(void (*fn)(void));

/* Writing to a connection the other side has closed is an error to handle, not a reason to end the program
 * (SIGPIPE on POSIX; nothing to do on Windows). */
void puddle_ignore_broken_pipe(void);

#ifdef __cplusplus
}
#endif
#endif
