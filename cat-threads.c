#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void *run_cat(void *arg) {
    int id = *(int *)arg;

    unsigned int seed = (unsigned)((uintptr_t)pthread_self()
                         ^ (uintptr_t)getpid()
                         ^ (uintptr_t)(id * 2654435761u)
                         ^ (uintptr_t)time(NULL));

    is_parent = 0;
    srand(seed);

    log_event("CAT", id, "born");

    while (!S->stop) {
        int nap = rand_duration_ms(S->N);
        log_event("CAT", id, "napping for %d ms", nap);
        interruptible_sleep_ms(nap);
        if (S->stop) break;

        lock();
        S->active_cats++;
        log_event("CAT", id,
                  "hungry (active_cats=%d, feeding_mice=%d)",
                  S->active_cats, S->feeding_mice);

        int bowl;
        while ((bowl = find_free_bowl()) < 0 && !S->stop) {
            log_event("CAT", id, "all bowls busy, waiting");
            wait_bowl_free();
        }

        if (S->stop) {
            S->active_cats--;
            if (S->active_cats == 0) broadcast_no_cats();
            unlock();
            break;
        }

        S->bowls[bowl] = id;
        log_event("CAT", id, "feeding at bowl %d", bowl);
        unlock();

        int feed = rand_duration_ms(S->F);
        interruptible_sleep_ms(feed);

        lock();
        if (S->bowls[bowl] == id) {
            S->bowls[bowl] = 0;
        }
        S->active_cats--;
        log_event("CAT", id,
                  "done eating, bowl %d free (active_cats=%d)",
                  bowl, S->active_cats);

        broadcast_bowl_free();
        if (S->active_cats == 0) {
            broadcast_no_cats();
        }
        unlock();
    }

    log_event("CAT", id, "exiting");
    return NULL;
}

