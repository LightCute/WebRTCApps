QT       += core gui widgets opengl network multimedia
FORMS    += mainwindow.ui
TARGET   = ui_x64
TEMPLATE = app
CONFIG   += c++17
DEFINES  += QT_DEPRECATED_WARNINGS

# WebRTC source (SHM structs from client/)
WEBRTC_SRC = /home/light/webrtc/checkout/src
INCLUDEPATH += $$WEBRTC_SRC
INCLUDEPATH += $$WEBRTC_SRC/apps/peerconnection/client

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    gl_video_widget.cpp \
    shm_video_source.cpp \
    shm_video_reader.cpp \
    control_channel.cpp \
    webrtc_process_manager.cpp \
    voice_chat_client.cpp \
    voice_chat_manager.cpp

HEADERS += \
    mainwindow.h \
    gl_video_widget.h \
    shm_video_source.h \
    shm_video_source_interface.h \
    shm_video_reader.h \
    shm_common.h \
    control_channel.h \
    webrtc_process_manager.h \
    voice_chat_client.h \
    voice_chat_manager.h

# libyuv for I420→ARGB conversion
INCLUDEPATH += /home/light/webrtc/checkout/src/third_party/libyuv/include
LIBS += -lpthread -ldl /home/light/work/webrtc_monitor/lib_x64/libyuv_x64.a
