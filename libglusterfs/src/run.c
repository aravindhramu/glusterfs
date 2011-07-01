/*
   Copyright (c) 2011 Gluster, Inc. <http://www.gluster.com>
   This file is part of GlusterFS.

   GlusterFS is free software; you can redistribute it and/or modify
   it under the terms of the GNU Affero General Public License as published
   by the Free Software Foundation; either version 3 of the License,
   or (at your option) any later version.

   GlusterFS is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <assert.h>
#include <sys/wait.h>

#ifdef RUN_STANDALONE
#define GF_CALLOC(n, s, t) calloc(n, s)
#define GF_ASSERT(cond) assert(cond)
#define GF_REALLOC(p, s) realloc(p, s)
#define GF_FREE(p) free(p)
#define gf_strdup(s) strdup(s)
#define gf_vasprintf(p, f, va) vasprintf(p, f, va)
#define gf_loglevel_t int
#define gf_log(dom, levl, fmt, args...) printf("LOG: " fmt "\n", ##args)
#define LOG_DEBUG 0
#ifdef __linux__
#define GF_LINUX_HOST_OS
#endif
#else /* ! RUN_STANDALONE */
#include "glusterfs.h"
#include "common-utils.h"
#endif

#include "run.h"

void
runinit (runner_t *runner)
{
        int i = 0;

        runner->argvlen = 64;
        runner->argv = GF_CALLOC (runner->argvlen,
                                  sizeof (*runner->argv),
                                  gf_common_mt_run_argv);
        runner->runerr = runner->argv ? 0 : errno;
        runner->chpid = -1;
        for (i = 0; i < 3; i++) {
                runner->chfd[i] = -1;
                runner->chio[i] = NULL;
        }
}

FILE *
runner_chio (runner_t *runner, int fd)
{
        GF_ASSERT (fd > 0 && fd < 3);

        return runner->chio[fd];
}

static void
runner_insert_arg (runner_t *runner, char *arg)
{
        int i = 0;

        GF_ASSERT (arg);

        if (runner->runerr)
                return;

        for (i = 0; i < runner->argvlen; i++) {
                if (runner->argv[i] == NULL)
                        break;
        }
        GF_ASSERT (i < runner->argvlen);

        if (i == runner->argvlen - 1) {
                runner->argv = GF_REALLOC (runner->argv,
                                           runner->argvlen * 2 * sizeof (*runner->argv));
                if (!runner->argv) {
                        runner->runerr = errno;
                        return;
                }
                memset (/* "+" is aware of the type of its left side,
                         * no need to multiply with type-size */
                        runner->argv + runner->argvlen,
                        0, runner->argvlen * sizeof (*runner->argv));
                runner->argvlen *= 2;
        }

        runner->argv[i] = arg;
}

void
runner_add_arg (runner_t *runner, const char *arg)
{
        arg = gf_strdup (arg);
        if (!arg) {
                runner->runerr = errno;
                return;
        }

        runner_insert_arg (runner, (char *)arg);
}

static void
runner_va_add_args (runner_t *runner, va_list argp)
{
        const char *arg;

        while ((arg = va_arg (argp, const char *)))
                runner_add_arg (runner, arg);
}

void
runner_add_args (runner_t *runner, ...)
{
        va_list argp;

        va_start (argp, runner);
        runner_va_add_args (runner, argp);
        va_end (argp);
}

void
runner_argprintf (runner_t *runner, const char *format, ...)
{
        va_list argva;
        char *arg = NULL;
        int ret = 0;

        va_start (argva, format);
        ret = gf_vasprintf (&arg, format, argva);
        va_end (argva);

        if (ret < 0) {
                runner->runerr = errno;
                return;
        }

        runner_insert_arg (runner, arg);
}

