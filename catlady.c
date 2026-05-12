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

static void run_cat(int id) {
    is_parent = 0;
    child_reset_signals();

    sem_mutex     = sem_open_attach(name_mutex);
    sem_no_cats   = sem_open_attach(name_no_cats);
    sem_bowl_free = sem_open_attach(name_bowl_free);

    srand((unsigned)((uintptr_t)getpid() ^ (uintptr_t)(id * 2654435761u)
                     ^ (uintptr_t)time(NULL)));

    log_event("CAT", id, "born (pid=%d)", (int)getpid());

    while (!S->stop) {
        int nap = rand_duration_ms(S->N);
        log_event("CAT", id, "napping for %d ms", nap);
        interruptible_sleep_ms(nap);
        if (S->stop) break;

        LOCK();
        S->active_cats++;
        log_event("CAT", id,
                  "hungry (active_cats=%d, feeding_mice=%d)",
                  S->active_cats, S->feeding_mice);

        int bowl;
        while ((bowl = find_free_bowl()) < 0 && !S->stop) {
            log_event("CAT", id, "all bowls busy, waiting");
            cv_wait(sem_bowl_free, &S->waiting_for_bowl);
        }
        if (S->stop) {
            S->active_cats--;
            if (S->active_cats == 0)
                cv_broadcast(sem_no_cats, &S->waiting_mice_for_no_cats);
            UNLOCK();
            break;
        }

        S->bowls[bowl] = +id;
        log_event("CAT", id, "feeding at bowl %d", bowl);
        UNLOCK();

        int feed = rand_duration_ms(S->F);
        interruptible_sleep_ms(feed);

        LOCK();
        S->bowls[bowl] = 0;
        S->active_cats--;
        log_event("CAT", id,
                  "done eating, bowl %d free (active_cats=%d)",
                  bowl, S->active_cats);

        cv_broadcast(sem_bowl_free, &S->waiting_for_bowl);
        if (S->active_cats == 0)
            cv_broadcast(sem_no_cats, &S->waiting_mice_for_no_cats);
        UNLOCK();
    }

    log_event("CAT", id, "exiting");
    _exit(0);
}

static void run_mouse(int id) {
    is_parent = 0;
    child_reset_signals();
    sem_mutex     = sem_open_attach(name_mutex);
    sem_no_cats   = sem_open_attach(name_no_cats);
    sem_bowl_free = sem_open_attach(name_bowl_free);

    srand((unsigned)((uintptr_t)getpid() ^ (uintptr_t)(id * 2246822519u)
                     ^ (uintptr_t)time(NULL) ^ 0xA5A5u));

    log_event("MOUSE", id, "born (pid=%d)", (int)getpid());

    const int TICK_MS = 100;

    while (!S->stop) {
        int nap = rand_duration_ms(S->Nm);
        log_event("MOUSE", id, "napping for %d ms", nap);
        interruptible_sleep_ms(nap);
        if (S->stop) break;

        LOCK();
        log_event("MOUSE", id,
                  "hungry (active_cats=%d, feeding_mice=%d)",
                  S->active_cats, S->feeding_mice);

        int bowl     = -1;
        int acquired = 0;
        while (!acquired && !S->stop) {
            while (S->active_cats > 0 && !S->stop) {
                log_event("MOUSE", id, "cats in sight, waiting");
                cv_wait(sem_no_cats, &S->waiting_mice_for_no_cats);
            }
            if (S->stop) break;

            bowl = find_free_bowl();
            if (bowl < 0) {
                log_event("MOUSE", id,
                          "all bowls full of mice, waiting for one to leave");
                cv_wait(sem_bowl_free, &S->waiting_for_bowl);
                continue;
            }

            S->bowls[bowl] = -id;
            S->feeding_mice++;
            acquired = 1;
            log_event("MOUSE", id,
                      "feeding at bowl %d (feeding_mice=%d)",
                      bowl, S->feeding_mice);
        }
        UNLOCK();

        if (!acquired) break;

        int total_ms = rand_duration_ms(S->Fm);
        int elapsed  = 0;
        int fled     = 0;
        while (elapsed < total_ms && !S->stop) {
            int chunk = (total_ms - elapsed) > TICK_MS
                          ? TICK_MS : (total_ms - elapsed);
            struct timespec req = { 0, (long)chunk * 1000000L };
            (void)nanosleep(&req, NULL);
            elapsed += chunk;

            LOCK();
            if (S->active_cats > 0) {
                log_event("MOUSE", id,
                          "FLEES from bowl %d (cat in sight, ate %d ms)",
                          bowl, elapsed);
                S->bowls[bowl] = 0;
                S->feeding_mice--;
                cv_broadcast(sem_bowl_free, &S->waiting_for_bowl);
                UNLOCK();
                fled = 1;
                break;
            }
            UNLOCK();
        }

        if (!fled) {
            LOCK();
            if (S->bowls[bowl] == -id) {
                S->bowls[bowl] = 0;
                S->feeding_mice--;
                log_event("MOUSE", id,
                          "satisfied, bowl %d free (ate %d ms)",
                          bowl, elapsed);
                cv_broadcast(sem_bowl_free, &S->waiting_for_bowl);
            }
            UNLOCK();
        }
    }

    log_event("MOUSE", id, "exiting");
    _exit(0);
}