/* platform/process.h on macOS, Linux and the BSDs. */
#include "platform/process.h"

#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

long puddle_process_id(void)
{
    return (long)getpid();
}

int puddle_console_interactive(void)
{
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

int puddle_console_echo(int on)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
        return 0;
    if (on)
        t.c_lflag |= ECHO;
    else
        t.c_lflag &= (tcflag_t)~ECHO;
    return tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
}

static void (*volatile g_stop)(void);

static void on_signal(int sig)
{
    (void)sig;
    if (g_stop)
        g_stop();
}

void puddle_on_stop_signal(void (*fn)(void))
{
    g_stop = fn;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void puddle_ignore_broken_pipe(void)
{
    signal(SIGPIPE, SIG_IGN);
}
