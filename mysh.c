/*
 * mysh.c - a small Unix shell
 *
 * Supports: pipelines (a | b | c), < > >> redirection, background jobs (&),
 * quotes ('...' and "..."), wildcards (*.c),
 * built-ins cd, pwd, jobs, exit, and Ctrl-C that stops the job, not the shell.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXARGS 64
#define MAXCMDS 16
#define MAXJOBS 32

typedef struct {
    char *argv[MAXARGS];
    char *in, *out;
    int append;
} Cmd;

static struct { pid_t pid; char line[128]; } jobs[MAXJOBS];
static int njobs;

static void reap(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) ;
    errno = saved;
}

/* every string of the current line lives here, so nothing needs freeing */
static char arena[16384];
static size_t used;

static char *keep(const char *s, size_t len)
{
    if (used + len + 1 > sizeof arena) return NULL;
    char *p = memcpy(arena + used, s, len);
    p[len] = '\0';
    used += len + 1;
    return p;
}

#define MAXTOK 256
#define OP 1      /* token is an operator: | < > >> & */
#define QUOTED 2  /* token had quotes, so no wildcard expansion */

/* split a line into words and operators, honouring '...' and "..." quotes */
static int tokenize(const char *s, char **tok, int *flags)
{
    int nt = 0;
    char word[1024];
    while (*s && nt < MAXTOK) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        if (strchr("|<>&", *s)) {
            int len = (s[0] == '>' && s[1] == '>') ? 2 : 1;
            flags[nt] = OP;
            tok[nt++] = keep(s, len);
            s += len;
            continue;
        }
        size_t w = 0;
        int quoted = 0;
        while (*s && !strchr(" \t\n|<>&", *s) && w < sizeof word - 1) {
            if (*s == '\'' || *s == '"') {
                char q = *s++;
                quoted = 1;
                while (*s && *s != q && w < sizeof word - 1) word[w++] = *s++;
                if (*s != q) return -1;          /* unterminated quote */
                s++;
            } else {
                word[w++] = *s++;
            }
        }
        flags[nt] = quoted ? QUOTED : 0;
        tok[nt++] = keep(word, w);
    }
    return nt;
}

static void add_arg(Cmd *c, int *na, const char *word, int flags)
{
    glob_t g;
    if (!(flags & QUOTED) && strpbrk(word, "*?[") && glob(word, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && *na < MAXARGS - 1; i++)
            c->argv[(*na)++] = keep(g.gl_pathv[i], strlen(g.gl_pathv[i]));
        globfree(&g);
        return;
    }
    if (*na < MAXARGS - 1) c->argv[(*na)++] = (char *)word;   /* no match: pass it literally, like bash */
}

/* split "ls *.c | grep 'x y' > out.txt &" into commands; returns count, -1 on syntax error */
static int parse(const char *line, Cmd *cmds, int *bg)
{
    char *tok[MAXTOK];
    int flags[MAXTOK], nc = 0, na = 0;
    used = 0;
    *bg = 0;
    memset(cmds, 0, sizeof(Cmd) * MAXCMDS);

    int nt = tokenize(line, tok, flags);
    if (nt < 0) return -1;
    for (int i = 0; i < nt; i++) {
        Cmd *c = &cmds[nc];
        if (!tok[i]) return -1;
        if (!(flags[i] & OP)) { add_arg(c, &na, tok[i], flags[i]); continue; }

        if (!strcmp(tok[i], "|")) {
            if (na == 0 || ++nc == MAXCMDS) return -1;
            na = 0;
        } else if (!strcmp(tok[i], "&")) {
            *bg = 1;
        } else {                                  /* < > >> need a file name next */
            if (i + 1 >= nt || (flags[i + 1] & OP)) return -1;
            if (!strcmp(tok[i], "<")) c->in = tok[++i];
            else { c->append = !strcmp(tok[i], ">>"); c->out = tok[++i]; }
        }
    }
    if (na == 0) return nc == 0 ? 0 : -1;
    return nc + 1;
}

static int builtin(Cmd *c)
{
    char **a = c->argv;
    if (!strcmp(a[0], "exit")) exit(0);
    if (!strcmp(a[0], "cd")) {
        if (chdir(a[1] ? a[1] : getenv("HOME")) < 0) perror("cd");
        return 1;
    }
    if (!strcmp(a[0], "pwd")) {
        char buf[512];
        puts(getcwd(buf, sizeof buf) ? buf : "?");
        return 1;
    }
    if (!strcmp(a[0], "jobs")) {
        for (int i = 0; i < njobs; i++) {
            int alive = kill(jobs[i].pid, 0) == 0 && waitpid(jobs[i].pid, NULL, WNOHANG) == 0;
            printf("[%d] %d  %-8s %s\n", i + 1, jobs[i].pid, alive ? "Running" : "Done", jobs[i].line);
        }
        return 1;
    }
    return 0;
}

static void execute(Cmd *cmds, int nc, int bg, const char *line)
{
    int prev = -1, fd[2];
    pid_t pids[MAXCMDS];

    for (int i = 0; i < nc; i++) {
        if (i < nc - 1 && pipe(fd) < 0) { perror("pipe"); return; }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return; }
        if (pid == 0) {
            signal(SIGINT, bg ? SIG_IGN : SIG_DFL);
            if (prev != -1) { dup2(prev, STDIN_FILENO); close(prev); }
            if (i < nc - 1) { close(fd[0]); dup2(fd[1], STDOUT_FILENO); close(fd[1]); }
            if (cmds[i].in) {
                int f = open(cmds[i].in, O_RDONLY);
                if (f < 0) { perror(cmds[i].in); _exit(1); }
                dup2(f, STDIN_FILENO); close(f);
            }
            if (cmds[i].out) {
                int f = open(cmds[i].out, O_WRONLY | O_CREAT | (cmds[i].append ? O_APPEND : O_TRUNC), 0644);
                if (f < 0) { perror(cmds[i].out); _exit(1); }
                dup2(f, STDOUT_FILENO); close(f);
            }
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "mysh: %s: command not found\n", cmds[i].argv[0]);
            _exit(127);
        }
        pids[i] = pid;
        if (prev != -1) close(prev);
        if (i < nc - 1) { close(fd[1]); prev = fd[0]; }
    }

    if (bg) {
        if (njobs < MAXJOBS) {
            jobs[njobs].pid = pids[nc - 1];
            snprintf(jobs[njobs].line, sizeof jobs[njobs].line, "%.120s", line);
            njobs++;
        }
        printf("[%d] %d\n", njobs, pids[nc - 1]);
        return;
    }
    for (int i = 0; i < nc; i++) waitpid(pids[i], NULL, 0);
}

int main(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = reap;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGINT, SIG_IGN);             /* Ctrl-C goes to the foreground job only */

    char line[1024], copy[1024];
    Cmd cmds[MAXCMDS];
    int bg;

    for (;;) {
        printf("mysh> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }
        line[strcspn(line, "\n")] = '\0';
        snprintf(copy, sizeof copy, "%s", line);

        int nc = parse(line, cmds, &bg);
        if (nc < 0) { fprintf(stderr, "mysh: syntax error\n"); continue; }
        if (nc == 0) continue;
        if (nc == 1 && !bg && builtin(&cmds[0])) continue;
        execute(cmds, nc, bg, copy);
    }
    return 0;
}
