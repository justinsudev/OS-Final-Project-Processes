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


/* ---- Configuration ------------------------------------------------------- */

typedef struct {
    int B;   /* bowls */
    int C;   /* cats */
    int M;   /* mice */
    int F;   /* cat feeding base seconds */
    int N;   /* cat not-hungry base seconds */
    int Fm;  /* mouse max feed base seconds */
    int Nm;  /* mouse not-hungry base seconds */
    int T;   /* total simulation seconds */
} Config;