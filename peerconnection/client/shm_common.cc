#include "shm_common.h"

void init_shm_sync(ShmCtrlBlock* shm) {
  pthread_mutexattr_t mtx_attr;
  pthread_mutexattr_init(&mtx_attr);
  pthread_mutexattr_setpshared(&mtx_attr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(&shm->mtx, &mtx_attr);
  pthread_mutexattr_destroy(&mtx_attr);

  pthread_condattr_t cv_attr;
  pthread_condattr_init(&cv_attr);
  pthread_condattr_setpshared(&cv_attr, PTHREAD_PROCESS_SHARED);
  pthread_cond_init(&shm->cv_can_write, &cv_attr);
  pthread_cond_init(&shm->cv_can_read, &cv_attr);
  pthread_condattr_destroy(&cv_attr);

  shm->w_idx = 0;
  shm->r_idx = 0;
  shm->frame_count = 0;
}
