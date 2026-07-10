#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <ctype.h>
#include "../include/shell.h"
#include "../include/scheduler.h"

typedef enum
{
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_state_t;

typedef struct job
{
    int jid;
    pid_t pgid;
    char *cmdline;
    job_state_t state;
    pid_t *pids; /* all pids belonging to this job's pipeline */
    int npids;
    int nrunning; /* how many of those pids haven't exited yet */
    struct job *next;
} job_t;

static job_t *job_list = NULL;
static int next_jid = 1;
static struct termios shell_tmodes;
static pid_t shell_pgid;
static int shell_interactive = 0;
static volatile sig_atomic_t sigchld_pending = 0;
static volatile sig_atomic_t sigint_received = 0;

static void add_job(pid_t pgid, const char *cmdline, job_state_t state,
                    pid_t *pids, int npids)
{
    job_t *j = calloc(1, sizeof(job_t));
    if (!j)
        return;
    j->jid = next_jid++;
    j->pgid = pgid;
    j->cmdline = strdup(cmdline);
    j->state = state;
    j->pids = malloc((size_t)npids * sizeof(pid_t));
    if (j->pids)
        memcpy(j->pids, pids, (size_t)npids * sizeof(pid_t));
    j->npids = npids;
    j->nrunning = npids;
    j->next = job_list;
    job_list = j;
}

static job_t *find_job_by_jid(int jid)
{
    for (job_t *j = job_list; j; j = j->next)
        if (j->jid == jid)
            return j;
    return NULL;
}

static job_t *find_job_by_pid(pid_t pid)
{
    for (job_t *j = job_list; j; j = j->next)
        for (int i = 0; i < j->npids; ++i)
            if (j->pids[i] == pid)
                return j;
    return NULL;
}

static void remove_job(job_t *job)
{
    job_t **pp = &job_list;
    while (*pp && *pp != job)
        pp = &(*pp)->next;
    if (*pp)
    {
        *pp = job->next;
        free(job->cmdline);
        free(job->pids);
        free(job);
    }
}

static void print_jobs(void)
{
    for (job_t *j = job_list; j; j = j->next)
    {
        const char *state_name = (j->state == JOB_RUNNING)   ? "Running"
                                 : (j->state == JOB_STOPPED) ? "Stopped"
                                                             : "Done";
        printf("[%d] (%d) %s    %s\n", j->jid, j->pgid, state_name, j->cmdline);
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    sigchld_pending = 1;
}

static void sigint_handler(int sig)
{
    (void)sig;
    if (shell_interactive)
    {
        sigint_received = 1;
        write(STDOUT_FILENO, "\n", 1);
    }
}

static void safe_tcsetpgrp(pid_t pgid)
{
    if (!shell_interactive)
        return;

    if (tcsetpgrp(STDIN_FILENO, pgid) < 0)
    {
        int err = errno;
        if (err == ENOTTY || err == ENODEV || err == EIO || err == EPERM || err == EINVAL)
            return;
        perror("tcsetpgrp");
    }
}

/* Plain signal() can fall back to one-shot System V semantics depending on
 * feature-test macros (e.g. _POSIX_C_SOURCE) and the C standard used to
 * compile, silently resetting the handler to SIG_DFL after it fires once.
 * sigaction() always gives reliable, persistent (BSD-style) semantics, so we
 * use it for any handler we need to survive more than one delivery. */
static void install_handler(int signum, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(signum, &sa, NULL);
}

typedef struct cmd
{
    char **argv;
    char *infile;
    char *outfile;
    int append;
} cmd_t;

typedef struct
{
    char *text;
    int no_expand;
} token_t;

static void shell_register_job(const char *cmdline);
static int is_builtin_cmd(char **argv);
static int do_builtin(char **argv);
static void launch_pipeline(cmd_t **cmds, int ncmds, int background, const char *fullcmd);
static void reap_children(void);
static void handle_pending_children(void);

static void init_shell(void)
{
    shell_interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM)
        perror("setpgid");

    if (shell_interactive)
    {
        if (tcgetattr(STDIN_FILENO, &shell_tmodes) < 0)
            perror("tcgetattr");
        safe_tcsetpgrp(shell_pgid);
        setvbuf(stdout, NULL, _IONBF, 0);
        install_handler(SIGCHLD, sigchld_handler);
        install_handler(SIGINT, sigint_handler);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
    }
    else
    {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
    }

    scheduler_init();
    scheduler_register_executor(shell_register_job);
}

static char *trim(char *s)
{
    while (*s && (*s == ' ' || *s == '\t'))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t'))
        *end-- = '\0';
    return s;
}

static char *read_line(void)
{
    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    while (1)
    {
        errno = 0;
        nread = getline(&line, &len, stdin);
        if (nread >= 0)
            break;
        if (errno == EINTR)
        {
            if (sigint_received)
            {
                sigint_received = 0;
                free(line);
                line = strdup("");
                return line;
            }
            clearerr(stdin);
            continue;
        }
        if (feof(stdin) || errno == EIO)
        {
            free(line);
            return NULL;
        }
        clearerr(stdin);
    }
    if (nread > 0 && line[nread - 1] == '\n')
        line[nread - 1] = '\0';
    return line;
}

static char *read_line_interactive(void)
{
    size_t cap = 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;

    while (1)
    {
        if (sigint_received)
        {
            sigint_received = 0;
            buf[0] = '\0';
            return buf;
        }

        handle_pending_children();

        int timeout = scheduler_next_due();
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv;
        struct timeval *tvp = NULL;
        if (timeout >= 0)
        {
            tv.tv_sec = timeout;
            tv.tv_usec = 0;
            tvp = &tv;
        }

        int ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, tvp);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                handle_pending_children();
                continue;
            }
            free(buf);
            return NULL;
        }

        if (ready == 0)
        {
            scheduler_check_pending();
            handle_pending_children();
            continue;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds))
        {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0)
            {
                if (n == 0 || errno == EIO)
                {
                    free(buf);
                    return NULL;
                }
                if (errno == EINTR)
                    continue;
                free(buf);
                return NULL;
            }
            if (c == '\n')
                break;
            if (len + 1 >= cap)
            {
                cap *= 2;
                char *new_buf = realloc(buf, cap);
                if (!new_buf)
                {
                    free(buf);
                    return NULL;
                }
                buf = new_buf;
            }
            buf[len++] = c;
        }
    }

    buf[len] = '\0';
    return buf;
}

