#include "write_dialog.h"
#include "core/value_codec.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

namespace {
QString fmt(double v, const VariableDef& d) {
    if (isFloatType(d.type) && d.bitOffset < 0)
        return QString::number(v, 'f', d.type == VariableDef::Double ? 6 : 3);
    if (d.bitOffset >= 0)
        return QStringLiteral("0x") + QString::number(static_cast<quint64>(v), 16)
                   .rightJustified((d.bitWidth + 3) / 4, '0').toUpper();
    return QString::number(static_cast<qlonglong>(v));
}
}

WriteDialog::WriteDialog(const VariableDef& def, double current, QWidget* parent)
    : QDialog(parent), def_(def) {
    setWindowTitle("写入变量");
    setMinimumWidth(420);

    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->addRow("变量:", new QLabel(def.name));
    form->addRow("地址:", new QLabel(QStringLiteral("0x%1")
                                        .arg(def.addr, 8, 16, QChar('0'))));
    form->addRow("类型:", new QLabel(def.bitOffset >= 0
        ? QString("%1:%2").arg(typeName(def.type)).arg(def.bitWidth)
        : QString::fromLatin1(typeName(def.type))));
    form->addRow("当前值:", new QLabel(fmt(current, def)));
    edit_ = new QLineEdit(fmt(current, def), this);
    edit_->selectAll();
    form->addRow("新值:", edit_);
    lay->addLayout(form);

    auto* hint = new QLabel("整数支持 0x 十六进制; 写入后会立即回读校验。", this);
    hint->setStyleSheet("color:#6b7280;");
    lay->addWidget(hint);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText("写入");
    box->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(box, &QDialogButtonBox::accepted, this, &WriteDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &WriteDialog::reject);
    lay->addWidget(box);
    edit_->setFocus();
}

void WriteDialog::accept() {
    const QString t = edit_->text().trimmed();
    bool ok = false;
    double v = 0.0;
    if (isFloatType(def_.type) && def_.bitOffset < 0) {
        v = t.toDouble(&ok);
    } else if (gs32::isSignedType(def_.type) && def_.bitOffset < 0) {
        const qlonglong x = t.toLongLong(&ok, 0);
        v = double(x);
    } else {
        const qulonglong x = t.toULongLong(&ok, 0);
        v = double(x);
    }
    if (!ok) {
        QMessageBox::warning(this, "输入无效", "请输入合法数值(整数可用 0x 前缀)。");
        return;
    }
    if (def_.bitOffset >= 0) {
        const qulonglong mask = gs32::bitMask(def_.bitWidth);
        if (v < 0 || static_cast<qulonglong>(v) > mask) {
            QMessageBox::warning(this, "输入越界",
                QString("该位域宽度 %1 bit, 允许范围 0..0x%2")
                    .arg(def_.bitWidth).arg(mask, 0, 16));
            return;
        }
    }
    value_ = v;
    QDialog::accept();
}
