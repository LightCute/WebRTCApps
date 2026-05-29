// video_frame_shm_ctrl.cc — SHM sync init for multi-consumer control block
#include "apps/peerconnection/video_capture_shm_RGA/video_frame_shm_ctrl.h"

#include <cstring>

int video_frame_shm_init(ShmMultiCtrlBlock* shm) {
  memset(shm, 0, sizeof(*shm));

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
  if (pthread_condattr_init(&cv_attr) != 0) {
    pthread_mutex_destroy(&shm->mtx);
    return -1;
  }
  if (pthread_condattr_setpshared(&cv_attr, PTHREAD_PROCESS_SHARED) != 0) {
    pthread_condattr_destroy(&cv_attr);
    pthread_mutex_destroy(&shm->mtx);
    return -1;
  }
  if (pthread_cond_init(&shm->cv_can_write, &cv_attr) != 0) {
    pthread_condattr_destroy(&cv_attr);
    pthread_mutex_destroy(&shm->mtx);
    return -1;
  }
  if (pthread_cond_init(&shm->cv_can_read, &cv_attr) != 0) {
    pthread_condattr_destroy(&cv_attr);
    pthread_cond_destroy(&shm->cv_can_write);
    pthread_mutex_destroy(&shm->mtx);
    return -1;
  }
  pthread_condattr_destroy(&cv_attr);

  return 0;
}