static char **split_unquoted(const char *s, char delim, int *count)
{
    size_t cap = 8;
    char **arr = calloc(cap, sizeof(char *));
    size_t n = 0;
    const char *p = s;
    const char *start = p;
    int in_sq = 0, in_dq = 0, esc = 0;
    while (*p)
    {
        char c = *p;
        if (!esc && c == '\\')
        {
            esc = 1;
            p++;
            continue;
        }
        if (!esc && c == '\'' && !in_dq)
        {
            in_sq = !in_sq;
        }
        else if (!esc && c == '"' && !in_sq)
        {
            in_dq = !in_dq;
        }
        else if (!in_sq && !in_dq && c == delim)
        {
            size_t seglen = p - start;
            char *seg = strndup(start, seglen);
            char *trimmed = trim(seg);
            arr[n++] = strdup(trimmed);
            free(seg);
            if (n + 1 >= cap)
            {
                cap *= 2;
                arr = realloc(arr, cap * sizeof(char *));
            }
            p++;
            start = p;
            esc = 0;
            continue;
        }
        esc = 0;
        p++;
    }
    if (p != start)
    {
        char *seg = strndup(start, p - start);
        char *trimmed = trim(seg);
        arr[n++] = strdup(trimmed);
        free(seg);
    }
    arr[n] = NULL;
    if (count)
        *count = n;
    return arr;
}

static void free_tokens(token_t *toks, int n)
{
    for (int i = 0; i < n; ++i)
        free(toks[i].text);
    free(toks);
}

