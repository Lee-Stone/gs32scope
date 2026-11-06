#ifndef COMBO_BOX_H
#define COMBO_BOX_H
/* 下拉框: 始终向下展开选项(不因空间自动翻到上方) */
#include <QAbstractItemView>
#include <QComboBox>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <algorithm>

class ComboBox : public QComboBox
{
    Q_OBJECT
public:
    using QComboBox::QComboBox;

signals:
    void popupAboutToShow();   /* 展开前发出, 用于下拉时刷新列表 */

public:
    void showPopup() override {
        emit popupAboutToShow();
        QComboBox::showPopup();
        QWidget* popup = view() ? view()->window() : nullptr;
        if (!popup) return;
        QScreen* scr = screen();
        if (!scr)
            scr = QGuiApplication::screenAt(mapToGlobal(QPoint(0, height())));
        if (!scr) return;
        const QRect avail = scr->availableGeometry();
        const int w = std::max(width(), popup->width());
        int h = popup->height();
        /* Qt 在部分样式下会把弹出列表拉到整屏高; 按 maxVisibleItems 限高,
         * 其余条目通过列表滚动查看 */
        if (view() && maxVisibleItems() > 0) {
            const int rowH = view()->sizeHintForRow(0);
            if (rowH > 0)
                h = std::min(h, rowH * maxVisibleItems() + 2 * view()->frameWidth());
        }
        int x = mapToGlobal(QPoint(0, 0)).x();
        int y = mapToGlobal(QPoint(0, height())).y();   /* 始终放在下方 */
        x = std::clamp(x, avail.left(), std::max(avail.left(), avail.right() - w));
        y = std::min(y, std::max(avail.top(), avail.bottom() - h));
        popup->setGeometry(x, y, w, h);
    }
};

#endif /* COMBO_BOX_H */
