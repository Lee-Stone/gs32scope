#ifndef SPIN_BOX_H
#define SPIN_BOX_H
/* 数值框: 隐藏原生小按钮, 右侧放大号 ▲/▼ 按钮, 采样间隔等更易操作 */
#include <QAbstractSpinBox>
#include <QResizeEvent>
#include <QSpinBox>
#include <QToolButton>
#include <algorithm>

class SpinBox : public QSpinBox
{
public:
    explicit SpinBox(QWidget* parent = nullptr) : QSpinBox(parent) {
        setMinimumWidth(112);
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        up_ = new QToolButton(this);
        down_ = new QToolButton(this);
        up_->setObjectName("spinUp");
        down_->setObjectName("spinDown");
        up_->setText(QString::fromUtf8("▲"));
        down_->setText(QString::fromUtf8("▼"));
        up_->setAutoRepeat(true);
        down_->setAutoRepeat(true);
        up_->setFocusPolicy(Qt::NoFocus);
        down_->setFocusPolicy(Qt::NoFocus);
        up_->setCursor(Qt::PointingHandCursor);
        down_->setCursor(Qt::PointingHandCursor);
        connect(up_, &QToolButton::clicked, this, &QSpinBox::stepUp);
        connect(down_, &QToolButton::clicked, this, &QSpinBox::stepDown);
        if (auto* le = lineEdit()) {
            le->setStyleSheet(QStringLiteral("padding-right:30px;"));
            le->setAlignment(Qt::AlignCenter);
        }
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QSpinBox::resizeEvent(e);
        const int w = 28;
        const int half = height() / 2;
        up_->setGeometry(width() - w - 1, 1, w, std::max(1, half - 1));
        down_->setGeometry(width() - w - 1, half, w, std::max(1, height() - half - 1));
        up_->raise();
        down_->raise();
    }

private:
    QToolButton* up_ = nullptr;
    QToolButton* down_ = nullptr;
};

#endif /* SPIN_BOX_H */
