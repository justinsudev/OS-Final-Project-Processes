#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#define NUM_CATS   3
#define NUM_MICE   5
#define NUM_BOWLS  4
#define CYCLES     5

typedef enum {
    BOWL_FREE = 0,
    BOWL_CAT,
    BOWL_MOUSE
} BowlState;

typedef struct {
    BowlState state;
    int owner_id;
} Bowl;

static Bowl bowls[NUM_BOWLS];

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cats_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t mice_cv = PTHREAD_COND_INITIALIZER;