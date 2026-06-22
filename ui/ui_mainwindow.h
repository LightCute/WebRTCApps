/********************************************************************************
** Form generated from reading UI file 'mainwindow.ui'
**
** Created by: Qt User Interface Compiler version 5.15.18
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MAINWINDOW_H
#define UI_MAINWINDOW_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include "gl_video_widget.h"

QT_BEGIN_NAMESPACE

class Ui_MainWindow
{
public:
    QWidget *centralWidget;
    QVBoxLayout *mainLayout;
    QStackedWidget *stack_;
    QWidget *listPage;
    QVBoxLayout *listLayout;
    QTreeWidget *peer_tree_;
    QWidget *callPage;
    QVBoxLayout *callLayout;
    QSplitter *callSplitter;
    QWidget *remote_container_;
    QVBoxLayout *remoteLayout;
    GlVideoWidget *remote_video_;
    QWidget *right_panel_;
    QVBoxLayout *panelLayout;
    QGroupBox *stats_group_;
    QGridLayout *statsLayout;
    QLabel *stats_loss_;
    QLabel *stats_conn_;
    QLabel *stats_rtt_;
    QLabel *stats_limit_;
    QLabel *stats_send_quality_;
    QLabel *stats_encode_ms_;
    QLabel *stats_recv_quality_;
    QLabel *stats_decode_ms_;
    QLabel *stats_send_rate_;
    QLabel *stats_avail_kbps_;
    QLabel *stats_recv_rate_;
    QLabel *stats_freeze_;
    QPushButton *stats_monitor_btn_;
    QGroupBox *video_quality_group_;
    QGridLayout *videoQualityLayout;
    QComboBox *video_caps_combo_;
    QPushButton *video_query_btn_;
    QPushButton *video_apply_btn_;
    QPlainTextEdit *chat_display_;
    QWidget *chatInputRow;
    QHBoxLayout *chatInputLayout;
    QLineEdit *chat_input_;
    QPushButton *send_button_;
    QPlainTextEdit *log_area_;
    QStatusBar *statusbar;

    void setupUi(QMainWindow *MainWindow)
    {
        if (MainWindow->objectName().isEmpty())
            MainWindow->setObjectName(QString::fromUtf8("MainWindow"));
        MainWindow->resize(612, 741);
        centralWidget = new QWidget(MainWindow);
        centralWidget->setObjectName(QString::fromUtf8("centralWidget"));
        mainLayout = new QVBoxLayout(centralWidget);
        mainLayout->setSpacing(4);
        mainLayout->setObjectName(QString::fromUtf8("mainLayout"));
        mainLayout->setContentsMargins(4, 4, 4, 4);
        stack_ = new QStackedWidget(centralWidget);
        stack_->setObjectName(QString::fromUtf8("stack_"));
        listPage = new QWidget();
        listPage->setObjectName(QString::fromUtf8("listPage"));
        listLayout = new QVBoxLayout(listPage);
        listLayout->setSpacing(4);
        listLayout->setObjectName(QString::fromUtf8("listLayout"));
        listLayout->setContentsMargins(0, 0, 0, 0);
        peer_tree_ = new QTreeWidget(listPage);
        peer_tree_->setObjectName(QString::fromUtf8("peer_tree_"));
        peer_tree_->setMaximumSize(QSize(16777215, 220));

        listLayout->addWidget(peer_tree_);

        stack_->addWidget(listPage);
        callPage = new QWidget();
        callPage->setObjectName(QString::fromUtf8("callPage"));
        callLayout = new QVBoxLayout(callPage);
        callLayout->setSpacing(0);
        callLayout->setObjectName(QString::fromUtf8("callLayout"));
        callLayout->setContentsMargins(0, 0, 0, 0);
        callSplitter = new QSplitter(callPage);
        callSplitter->setObjectName(QString::fromUtf8("callSplitter"));
        callSplitter->setOrientation(Qt::Orientation::Horizontal);
        callSplitter->setHandleWidth(4);
        callSplitter->setChildrenCollapsible(false);
        remote_container_ = new QWidget(callSplitter);
        remote_container_->setObjectName(QString::fromUtf8("remote_container_"));
        remote_container_->setMinimumSize(QSize(400, 0));
        remoteLayout = new QVBoxLayout(remote_container_);
        remoteLayout->setSpacing(0);
        remoteLayout->setObjectName(QString::fromUtf8("remoteLayout"));
        remoteLayout->setContentsMargins(0, 0, 0, 0);
        remote_video_ = new GlVideoWidget(remote_container_);
        remote_video_->setObjectName(QString::fromUtf8("remote_video_"));
        QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(remote_video_->sizePolicy().hasHeightForWidth());
        remote_video_->setSizePolicy(sizePolicy);

        remoteLayout->addWidget(remote_video_);

        callSplitter->addWidget(remote_container_);
        right_panel_ = new QWidget(callSplitter);
        right_panel_->setObjectName(QString::fromUtf8("right_panel_"));
        right_panel_->setMinimumSize(QSize(200, 0));
        panelLayout = new QVBoxLayout(right_panel_);
        panelLayout->setSpacing(4);
        panelLayout->setObjectName(QString::fromUtf8("panelLayout"));
        panelLayout->setContentsMargins(8, 8, 8, 8);
        stats_group_ = new QGroupBox(right_panel_);
        stats_group_->setObjectName(QString::fromUtf8("stats_group_"));
        statsLayout = new QGridLayout(stats_group_);
        statsLayout->setSpacing(2);
        statsLayout->setObjectName(QString::fromUtf8("statsLayout"));
        stats_loss_ = new QLabel(stats_group_);
        stats_loss_->setObjectName(QString::fromUtf8("stats_loss_"));

        statsLayout->addWidget(stats_loss_, 1, 0, 1, 1);

        stats_conn_ = new QLabel(stats_group_);
        stats_conn_->setObjectName(QString::fromUtf8("stats_conn_"));

        statsLayout->addWidget(stats_conn_, 0, 0, 1, 1);

        stats_rtt_ = new QLabel(stats_group_);
        stats_rtt_->setObjectName(QString::fromUtf8("stats_rtt_"));

        statsLayout->addWidget(stats_rtt_, 0, 1, 1, 1);

        stats_limit_ = new QLabel(stats_group_);
        stats_limit_->setObjectName(QString::fromUtf8("stats_limit_"));

        statsLayout->addWidget(stats_limit_, 1, 1, 1, 1);

        stats_send_quality_ = new QLabel(stats_group_);
        stats_send_quality_->setObjectName(QString::fromUtf8("stats_send_quality_"));

        statsLayout->addWidget(stats_send_quality_, 2, 0, 1, 1);

        stats_encode_ms_ = new QLabel(stats_group_);
        stats_encode_ms_->setObjectName(QString::fromUtf8("stats_encode_ms_"));

        statsLayout->addWidget(stats_encode_ms_, 2, 1, 1, 1);

        stats_recv_quality_ = new QLabel(stats_group_);
        stats_recv_quality_->setObjectName(QString::fromUtf8("stats_recv_quality_"));

        statsLayout->addWidget(stats_recv_quality_, 3, 0, 1, 1);

        stats_decode_ms_ = new QLabel(stats_group_);
        stats_decode_ms_->setObjectName(QString::fromUtf8("stats_decode_ms_"));

        statsLayout->addWidget(stats_decode_ms_, 3, 1, 1, 1);

        stats_send_rate_ = new QLabel(stats_group_);
        stats_send_rate_->setObjectName(QString::fromUtf8("stats_send_rate_"));

        statsLayout->addWidget(stats_send_rate_, 4, 0, 1, 1);

        stats_avail_kbps_ = new QLabel(stats_group_);
        stats_avail_kbps_->setObjectName(QString::fromUtf8("stats_avail_kbps_"));

        statsLayout->addWidget(stats_avail_kbps_, 4, 1, 1, 1);

        stats_recv_rate_ = new QLabel(stats_group_);
        stats_recv_rate_->setObjectName(QString::fromUtf8("stats_recv_rate_"));

        statsLayout->addWidget(stats_recv_rate_, 5, 0, 1, 1);

        stats_freeze_ = new QLabel(stats_group_);
        stats_freeze_->setObjectName(QString::fromUtf8("stats_freeze_"));

        statsLayout->addWidget(stats_freeze_, 5, 1, 1, 1);

        stats_monitor_btn_ = new QPushButton(stats_group_);
        stats_monitor_btn_->setObjectName(QString::fromUtf8("stats_monitor_btn_"));

        statsLayout->addWidget(stats_monitor_btn_, 6, 0, 1, 2);


        panelLayout->addWidget(stats_group_);

        video_quality_group_ = new QGroupBox(right_panel_);
        video_quality_group_->setObjectName(QString::fromUtf8("video_quality_group_"));
        videoQualityLayout = new QGridLayout(video_quality_group_);
        videoQualityLayout->setSpacing(2);
        videoQualityLayout->setObjectName(QString::fromUtf8("videoQualityLayout"));
        video_caps_combo_ = new QComboBox(video_quality_group_);
        video_caps_combo_->addItem(QString());
        video_caps_combo_->setObjectName(QString::fromUtf8("video_caps_combo_"));
        video_caps_combo_->setEnabled(false);

        videoQualityLayout->addWidget(video_caps_combo_, 0, 0, 1, 2);

        video_query_btn_ = new QPushButton(video_quality_group_);
        video_query_btn_->setObjectName(QString::fromUtf8("video_query_btn_"));

        videoQualityLayout->addWidget(video_query_btn_, 1, 0, 1, 1);

        video_apply_btn_ = new QPushButton(video_quality_group_);
        video_apply_btn_->setObjectName(QString::fromUtf8("video_apply_btn_"));
        video_apply_btn_->setEnabled(false);

        videoQualityLayout->addWidget(video_apply_btn_, 1, 1, 1, 1);


        panelLayout->addWidget(video_quality_group_);

        chat_display_ = new QPlainTextEdit(right_panel_);
        chat_display_->setObjectName(QString::fromUtf8("chat_display_"));
        chat_display_->setReadOnly(true);

        panelLayout->addWidget(chat_display_);

        chatInputRow = new QWidget(right_panel_);
        chatInputRow->setObjectName(QString::fromUtf8("chatInputRow"));
        chatInputLayout = new QHBoxLayout(chatInputRow);
        chatInputLayout->setSpacing(4);
        chatInputLayout->setObjectName(QString::fromUtf8("chatInputLayout"));
        chatInputLayout->setContentsMargins(0, 0, 0, 0);
        chat_input_ = new QLineEdit(chatInputRow);
        chat_input_->setObjectName(QString::fromUtf8("chat_input_"));
        chat_input_->setEnabled(false);

        chatInputLayout->addWidget(chat_input_);

        send_button_ = new QPushButton(chatInputRow);
        send_button_->setObjectName(QString::fromUtf8("send_button_"));
        send_button_->setEnabled(false);
        send_button_->setMaximumSize(QSize(80, 16777215));

        chatInputLayout->addWidget(send_button_);


        panelLayout->addWidget(chatInputRow);

        callSplitter->addWidget(right_panel_);

        callLayout->addWidget(callSplitter);

        stack_->addWidget(callPage);

        mainLayout->addWidget(stack_);

        log_area_ = new QPlainTextEdit(centralWidget);
        log_area_->setObjectName(QString::fromUtf8("log_area_"));
        log_area_->setMaximumSize(QSize(16777215, 100));
        log_area_->setReadOnly(true);

        mainLayout->addWidget(log_area_);

        MainWindow->setCentralWidget(centralWidget);
        statusbar = new QStatusBar(MainWindow);
        statusbar->setObjectName(QString::fromUtf8("statusbar"));
        MainWindow->setStatusBar(statusbar);

        retranslateUi(MainWindow);

        stack_->setCurrentIndex(0);


        QMetaObject::connectSlotsByName(MainWindow);
    } // setupUi

    void retranslateUi(QMainWindow *MainWindow)
    {
        MainWindow->setWindowTitle(QCoreApplication::translate("MainWindow", "WebRTC \350\277\234\347\250\213\346\216\247\345\210\266", nullptr));
        QTreeWidgetItem *___qtreewidgetitem = peer_tree_->headerItem();
        ___qtreewidgetitem->setText(2, QCoreApplication::translate("MainWindow", "\347\212\266\346\200\201", nullptr));
        ___qtreewidgetitem->setText(1, QCoreApplication::translate("MainWindow", "\345\220\215\347\247\260", nullptr));
        ___qtreewidgetitem->setText(0, QCoreApplication::translate("MainWindow", "ID", nullptr));
        stats_group_->setTitle(QCoreApplication::translate("MainWindow", "\351\200\232\344\277\241\350\264\250\351\207\217", nullptr));
        stats_loss_->setText(QCoreApplication::translate("MainWindow", "\344\270\242\345\214\205: --%", nullptr));
        stats_conn_->setText(QCoreApplication::translate("MainWindow", "\350\277\236\346\216\245: --", nullptr));
        stats_rtt_->setText(QCoreApplication::translate("MainWindow", "\345\273\266\350\277\237: --ms", nullptr));
        stats_limit_->setText(QCoreApplication::translate("MainWindow", "\351\231\215\350\264\250: --", nullptr));
        stats_send_quality_->setText(QCoreApplication::translate("MainWindow", "\345\217\221\351\200\201: --", nullptr));
        stats_encode_ms_->setText(QCoreApplication::translate("MainWindow", "\347\274\226\347\240\201: --ms", nullptr));
        stats_recv_quality_->setText(QCoreApplication::translate("MainWindow", "\346\216\245\346\224\266: --", nullptr));
        stats_decode_ms_->setText(QCoreApplication::translate("MainWindow", "\350\247\243\347\240\201: --ms", nullptr));
        stats_send_rate_->setText(QCoreApplication::translate("MainWindow", "\345\217\221\351\200\201\347\240\201\347\216\207: --", nullptr));
        stats_avail_kbps_->setText(QCoreApplication::translate("MainWindow", "\345\217\257\347\224\250\345\270\246\345\256\275: --", nullptr));
        stats_recv_rate_->setText(QCoreApplication::translate("MainWindow", "\346\216\245\346\224\266\347\240\201\347\216\207: --", nullptr));
        stats_freeze_->setText(QCoreApplication::translate("MainWindow", "\345\206\273\347\273\223: 0", nullptr));
        stats_monitor_btn_->setText(QCoreApplication::translate("MainWindow", "\346\265\213\351\207\217\345\220\257\345\212\250", nullptr));
        video_quality_group_->setTitle(QCoreApplication::translate("MainWindow", "\350\247\206\351\242\221\347\224\273\350\264\250", nullptr));
        video_caps_combo_->setItemText(0, QCoreApplication::translate("MainWindow", "640\303\227480 30fps", nullptr));

        video_query_btn_->setText(QCoreApplication::translate("MainWindow", "\346\237\245\350\257\242\350\203\275\345\212\233", nullptr));
        video_apply_btn_->setText(QCoreApplication::translate("MainWindow", "\345\272\224\347\224\250", nullptr));
        chat_display_->setPlaceholderText(QCoreApplication::translate("MainWindow", "DataChannel / \350\257\255\351\237\263\345\257\271\350\257\235\346\266\210\346\201\257...", nullptr));
        chat_input_->setPlaceholderText(QCoreApplication::translate("MainWindow", "\350\276\223\345\205\245\346\266\210\346\201\257...", nullptr));
        send_button_->setText(QCoreApplication::translate("MainWindow", "\345\217\221\351\200\201", nullptr));
        log_area_->setPlaceholderText(QCoreApplication::translate("MainWindow", "\346\227\245\345\277\227...", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MainWindow: public Ui_MainWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MAINWINDOW_H
