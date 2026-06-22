// shm_video_source_interface.h — cross-platform SHM video frame source.
//
// Implementations:
//   Linux   — PosixShmVideoSource  (System V shm: ftok/shmat)
//   Windows — WinShmVideoSource    (CreateFileMapping/MapViewOfFile)
#ifndef SHM_VIDEO_SOURCE_INTERFACE_H_
#define SHM_VIDEO_SOURCE_INTERFACE_H_

#include <QImage>
#include <QObject>

class IShmVideoSource : public QObject {
  Q_OBJECT
 public:
  explicit IShmVideoSource(QObject* parent = nullptr) : QObject(parent) {}
  ~IShmVideoSource() override = default;

  // Open the shared memory region and start reading frames.
  virtual bool Start(const QString& shm_key, int proj_id) = 0;

  // Stop reading and release resources.
  virtual void Stop() = 0;

 signals:
  void frameReady(const QImage& frame);
};

#endif  // SHM_VIDEO_SOURCE_INTERFACE_H_