void
runner_log (runner_t *runner, const char *dom, gf_loglevel_t lvl,
            const char *msg)
{
        char *buf = NULL;
        size_t len = 0;
        int i = 0;

        if (runner->runerr)
                return;

        for (i = 0;; i++) {
                if (runner->argv[i] == NULL)
                        break;
                len += (strlen (runner->argv[i]) + 1);
        }

        buf = GF_CALLOC (1, len + 1, gf_common_mt_run_logbuf);
        if (!buf) {
                runner->runerr = errno;
                return;
        }
        for (i = 0;; i++) {
                if (runner->argv[i] == NULL)
                        break;
                strcat (buf, runner->argv[i]);
                strcat (buf, " ");
        }
        if (len > 0)
                buf[len - 1] = '\0';

        gf_log (dom, lvl, "%s: %s", msg, buf);

        GF_FREE (buf);
}

void
runner_redir (runner_t *runner, int fd, int tgt_fd)
{
        GF_ASSERT (fd > 0 && fd < 3);

        runner->chfd[fd] = (tgt_fd >= 0) ? tgt_fd : -2;
}

int
runner_start (runner_t *runner)
{
        int pi[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
        int xpi[2];
        int ret = 0;
        int errno_priv = 0;
        int i = 0;
        sigset_t set;

        if (runner->runerr) {
                errno = runner->runerr;
                return -1;
        }

        GF_ASSERT (runner->argv[0]);

        /* set up a channel to child to communicate back
         * possible execve(2) failures
         */
        ret = pipe(xpi);
        if (ret != -1)
                ret = fcntl (xpi[1], F_SETFD, FD_CLOEXEC);

        for (i = 0; i < 3; i++) {
                if (runner->chfd[i] != -2)
                        continue;
                ret = pipe (pi[i]);
                if (ret != -1) {
                        runner->chio[i] = fdopen (pi[i][i ? 0 : 1], i ? "r" : "w");
                        if (!runner->chio[i])
                                ret = -1;
                }
        }

        if (ret != -1)
                runner->chpid = fork ();
        switch (runner->chpid) {
        case -1:
                errno_priv = errno;
                close (xpi[0]);
                close (xpi[1]);
                for (i = 0; i < 3; i++) {
                        close (pi[i][0]);
                        close (pi[i][1]);
                }
                errno = errno_priv;
                return -1;
        case 0:
                for (i = 0; i < 3; i++)
                        close (pi[i][i ? 0 : 1]);
                close (xpi[0]);
                ret = 0;
#ifdef GF_LINUX_HOST_OS
                {
                        DIR *d = NULL;
                        struct dirent *de = NULL;
                        char *e = NULL;

                        d = opendir ("/proc/self/fd");
                        if (d) {
                                while ((de = readdir (d))) {
                                        i = strtoul (de->d_name, &e, 10);
                                        if (*e == '\0' &&
                                            i > 2 && i != dirfd (d) &&
                                            i != pi[0][0] && i != pi[1][1] &&
                                            i != pi[2][1] && i != xpi[1])
                                                close (i);
                                }
                                closedir (d);
                        } else
                                ret = -1;
                }
#else
                for (i = 3; i < 65536; i++) {
                        if (i != pi[0][0] && i != pi[1][1] &&
                            i != pi[2][1] && i != xpi[1])
                                close (i);
                }
#endif

                for (i = 0; i < 3; i++) {
                        if (ret == -1)
                                break;
                        switch (runner->chfd[i]) {
                        case -1:
                                /* no redir */
                                break;
                        case -2:
                                /* redir to pipe */
                                ret = dup2 (pi[i][i ? 1 : 0], i);
                                errno_priv = errno;
                                close (pi[i][i ? 1 : 0]);
                                errno = errno_priv;
                                break;
                        default:
                                /* redir to file */
                                ret = dup2 (runner->chfd[i], i);
                        }
                }

                if (ret != -1) {
                        /* save child from inheriting our singal handling */
                        sigemptyset (&set);
                        sigprocmask (SIG_SETMASK, &set, NULL);

                        execvp (runner->argv[0], runner->argv);
                }
                ret = write (xpi[1], &errno, sizeof (errno));
                _exit (1);
        }

        errno_priv = errno;
        for (i = 0; i < 3; i++)
                close (pi[i][i ? 1 : 0]);
        close (xpi[1]);
        if (ret == -1) {
                for (i = 0; i < 3; i++) {
                        if (runner->chio[i]) {
                                fclose (runner->chio[i]);
                                runner->chio[i] = NULL;
                        }
                }
        } else {
                ret = read (xpi[0], (char *)&errno_priv, sizeof (errno_priv));
                close (xpi[0]);
                if (ret <= 0)
                        return 0;
                GF_ASSERT (ret == sizeof (errno_priv));
        }
        errno = errno_priv;
        return -1;
}

int
runner_end_reuse (runner_t *runner)
{
        int i = 0;
        int ret = -1;
        int chstat = 0;

        if (runner->chpid > 0) {
                if (waitpid (runner->chpid, &chstat, 0) == runner->chpid)
                        ret = chstat;
        }

        for (i = 0; i < 3; i++) {
                if (runner->chio[i]) {
                        fclose (runner->chio[i]);
                        runner->chio[i] = NULL;
                }
        }

        return ret;
}

int
runner_end (runner_t *runner)
{
        int i = 0;
        int ret = -1;
        char **p = NULL;

        ret = runner_end_reuse (runner);

        for (p = runner->argv; *p; p++)
                GF_FREE (*p);
        GF_FREE (runner->argv);
        for (i = 0; i < 3; i++)
                close (runner->chfd[i]);

        return ret;
}

static int
runner_run_generic (runner_t *runner, int (*rfin)(runner_t *runner))
{
        int ret = 0;

        ret = runner_start (runner);
        if (ret != 0)
                return -1;

        return rfin (runner) ? -1 : 0;
}

int
runner_run (runner_t *runner)
{
        return runner_run_generic (runner, runner_end);
}

int
runner_run_reuse (runner_t *runner)
{
        return runner_run_generic (runner, runner_end_reuse);
}

int
runcmd (const char *arg, ...)
{
        runner_t runner;
        va_list argp;

        runinit (&runner);
        /* ISO C requires a named argument before '...' */
        runner_add_arg (&runner, arg);

        va_start (argp, arg);
        runner_va_add_args (&runner, argp);
        va_end (argp);

        return runner_run (&runner);
}

#ifdef RUN_DO_TESTS
static void
TBANNER (const char *txt)
{
        printf("######\n### testing %s\n", txt);
}

int
main ()
{
        runner_t runner;
        char buf[80];
        char *wdbuf;;
        int ret;
        long pathmax = pathconf ("/", _PC_PATH_MAX);

        wdbuf = malloc (pathmax);
        assert (wdbuf);
        getcwd (wdbuf, pathmax);

        TBANNER ("basic functionality");
        runcmd ("echo", "a", "b", NULL);

        TBANNER ("argv extension");
        runcmd ("echo", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
                "11", "12", "13", "14", "15", "16", "17", "18", "19", "20",
                "21", "22", "23", "24", "25", "26", "27", "28", "29", "30",
                "31", "32", "33", "34", "35", "36", "37", "38", "39", "40",
                "41", "42", "43", "44", "45", "46", "47", "48", "49", "50",
                "51", "52", "53", "54", "55", "56", "57", "58", "59", "60",
                "61", "62", "63", "64", "65", "66", "67", "68", "69", "70",
                "71", "72", "73", "74", "75", "76", "77", "78", "79", "80",
                "81", "82", "83", "84", "85", "86", "87", "88", "89", "90",
                "91", "92", "93", "94", "95", "96", "97", "98", "99", "100", NULL);

        TBANNER ("add_args, argprintf, log, and popen-style functionality");
        runinit (&runner);
        runner_add_args (&runner, "echo", "pid:", NULL);
        runner_argprintf (&runner, "%d\n", getpid());
        runner_add_arg (&runner, "wd:");
        runner_add_arg (&runner, wdbuf);
        runner_redir (&runner, 1, RUN_PIPE);
        runner_start (&runner);
        runner_log (&runner, "(x)", LOG_DEBUG, "starting program");
        while (fgets (buf, sizeof(buf), runner_chio (&runner, 1)))
                printf ("got: %s", buf);
        runner_end (&runner);

        TBANNER ("execve error reporting");
        ret = runcmd ("bafflavvitty", NULL);
        printf ("%d %d [%s]\n", ret, errno, strerror (errno));

        return 0;
}
#endif