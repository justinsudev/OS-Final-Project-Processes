#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_BOWLS 64

typedef struct {
    int B;
    int C;
    int M;
    int F;
    int N;
    int Fm;
    int Nm;
    int T;
} Config;

typedef struct {
    int B, C, M;
    int F, N, Fm, Nm;
    int bowls[MAX_BOWLS];
    int active_cats;
    int feeding_mice;
    int waiting_for_bowl;
    int waiting_mice_for_no_cats;
    volatile sig_atomic_t stop;
    struct timespec t0;
} SharedState;

static SharedState *S = NULL;
static sem_t *sem_mutex     = SEM_FAILED;
static sem_t *sem_no_cats   = SEM_FAILED;
static sem_t *sem_bowl_free = SEM_FAILED;

static char name_shm[64];
static char name_mutex[64];
static char name_no_cats[64];
static char name_bowl_free[64];

static int is_parent = 1;  

static void die(const char *what) {
    fprintf(stderr, "FATAL: %s: %s\n", what, strerror(errno));
    exit(1);
}

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double s  = (double)(t.tv_sec  - S->t0.tv_sec);
    double ns = (double)(t.tv_nsec - S->t0.tv_nsec);
    return s + ns / 1e9;
}

static void log_event(const char *role, int id, const char *fmt, ...) {
    char prefix[64];
    if (id >= 0) snprintf(prefix, sizeof prefix, "[%s %d]", role, id);
    else         snprintf(prefix, sizeof prefix, "[%s]",    role);

    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    char line[640];
    int n = snprintf(line, sizeof line,
                     "[t=%7.3f] %-12s %s\n",
                     now_seconds(), prefix, body);
    if (n > 0) {
        if (n > (int)sizeof line) n = (int)sizeof line;
        (void)!write(STDOUT_FILENO, line, (size_t)n);
    }
}

static sem_t *sem_open_create(const char *name, unsigned int value) {
    sem_unlink(name);
    sem_t *s = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if (s == SEM_FAILED) die("sem_open(create)");
    return s;
}

static sem_t *sem_open_attach(const char *name) {
    sem_t *s = sem_open(name, 0);
    if (s == SEM_FAILED) die("sem_open(attach)");
    return s;
}

static void LOCK(void)   {
    while (sem_wait(sem_mutex) == -1) {
        if (errno == EINTR) continue;
        die("sem_wait(mutex)");
    }
}

static void UNLOCK(void) {
    if (sem_post(sem_mutex) == -1) die("sem_post(mutex)");
}

static void cv_wait(sem_t *sem, int *waiters) {
    (*waiters)++;
    UNLOCK();
    while (sem_wait(sem) == -1) {
        if (errno == EINTR) continue;
        die("sem_wait(cv)");
    }
    LOCK();
}

static void cv_broadcast(sem_t *sem, int *waiters) {
    while (*waiters > 0) {
        if (sem_post(sem) == -1) die("sem_post(cv)");
        (*waiters)--;
    }
}

static void interruptible_sleep_ms(int ms) {
    const int slice = 50;
    while (ms > 0 && !S->stop) {
        int chunk = ms > slice ? slice : ms;
        struct timespec req = { chunk / 1000, (long)(chunk % 1000) * 1000000L };
        (void)nanosleep(&req, NULL);
        ms -= chunk;
    }
}

static int rand_duration_ms(int base_seconds) {
    if (base_seconds <= 0) return 0;
    int base_ms = base_seconds * 1000;
    int half    = base_ms / 2;
    int r       = rand() % (base_ms + 1);  
    return half + r;
}

static int find_free_bowl(void) {
    for (int i = 0; i < S->B; i++)
        if (S->bowls[i] == 0) return i;
    return -1;
}

static void child_reset_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
}