static char *expand_env_vars(const char *s)
{
    size_t cap = strlen(s) + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    size_t oi = 0;
    for (size_t i = 0; s[i];)
    {
        if (s[i] == '$')
        {
            if (s[i + 1] == '{')
            {
                size_t j = i + 2;
                while (s[j] && s[j] != '}')
                    j++;
                if (s[j] == '}')
                {
                    char *name = strndup(s + i + 2, j - (i + 2));
                    char *val = getenv(name);
                    free(name);
                    if (val)
                    {
                        size_t vl = strlen(val);
                        if (oi + vl + 1 > cap)
                        {
                            cap = (oi + vl + 1) * 2;
                            out = realloc(out, cap);
                        }
                        memcpy(out + oi, val, vl);
                        oi += vl;
                    }
                    i = j + 1;
                    continue;
                }
            }
            size_t j = i + 1;
            if (s[j] == '_' || isalpha((unsigned char)s[j]))
            {
                j++;
                while (s[j] && (s[j] == '_' || isalnum((unsigned char)s[j])))
                    j++;
            }
            if (j > i + 1)
            {
                char *name = strndup(s + i + 1, j - (i + 1));
                char *val = getenv(name);
                free(name);
                if (val)
                {
                    size_t vl = strlen(val);
                    if (oi + vl + 1 > cap)
                    {
                        cap = (oi + vl + 1) * 2;
                        out = realloc(out, cap);
                    }
                    memcpy(out + oi, val, vl);
                    oi += vl;
                }
                i = j;
                continue;
            }
            if (oi + 2 > cap)
            {
                cap *= 2;
                out = realloc(out, cap);
            }
            out[oi++] = '$';
            i++;
            continue;
        }
        if (oi + 2 > cap)
        {
            cap *= 2;
            out = realloc(out, cap);
        }
        out[oi++] = s[i++];
    }
    out[oi] = '\0';
    return out;
}

static token_t *tokenize(const char *s, int *ntokens)
{
    size_t cap = 16;
    token_t *toks = calloc(cap, sizeof(token_t));
    int n = 0;
    const char *p = s;
    char buf[4096];
    size_t bi = 0;
    int in_sq = 0, in_dq = 0, esc = 0;
    while (*p)
    {
        char c = *p;
        if (!in_sq && !in_dq && (c == ' ' || c == '\t'))
        {
            if (bi > 0)
            {
                buf[bi] = '\0';
                toks[n].text = strdup(buf);
                toks[n].no_expand = 0;
                n++;
                bi = 0;
            }
            p++;
            continue;
        }
        if (!esc && c == '\\')
        {
            esc = 1;
            p++;
            continue;
        }
        if (!esc && c == '\'' && !in_dq)
        {
            in_sq = !in_sq;
            p++;
            continue;
        }
        if (!esc && c == '"' && !in_sq)
        {
            in_dq = !in_dq;
            p++;
            continue;
        }
        if (!in_sq && !in_dq && (c == '<' || c == '>' || c == '|' || c == '&'))
        {
            if (bi > 0)
            {
                buf[bi] = '\0';
                toks[n].text = strdup(buf);
                toks[n].no_expand = 0;
                n++;
                bi = 0;
            }
            if (c == '>' && p[1] == '>')
            {
                toks[n].text = strdup(">>");
                toks[n].no_expand = 0;
                n++;
                p += 2;
                continue;
            }
            char op[3] = {c, '\0', '\0'};
            toks[n].text = strdup(op);
            toks[n].no_expand = 0;
            n++;
            p++;
            continue;
        }
        if (esc)
        {
            buf[bi++] = c;
            esc = 0;
            p++;
            continue;
        }
        buf[bi++] = c;
        p++;
        if (bi >= sizeof(buf) - 2)
            break;
    }
    if (bi > 0)
    {
        buf[bi] = '\0';
        toks[n].text = strdup(buf);
        toks[n].no_expand = 0;
        n++;
    }
    *ntokens = n;
    return toks;
}

static void free_cmd(cmd_t *c)
{
    if (!c)
        return;
    if (c->argv)
    {
        for (char **p = c->argv; *p; ++p)
            free(*p);
        free(c->argv);
    }
    free(c->infile);
    free(c->outfile);
}

