#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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