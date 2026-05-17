#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    volatile sig_atomic_t stop;
    struct timespec t0;
} SharedState;

static Config cfg;
static SharedState *S = NULL;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_no_cats = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cv_bowl_free = PTHREAD_COND_INITIALIZER;

static int is_parent = 1;

static void die(const char *what) {
    perror(what);
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

static int rand_duration_ms(int base_seconds) {
    if (base_seconds <= 0) return 0;
    int base_ms = base_seconds * 1000;
    int half    = base_ms / 2;
    int r       = rand() % (base_ms + 1);
    return half + r;
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

static int find_free_bowl(void) {
    for (int i = 0; i < S->B; i++) {
        if (S->bowls[i] == 0) return i;
    }
    return -1;
}

static void lock(void) {
    if (pthread_mutex_lock(&mtx) != 0) die("pthread_mutex_lock");
}

static void unlock(void) {
    if (pthread_mutex_unlock(&mtx) != 0) die("pthread_mutex_unlock");
}

static void wait_no_cats(void) {
    if (pthread_cond_wait(&cv_no_cats, &mtx) != 0) die("pthread_cond_wait");
}

static void wait_bowl_free(void) {
    if (pthread_cond_wait(&cv_bowl_free, &mtx) != 0) die("pthread_cond_wait");
}

static void broadcast_no_cats(void) {
    if (pthread_cond_broadcast(&cv_no_cats) != 0) die("pthread_cond_broadcast");
}

static void broadcast_bowl_free(void) {
    if (pthread_cond_broadcast(&cv_bowl_free) != 0) die("pthread_cond_broadcast");
}