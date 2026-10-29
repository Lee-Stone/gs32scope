#ifndef WRITE_DIALOG_H
#define WRITE_DIALOG_H
#include "core/variable_def.h"
#include <QDialog>

class QLineEdit;

class WriteDialog : public QDialog
{
    Q_OBJECT
public:
    explicit WriteDialog(const VariableDef& def, double current,
                         QWidget* parent = nullptr);
    double value() const { return value_; }

protected:
    void accept() override;

private:
    VariableDef def_;
    QLineEdit*  edit_ = nullptr;
    double      value_ = 0.0;
};

#endif /* WRITE_DIALOG_H */
