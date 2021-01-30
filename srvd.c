#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "real_main.h"

int real_argc;
char** real_argv;

void closeall(int fd)
{
    int fdlimit = sysconf(_SC_OPEN_MAX);

    while (fd < fdlimit)
        close(fd++);
}

int daemon(int nochdir, int noclose)
{
    switch (fork()) {
    case 0:
        break;
    case -1:
        return -1;
    default:
        _exit(0); /* 原进程退出 */
    }

    if (setsid() < 0) { /* 不应该失败 */
        perror("setsid");
        return -1;
    }
    /* 如果你希望将来获得一个控制tty,则排除(dyke)以下的switch语句 */
    /* -- 正常情况不建议用于守护程序 */

    switch (fork()) {
    case 0:
        break;
    case -1:
        return -1;
    default:
        _exit(0);
    }

    if (!nochdir)
        chdir("/");
    if (!noclose) {
        closeall(0);
        open("/dev/null", O_RDWR);
        dup(0);
        dup(0);
    }

    return 0;
}

void mysystem(int argc, char** argv)
{
    switch (fork()) {
    case -1:
        fprintf(stderr, "fork failed");
        exit(-1);
    case 0:
        real_main(argc, argv);
        exit(0);
    default:
        break;
    }
}

void child_exit(int signo)
{
    int status;

    /* 非阻塞地等待任何子进程结束 */
    if (waitpid(-1, &status, WNOHANG) < 0) {
        /*
     * 不建议在信号处理函数中调用标准输入/输出函数，
     * 但在一个类似这个的玩具程序里或许没问题
     */
        fprintf(stderr, "waitpid failed\n");
        return;
    }
    fprintf(stderr, "child exit\n");
    mysystem(real_argc, real_argv);
    fprintf(stderr, "mysys over\n");
}

int main(int argc, char* argv[])
{
    int noclose = 0;
    int nochdir = 1;
    if (argc > 1 && !strcmp(argv[1], "-d")) {
        noclose = 1;
        real_argc = argc - 2;
        real_argv = &argv[2];
    } else {
        real_argc = argc - 1;
        real_argv = &argv[1];
    }

    if (daemon(nochdir, noclose) < 0) {
        perror("daemon");
        exit(2);
    }
    struct sigaction act;

    /* 设定sig_chld函数作为我们SIGCHLD信号的处理函数 */
    act.sa_handler = child_exit;

    /* 在这个范例程序里，我们不想阻塞其它信号 */
    sigemptyset(&act.sa_mask);

    /*
   * 我们只关心被终止的子进程，而不是被中断
   * 的子进程 (比如用户在终端上按Control-Z)
   */
    act.sa_flags = SA_NOCLDSTOP;

    /*
   * 使这些设定的值生效. 如果我们是写一个真实的应用程序，
   * 我们也许应该保存这些原有值，而不是传递一个NULL。
   */
    if (sigaction(SIGCHLD, &act, NULL) < 0) {
        fprintf(stderr, "sigaction failed\n");
        return 1;
    }

    mysystem(real_argc, real_argv);
    while (1)
        pause();
    return 0;
}