static cmd_t *parse_command(const char *s)
{
    cmd_t *c = calloc(1, sizeof(cmd_t));
    if (!c)
        return NULL;
    int ntoks = 0;
    token_t *toks = tokenize(s, &ntoks);
    size_t cap = 8;
    size_t n = 0;
    c->argv = calloc(cap, sizeof(char *));
    for (int i = 0; i < ntoks; ++i)
    {
        char *t = toks[i].text;
        if (strcmp(t, "<") == 0)
        {
            if (i + 1 < ntoks)
            {
                c->infile = strdup(toks[i + 1].text);
                i++;
            }
        }
        else if (strcmp(t, ">") == 0)
        {
            if (i + 1 < ntoks)
            {
                c->outfile = strdup(toks[i + 1].text);
                c->append = 0;
                i++;
            }
        }
        else if (strcmp(t, ">>") == 0)
        {
            if (i + 1 < ntoks)
            {
                c->outfile = strdup(toks[i + 1].text);
                c->append = 1;
                i++;
            }
        }
        else
        {
            char *expanded = expand_env_vars(t);
            if (n + 1 >= cap)
            {
                cap *= 2;
                c->argv = realloc(c->argv, cap * sizeof(char *));
            }
            c->argv[n++] = expanded;
        }
    }
    c->argv[n] = NULL;
    free_tokens(toks, ntoks);
    return c;
}

static void execute_line(const char *line, int default_background)
{
    char *copy = strdup(line);
    if (!copy)
        return;
    char *trimmed = trim(copy);
    if (trimmed[0] == '\0')
    {
        free(copy);
        return;
    }

    int seq_count = 0;
    char **seqs = split_unquoted(trimmed, ';', &seq_count);
    for (int si = 0; si < seq_count; ++si)
    {
        char *seq = seqs[si];
        int background = default_background;
        size_t L = strlen(seq);
        while (L > 0 && (seq[L - 1] == ' ' || seq[L - 1] == '\t'))
            seq[--L] = '\0';
        if (L > 0 && seq[L - 1] == '&')
        {
            background = 1;
            seq[--L] = '\0';
        }

        int npipe = 0;
        char **pipe_parts = split_unquoted(seq, '|', &npipe);
        if (npipe == 0)
        {
            free(pipe_parts);
            continue;
        }

        if (npipe == 1)
        {
            cmd_t *c = parse_command(pipe_parts[0]);
            if (c->argv && c->argv[0] && is_builtin_cmd(c->argv))
            {
                do_builtin(c->argv);
                free_cmd(c);
                free(c);
            }
            else
            {
                cmd_t **cmds = calloc(1, sizeof(cmd_t *));
                cmds[0] = c;
                launch_pipeline(cmds, 1, background, seq);
                free_cmd(c);
                free(cmds);
            }
        }
        else
        {
            cmd_t **cmds = calloc(npipe, sizeof(cmd_t *));
            for (int i = 0; i < npipe; ++i)
                cmds[i] = parse_command(pipe_parts[i]);
            launch_pipeline(cmds, npipe, background, seq);
            for (int i = 0; i < npipe; ++i)
            {
                free_cmd(cmds[i]);
                free(cmds[i]);
            }
            free(cmds);
        }

        for (int i = 0; i < npipe; ++i)
            free(pipe_parts[i]);
        free(pipe_parts);
    }
    for (int i = 0; i < seq_count; ++i)
        free(seqs[i]);
    free(seqs);
    free(copy);
}

static void shell_register_job(const char *cmdline)
{
    execute_line(cmdline, 1);
}

static int is_builtin_cmd(char **argv)
{
    if (!argv || !argv[0])
        return 0;
    return (strcmp(argv[0], "cd") == 0 || strcmp(argv[0], "exit") == 0 ||
            strcmp(argv[0], "jobs") == 0 || strcmp(argv[0], "fg") == 0 ||
            strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "schedule") == 0);
}

