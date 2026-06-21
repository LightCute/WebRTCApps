#include "shm_common.h"

int init_shm_sync(ShmCtrlBlock* shm) {
  pthread_mutexattr_t mtx_attr;
  if (pthread_mutexattr_init(&mtx_attr) != 0) return -1;
  if (pthread_mutexattr_setpshared(&mtx_attr, PTHREAD_PROCESS_SHARED) != 0) {
    pthread_mutexattr_destroy(&mtx_attr);
    return -1;
  }
  if (pthread_mutex_init(&shm->mtx, &mtx_attr) != 0) {
    pthread_mutexattr_destroy(&mtx_attr);
    return -1;
  }
  pthread_mutexattr_destroy(&mtx_attr);

  pthread_condattr_t cv_attr;
  if (pthread_condattr_init(&cv_attr) != 0) return -1;
  if (pthread_condattr_setpshared(&cv_attr, PTHREAD_PROCESS_SHARED) != 0) {
    pthread_condattr_destroy(&cv_attr);
    return -1;
  }
  if (pthread_cond_init(&shm->cv_can_write, &cv_attr) != 0) {
    pthread_condattr_destroy(&cv_attr);
    return -1;
  }
  if (pthread_cond_init(&shm->cv_can_read, &cv_attr) != 0) {
    pthread_condattr_destroy(&cv_attr);
    return -1;
  }
  pthread_condattr_destroy(&cv_attr);

  shm->w_idx = 0;
  shm->r_idx = 0;
  shm->frame_count = 0;
  return 0;
}
