QT       += core gui widgets opengl network
FORMS    += mainwindow.ui
TARGET   = ui_rk
TEMPLATE = app
CONFIG   += c++17
DEFINES  += QT_DEPRECATED_WARNINGS

# WebRTC source (SHM structs, video_frame_shm_ctrl)
WEBRTC_SRC = /home/light/webrtc/checkout/src
INCLUDEPATH += $$WEBRTC_SRC
INCLUDEPATH += $$WEBRTC_SRC/apps/peerconnection/client
INCLUDEPATH += $$WEBRTC_SRC/apps/peerconnection/video_capture_shm_RGA
INCLUDEPATH += ./  # local RGA headers (rga.h, drmrga.h)

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    gl_video_widget.cpp \
    dma_buf_video_source.cpp \
    dma_buf_reader.cc \
    control_channel.cpp \
    webrtc_process_manager.cpp \
    voice_chat_client.cpp \
    voice_chat_manager.cpp \
    ai_receiver.cpp \
    serial_worker.cpp

HEADERS += \
    mainwindow.h \
    gl_video_widget.h \
    dma_buf_video_source.h \
    dma_buf_reader.h \
    shm_common.h \
    control_channel.h \
    webrtc_process_manager.h \
    voice_chat_client.h \
    voice_chat_manager.h \
    ai_receiver.h \
    serial_worker.h

# libyuv for RGA fallback
INCLUDEPATH += /home/light/webrtc/checkout/src/third_party/libyuv/include

LIBYUV_DIR = /home/light/work/webrtc_monitor/lib_x64
LIBS += $$LIBYUV_DIR/libyuv_arm64.a
LIBS += -lpthread -ldl -lGLESv2 -lEGL
LIBS += -Wl,--unresolved-symbols=ignore-in-object-files