static int do_builtin(char **argv)
{
    if (strcmp(argv[0], "cd") == 0)
    {
        if (!argv[1])
        {
            char *home = getenv("HOME");
            if (home)
                chdir(home);
        }
        else
        {
            if (chdir(argv[1]) < 0)
                perror("cd");
        }
        return 1;
    }
    if (strcmp(argv[0], "exit") == 0)
    {
        exit(0);
    }
    if (strcmp(argv[0], "jobs") == 0)
    {
        print_jobs();
        return 1;
    }
    if (strcmp(argv[0], "schedule") == 0)
    {
        if (!argv[1] || !argv[2])
        {
            fprintf(stderr, "schedule: usage: schedule <seconds> <command>\n");
            return 1;
        }
        int delay = atoi(argv[1]);
        if (delay < 0)
            delay = 0;
        char cmdline[1024] = {0};
        for (int i = 2; argv[i]; ++i)
        {
            strcat(cmdline, argv[i]);
            if (argv[i + 1])
                strcat(cmdline, " ");
        }
        int jid = scheduler_add_job(delay, cmdline);
        printf("Scheduled job [%d] to run in %d seconds: %s\n", jid, delay, cmdline);
        return 1;
    }
    if (strcmp(argv[0], "fg") == 0)
    {
        if (!argv[1])
        {
            fprintf(stderr, "fg: usage: fg %%jobid\n");
            return 1;
        }
        int jid = atoi(argv[1] + (argv[1][0] == '%' ? 1 : 0));
        job_t *j = find_job_by_jid(jid);
        if (!j)
        {
            fprintf(stderr, "fg: no such job\n");
            return 1;
        }
        pid_t pgid = j->pgid;
        kill(-pgid, SIGCONT);
        j->state = JOB_RUNNING;
        safe_tcsetpgrp(pgid);

        int status;
        int stopped = 0;
        while (j->nrunning > 0)
        {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);
            if (w == -1)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (WIFEXITED(status) || WIFSIGNALED(status))
            {
                j->nrunning--;
            }
            else if (WIFSTOPPED(status))
            {
                stopped = 1;
                break;
            }
        }
        safe_tcsetpgrp(shell_pgid);

        if (stopped)
        {
            j->state = JOB_STOPPED;
            printf("\n[%d]+ Stopped    %s\n", j->jid, j->cmdline);
        }
        else if (j->nrunning <= 0)
        {
            printf("\n[%d]+ Done    %s\n", j->jid, j->cmdline);
            remove_job(j);
        }
        return 1;
    }
    if (strcmp(argv[0], "bg") == 0)
    {
        if (!argv[1])
        {
            fprintf(stderr, "bg: usage: bg %%jobid\n");
            return 1;
        }
        int jid = atoi(argv[1] + (argv[1][0] == '%' ? 1 : 0));
        job_t *j = find_job_by_jid(jid);
        if (!j)
        {
            fprintf(stderr, "bg: no such job\n");
            return 1;
        }
        kill(-j->pgid, SIGCONT);
        j->state = JOB_RUNNING;
        return 1;
    }
    return 0;
}

