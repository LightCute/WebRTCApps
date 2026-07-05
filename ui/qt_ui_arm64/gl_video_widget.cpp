#include "gl_video_widget.h"
#include <QOpenGLShader>
#include <QPainter>
#include <QDebug>

static const char* kVertexShader = R"(
attribute vec4 a_position;
attribute vec2 a_texCoord;
varying vec2 v_texCoord;
void main() {
    gl_Position = a_position;
    v_texCoord = a_texCoord;
}
)";

static const char* kFragmentShader = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 v_texCoord;
uniform sampler2D u_texture;
void main() {
    gl_FragColor = texture2D(u_texture, v_texCoord);
}
)";

GlVideoWidget::GlVideoWidget(QWidget* parent)
    : QOpenGLWidget(parent), program_(nullptr)
{
}

GlVideoWidget::~GlVideoWidget()
{
    makeCurrent();
    if (texture_id_) {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }
    delete program_;
    doneCurrent();
}

void GlVideoWidget::setDetections(QVector<Detection> detections) {
    // Filter by confidence threshold
    int total = (int)detections.size();
    detections.erase(std::remove_if(detections.begin(), detections.end(),
        [](const Detection& d) { return d.conf < 0.45f; }),
        detections.end());

    // ── Post-NMS: suppress duplicate overlapping boxes of same class ──
    for (int i = 0; i < detections.size(); i++) {
        for (int j = detections.size() - 1; j > i; j--) {
            if (detections[i].cls_id != detections[j].cls_id) continue;
            int l = qMax(detections[i].left, detections[j].left);
            int t = qMax(detections[i].top, detections[j].top);
            int r = qMin(detections[i].right, detections[j].right);
            int b = qMin(detections[i].bottom, detections[j].bottom);
            if (l >= r || t >= b) continue;
            float inter = (r - l) * (b - t);
            float areaI = (detections[i].right - detections[i].left)
                        * (detections[i].bottom - detections[i].top);
            float areaJ = (detections[j].right - detections[j].left)
                        * (detections[j].bottom - detections[j].top);
            if (inter / (areaI + areaJ - inter) > 0.5f) {
                if (detections[i].conf < detections[j].conf)
                    detections[i] = detections[j];
                detections.removeAt(j);
            }
        }
    }

    fprintf(stderr, "GlVideoWidget: got %d detections, %d after NMS (tex=%dx%d)\n",
            total, (int)detections.size(), tex_w_, tex_h_);
    detections_ = std::move(detections);
    update();  // trigger repaint when new detections arrive
}

void GlVideoWidget::setFrame(QImage frame)
{
    QMutexLocker lock(&mutex_);
    pending_frame_ = std::move(frame);
    frame_dirty_ = true;
    update();
}

void GlVideoWidget::setFrameInfo(const QString& info)
{
    frame_info_ = info;
    update();
}

void GlVideoWidget::initShader()
{
    delete program_;
    program_ = new QOpenGLShaderProgram();
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)) {
        qWarning() << "Vertex shader compile failed:" << program_->log();
    }
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
        qWarning() << "Fragment shader compile failed:" << program_->log();
    }
    program_->bindAttributeLocation("a_position", 0);
    program_->bindAttributeLocation("a_texCoord", 1);
    if (!program_->link()) {
        qWarning() << "Shader link failed:" << program_->log();
    }
}

void GlVideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    initShader();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glDisable(GL_DEPTH_TEST);
}

void GlVideoWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void GlVideoWidget::uploadTexture(const QImage& frame)
{
    QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
    int w = rgba.width();
    int h = rgba.height();

    if (first_upload_ || w != tex_w_ || h != tex_h_) {
        if (texture_id_ == 0) {
            glGenTextures(1, &texture_id_);
        }
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
        tex_w_ = w;
        tex_h_ = h;
        first_upload_ = false;
    } else {
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    }
}

void GlVideoWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    // Upload new frame if available; otherwise reuse existing GPU texture
    {
        QMutexLocker lock(&mutex_);
        if (frame_dirty_ && !pending_frame_.isNull()) {
            uploadTexture(pending_frame_);
            frame_dirty_ = false;
        }
    }

    // No texture uploaded yet — nothing to render
    if (first_upload_ || tex_w_ <= 0 || tex_h_ <= 0) {
        return;
    }

    program_->bind();
    program_->setUniformValue("u_texture", 0);

    // Use uploaded texture dimensions for aspect ratio (stable, avoids flicker)
    float tex_w = static_cast<float>(tex_w_);
    float tex_h = static_cast<float>(tex_h_);
    float widget_w = static_cast<float>(this->width());
    float widget_h = static_cast<float>(this->height());
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (widget_w / widget_h > tex_w / tex_h) {
        scale_x = (tex_w / tex_h) / (widget_w / widget_h);
    } else {
        scale_y = (widget_w / widget_h) / (tex_w / tex_h);
    }

    GLfloat vertices[] = {
        -scale_x, -scale_y,
         scale_x, -scale_y,
         scale_x,  scale_y,
        -scale_x,  scale_y,
    };
    GLfloat texCoords[] = {
        0.0f, 1.0f,
        1.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f,
    };

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    // Detection overlay (account for letterboxing)
    if (!detections_.isEmpty() && tex_w_ > 0 && tex_h_ > 0) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        float draw_w = scale_x * widget_w;
        float draw_h = scale_y * widget_h;
        float off_x = (widget_w - draw_w) / 2.0f;
        float off_y = (widget_h - draw_h) / 2.0f;
        float sx = draw_w / float(tex_w_);
        float sy = draw_h / float(tex_h_);
        for (const auto& d : detections_) {
            // Label-based color coding (matches ai_viewer)
            QColor c;
            QString lbl = d.label.toLower();
            if (lbl == "fallen" || lbl == "fire") {
                c = QColor(255, 40, 40, 220);     // red (danger)
            } else if (lbl == "smoke") {
                c = QColor(180, 180, 180, 220);   // gray (smoke)
            } else if (lbl == "sitting") {
                c = QColor(255, 200, 40, 220);    // yellow (attention)
            } else if (lbl == "standing") {
                c = QColor(40, 200, 80, 220);     // green (normal)
            } else if (d.conf > 0.7f) {
                c = QColor(255, 0, 0, 200);
            } else {
                c = QColor(0, 0, 255, 200);
            }
            painter.setPen(QPen(c, 3));
            int x = int(d.left * sx + off_x), y = int(d.top * sy + off_y);
            int w = int((d.right - d.left) * sx), h = int((d.bottom - d.top) * sy);
            painter.drawRect(x, y, w, h);
            // Label background shares box color
            QString txt = QString("%1 %2%").arg(d.label).arg(int(d.conf * 100));
            int tw = painter.fontMetrics().horizontalAdvance(txt) + 6;
            QRect tr(x, y - 18, tw, 18);
            if (tr.y() < 0) tr.moveTop(y + h + 2);
            painter.fillRect(tr, c.darker(150));
            painter.setPen(Qt::white);
            painter.drawText(tr.adjusted(3, 0, 0, 0), Qt::AlignVCenter, txt);
        }
    }

    // Frame info overlay
    if (!frame_info_.isEmpty()) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        QStringList lines = frame_info_.split('\n');
        QFont font("monospace", 10);
        painter.setFont(font);
        QFontMetrics fm(font);

        int max_width = 0;
        for (const auto& line : lines)
            max_width = qMax(max_width, fm.horizontalAdvance(line));
        int total_height = lines.size() * fm.height() + 8;

        QRect bg_rect(8, 8, max_width + 16, total_height);
        painter.fillRect(bg_rect, QColor(0, 0, 0, 160));
        painter.setPen(Qt::white);
        int y = 8 + fm.ascent() + 2;
        for (const auto& line : lines) {
            painter.drawText(14, y, line);
            y += fm.height();
        }
    }
}
