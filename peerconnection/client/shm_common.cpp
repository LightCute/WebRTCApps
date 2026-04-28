#include "shm_common.h"

void init_shm_sync(ShmCtrlBlock* shm)
{
    // 初始化锁属性：跨进程共享
    pthread_mutexattr_t mtx_attr;
    pthread_mutexattr_init(&mtx_attr);
    pthread_mutexattr_setpshared(&mtx_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->mtx, &mtx_attr);
    pthread_mutexattr_destroy(&mtx_attr);

    // 初始化条件变量属性：跨进程共享
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&shm->cv_can_write, &cond_attr);
    pthread_cond_init(&shm->cv_can_read, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
}