static void *run_mouse(void *arg) {
    int id = *(int *)arg;

    unsigned int seed = (unsigned)((uintptr_t)pthread_self()
                         ^ (uintptr_t)getpid()
                         ^ (uintptr_t)(id * 2246822519u)
                         ^ (uintptr_t)time(NULL)
                         ^ 0xA5A5u);

    is_parent = 0;
    srand(seed);

    log_event("MOUSE", id, "born");

    const int TICK_MS = 100;

    while (!S->stop) {
        int nap = rand_duration_ms(S->Nm);
        log_event("MOUSE", id, "napping for %d ms", nap);
        interruptible_sleep_ms(nap);
        if (S->stop) break;

        lock();
        log_event("MOUSE", id,
                  "hungry (active_cats=%d, feeding_mice=%d)",
                  S->active_cats, S->feeding_mice);

        int bowl = -1;

        while (!S->stop) {
            while (S->active_cats > 0 && !S->stop) {
                log_event("MOUSE", id, "cats in sight, waiting");
                wait_no_cats();
            }
            if (S->stop) break;

            bowl = find_free_bowl();
            if (bowl < 0) {
                log_event("MOUSE", id,
                          "all bowls full, waiting for one to free");
                wait_bowl_free();
                continue;
            }

            S->bowls[bowl] = -id;
            S->feeding_mice++;
            log_event("MOUSE", id,
                      "feeding at bowl %d (feeding_mice=%d)",
                      bowl, S->feeding_mice);
            break;
        }

        unlock();
        if (S->stop) break;
        if (bowl < 0) continue;

        int total_ms = rand_duration_ms(S->Fm);
        int elapsed = 0;
        int fled = 0;

        while (elapsed < total_ms && !S->stop) {
            int chunk = (total_ms - elapsed) > TICK_MS ? TICK_MS : (total_ms - elapsed);
            struct timespec req = { 0, (long)chunk * 1000000L };
            (void)nanosleep(&req, NULL);
            elapsed += chunk;

            lock();
            if (S->active_cats > 0) {
                log_event("MOUSE", id,
                          "FLEES from bowl %d (cat in sight, ate %d ms)",
                          bowl, elapsed);

                if (S->bowls[bowl] == -id) {
                    S->bowls[bowl] = 0;
                    S->feeding_mice--;
                }

                broadcast_bowl_free();
                unlock();
                fled = 1;
                break;
            }
            unlock();
        }

        if (!fled) {
            lock();
            if (S->bowls[bowl] == -id) {
                S->bowls[bowl] = 0;
                S->feeding_mice--;
                log_event("MOUSE", id,
                          "satisfied, bowl %d free (ate %d ms)",
                          bowl, elapsed);
                broadcast_bowl_free();
            }
            unlock();
        }
    }

    log_event("MOUSE", id, "exiting");
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [B C M F N Fm Nm T]\n"
        "  B  = bowls                              (default 3)\n"
        "  C  = cats                               (default 4)\n"
        "  M  = mice                               (default 5)\n"
        "  F  = cat feeding base seconds           (default 2)\n"
        "  N  = cat not-hungry base seconds        (default 3)\n"
        "  Fm = mouse max feed base seconds        (default 2)\n"
        "  Nm = mouse not-hungry base seconds      (default 2)\n"
        "  T  = total simulation seconds           (default 30)\n"
        "Durations are randomized within [base/2, 3*base/2].\n",
        prog);
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);

    cfg.B = 3;
    cfg.C = 4;
    cfg.M = 5;
    cfg.F = 2;
    cfg.N = 3;
    cfg.Fm = 2;
    cfg.Nm = 2;
    cfg.T = 30;

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(argv[0]);
        return 0;
    }

    int *fields[] = {
        &cfg.B, &cfg.C, &cfg.M, &cfg.F, &cfg.N, &cfg.Fm, &cfg.Nm, &cfg.T
    };

    for (int i = 1; i < argc && (i - 1) < 8; i++) {
        *fields[i - 1] = atoi(argv[i]);
    }

    if (cfg.B < 1 || cfg.B > MAX_BOWLS) {
        fprintf(stderr, "B must be between 1 and %d\n", MAX_BOWLS);
        return 1;
    }
    if (cfg.C < 0 || cfg.M < 0 || cfg.T < 1) {
        fprintf(stderr, "Invalid parameters (C>=0, M>=0, T>=1)\n");
        return 1;
    }
    if (cfg.C + cfg.M == 0) {
        fprintf(stderr, "Nothing to simulate (C=0 and M=0)\n");
        return 1;
    }

    S = calloc(1, sizeof(*S));
    if (!S) die("calloc");

    S->B  = cfg.B;
    S->C  = cfg.C;
    S->M  = cfg.M;
    S->F  = cfg.F;
    S->N  = cfg.N;
    S->Fm = cfg.Fm;
    S->Nm = cfg.Nm;

    for (int i = 0; i < MAX_BOWLS; i++) {
        S->bowls[i] = 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &S->t0);

    log_event("SIM", -1,
              "starting  B=%d C=%d M=%d F=%d N=%d Fm=%d Nm=%d T=%d",
              cfg.B, cfg.C, cfg.M, cfg.F, cfg.N, cfg.Fm, cfg.Nm, cfg.T);

    int total = cfg.C + cfg.M;
    pthread_t *threads = calloc((size_t)total, sizeof(pthread_t));
    int *ids = calloc((size_t)total, sizeof(int));
    if (!threads || !ids) die("calloc");

    int idx = 0;
    for (int i = 1; i <= cfg.C; i++) {
        ids[idx] = i;
        if (pthread_create(&threads[idx], NULL, run_cat, &ids[idx]) != 0) {
            die("pthread_create(cat)");
        }
        idx++;
    }
    for (int i = 1; i <= cfg.M; i++) {
        ids[idx] = i;
        if (pthread_create(&threads[idx], NULL, run_mouse, &ids[idx]) != 0) {
            die("pthread_create(mouse)");
        }
        idx++;
    }

    sleep((unsigned)cfg.T);

    lock();
    S->stop = 1;
    broadcast_no_cats();
    broadcast_bowl_free();
    unlock();

    log_event("SIM", -1, "stopping simulation, waking threads...");

    for (int i = 0; i < total; i++) {
        pthread_join(threads[i], NULL);
    }

    log_event("SIM", -1, "all threads joined, cleaning up");

    free(threads);
    free(ids);
    free(S);
    return 0;
}