static void launch_pipeline(cmd_t **cmds, int ncmds, int background, const char *fullcmd)
{
    int in_fd = -1;
    int pipefd[2];
    pid_t pgid = 0;
    pid_t *pids = calloc((size_t)ncmds, sizeof(pid_t));

    for (int i = 0; i < ncmds; ++i)
    {
        if (i < ncmds - 1)
        {
            if (pipe(pipefd) < 0)
            {
                perror("pipe");
                free(pids);
                return;
            }
        }
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            free(pids);
            return;
        }
        if (pid == 0)
        {
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            if (pgid == 0)
                pgid = getpid();
            if (setpgid(0, pgid) < 0 && errno != EPERM)
                perror("setpgid");
            if (!background && shell_interactive)
                safe_tcsetpgrp(pgid);
            if (in_fd != -1)
            {
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }
            if (i < ncmds - 1)
            {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            if (cmds[i]->infile)
            {
                int fd = open(cmds[i]->infile, O_RDONLY);
                if (fd < 0)
                {
                    perror(cmds[i]->infile);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (cmds[i]->outfile)
            {
                int fd;
                if (cmds[i]->append)
                    fd = open(cmds[i]->outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                else
                    fd = open(cmds[i]->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0)
                {
                    perror(cmds[i]->outfile);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            if (!cmds[i]->argv || !cmds[i]->argv[0])
                _exit(0);
            execvp(cmds[i]->argv[0], cmds[i]->argv);
            perror("exec");
            _exit(127);
        }
        else
        {
            if (pgid == 0)
                pgid = pid;
            pids[i] = pid;
            if (setpgid(pid, pgid) < 0 && errno != EPERM && errno != EACCES)
                perror("setpgid");
            if (in_fd != -1)
                close(in_fd);
            if (i < ncmds - 1)
            {
                close(pipefd[1]);
                in_fd = pipefd[0];
            }
        }
    }

    if (background)
    {
        add_job(pgid, fullcmd, JOB_RUNNING, pids, ncmds);
        printf("[%d] %d\n", next_jid - 1, pgid);
    }
    else
    {
        if (shell_interactive)
            safe_tcsetpgrp(pgid);
        int status;
        pid_t w;
        do
        {
            w = waitpid(-pgid, &status, WUNTRACED);
            if (w == -1 && errno != EINTR)
                break;
        } while (!WIFEXITED(status) && !WIFSIGNALED(status) && !WIFSTOPPED(status));
        if (WIFSTOPPED(status))
        {
            add_job(pgid, fullcmd, JOB_STOPPED, pids, ncmds);
            printf("\n[%d]+ Stopped    %s\n", next_jid - 1, fullcmd);
        }
        if (shell_interactive)
            safe_tcsetpgrp(shell_pgid);
    }

    free(pids); /* add_job() copies pids internally, so safe to free here */
}

static void reap_children(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
    {
        job_t *j = find_job_by_pid(pid);
        if (!j)
            continue;
        if (WIFEXITED(status) || WIFSIGNALED(status))
        {
            j->nrunning--;
            if (j->nrunning <= 0)
            {
                j->state = JOB_DONE;
                printf("\n[%d]+ Done    %s\n", j->jid, j->cmdline);
                remove_job(j);
            }
        }
        else if (WIFSTOPPED(status))
        {
            j->state = JOB_STOPPED;
            printf("\n[%d]+ Stopped    %s\n", j->jid, j->cmdline);
        }
        else if (WIFCONTINUED(status))
        {
            j->state = JOB_RUNNING;
        }
    }
}

static void handle_pending_children(void)
{
    if (sigchld_pending)
    {
        sigchld_pending = 0;
        reap_children();
    }
}

void shell_loop(void)
{
    init_shell();
    for (;;)
    {
        if (shell_interactive)
        {
            scheduler_check_pending();
            handle_pending_children();
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd)) != NULL)
                printf("%s$ ", cwd);
            else
                printf("$ ");
            fflush(stdout);
        }

        char *line = shell_interactive ? read_line_interactive() : read_line();
        if (!line)
        {
            printf("\n");
            break;
        }

        char *trimmed = trim(line);
        if (trimmed[0] == '\0')
        {
            free(line);
            continue;
        }

        scheduler_check_pending();
        handle_pending_children();

        int seq_count = 0;
        char **seqs = split_unquoted(trimmed, ';', &seq_count);
        for (int si = 0; si < seq_count; ++si)
        {
            char *seq = seqs[si];
            int background = 0;
            size_t L = strlen(seq);
            while (L > 0 && (seq[L - 1] == ' ' || seq[L - 1] == '\t'))
                seq[--L] = '\0';
            if (L > 0 && seq[L - 1] == '&')
            {
                background = 1;
                seq[--L] = '\0';
            }

            int npipe = 0;
            char **pipe_parts = split_unquoted(seq, '|', &npipe);
            if (npipe == 0)
            {
                free(pipe_parts);
                continue;
            }

            if (npipe == 1)
            {
                cmd_t *c = parse_command(pipe_parts[0]);
                if (c && c->argv && c->argv[0] && is_builtin_cmd(c->argv))
                {
                    do_builtin(c->argv);
                    free_cmd(c);
                    free(c);
                }
                else if (c)
                {
                    cmd_t **cmds = calloc(1, sizeof(cmd_t *));
                    cmds[0] = c;
                    launch_pipeline(cmds, 1, background, seq);
                    free_cmd(c);
                    free(cmds);
                }
            }
            else
            {
                cmd_t **cmds = calloc(npipe, sizeof(cmd_t *));
                for (int i = 0; i < npipe; ++i)
                    cmds[i] = parse_command(pipe_parts[i]);
                launch_pipeline(cmds, npipe, background, seq);
                for (int i = 0; i < npipe; ++i)
                {
                    free_cmd(cmds[i]);
                    free(cmds[i]);
                }
                free(cmds);
            }

            for (int i = 0; i < npipe; ++i)
                free(pipe_parts[i]);
            free(pipe_parts);
        }
        for (int i = 0; i < seq_count; ++i)
            free(seqs[i]);
        free(seqs);
        free(line);
    }
}