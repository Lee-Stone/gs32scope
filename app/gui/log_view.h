#ifndef LOG_VIEW_H
#define LOG_VIEW_H
/*
 * 日志窗口: 默认跟随新日志自动滚到底。
 * 用户手动向上滚动(滚轮/滚动条/PageUp)后停止自动;
 * 在日志区双击恢复自动并滚到底。
 */
#include <QAbstractSlider>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QWheelEvent>

class LogView : public QPlainTextEdit
{
public:
    explicit LogView(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        setReadOnly(true);
        setMaximumBlockCount(5000);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        viewport()->installEventFilter(this);
        QScrollBar* sb = verticalScrollBar();
        connect(sb, &QScrollBar::actionTriggered, this, [this](int action) {
            /* 滚轮/键盘/点滚动条槽的向上动作停止自动 */
            if (action == QAbstractSlider::SliderSingleStepSub ||
                action == QAbstractSlider::SliderPageStepSub ||
                action == QAbstractSlider::SliderToMinimum)
                autoScroll_ = false;
        });
        connect(sb, &QScrollBar::sliderMoved, this, [this](int) {
            autoScroll_ = false;
        });
    }

    /* 追加一行: 自动模式滚到底; 手动模式保持当前查看位置 */
    void appendLine(const QString& line) {
        const int keep = verticalScrollBar()->value();
        appendPlainText(line);
        if (autoScroll_)
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        else
            verticalScrollBar()->setValue(keep);
    }

    bool autoScroll() const { return autoScroll_; }

protected:
    void wheelEvent(QWheelEvent* e) override {
        if (e->angleDelta().y() > 0)
            autoScroll_ = false;   /* 向上滚: 停住 */
        QPlainTextEdit::wheelEvent(e);
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        resumeAuto();
        e->accept();               /* 不再触发选中单词/光标跳动 */
    }

    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == viewport() && e->type() == QEvent::MouseButtonDblClick) {
            resumeAuto();
            return true;
        }
        return QPlainTextEdit::eventFilter(o, e);
    }

    bool viewportEvent(QEvent* e) override {
        if (e->type() == QEvent::MouseButtonDblClick) {
            resumeAuto();
            return true;
        }
        return QPlainTextEdit::viewportEvent(e);
    }

    void keyPressEvent(QKeyEvent* e) override {
        switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_PageUp:
        case Qt::Key_Home:
            autoScroll_ = false;
            break;
        default:
            break;
        }
        QPlainTextEdit::keyPressEvent(e);
    }

private:
    void resumeAuto() {
        autoScroll_ = true;
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }

    bool autoScroll_ = true;
};

#endif /* LOG_VIEW_H */
