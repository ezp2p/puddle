/* platform/process.h on Windows. NOT YET BUILT OR TESTED ON WINDOWS: written on a Mac, to be checked on a Windows
 * machine. */
#include "platform/process.h"

#include <io.h>
#include <process.h>
#include <stdio.h>
#include <windows.h>

long puddle_process_id(void)
{
    return (long)_getpid();
}

int puddle_console_interactive(void)
{
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
}

int puddle_console_echo(int on)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode))
        return 0;
    mode = on ? (mode | ENABLE_ECHO_INPUT) : (mode & ~(DWORD)ENABLE_ECHO_INPUT);
    return SetConsoleMode(in, mode) != 0;
}

static void (*volatile g_stop)(void);

static BOOL WINAPI on_console_event(DWORD event)
{
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_stop)
            g_stop();
        return TRUE;
    default:
        return FALSE;
    }
}

void puddle_on_stop_signal(void (*fn)(void))
{
    g_stop = fn;
    SetConsoleCtrlHandler(on_console_event, TRUE);
}

void puddle_ignore_broken_pipe(void)
{
    /* Windows has no SIGPIPE: a write to a closed connection fails with an error, which is already handled. */
}
