#ifndef VARIABLE_DIALOG_H
#define VARIABLE_DIALOG_H
/*
 * VariableDialog - 变量选择对话框(非模态)
 * 列: 变量(节点勾选框=监控) | 地址 | 大小 | 类型 | 绘制(仅叶子)
 * 文件根无勾选框。勾"绘制"未勾"监控"的叶子自动补监控。
 */
#include "core/variable_def.h"
#include "symbols/dwarf_parser.h"
#include <QDialog>
#include <QLineEdit>
#include <QSettings>
#include <QTreeWidget>

class VariableDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VariableDialog(QWidget* parent = nullptr);

    void saveState(QSettings& s) const;   /* 持久化: 导入文件/勾选状态 */
    void restoreState(QSettings& s);      /* 启动恢复: 重导入文件 + 重放勾选 */
    /* 启动参数/自动化入口: 静默导入 + 监控所有顶层变量/外设 */
    bool importElf(const QString& path);
    bool importSvd(const QString& path);
    void monitorAll();

signals:
    void monitorAdded(const DwarfVar& v);
    void monitorRemoved(const QString& rootPath);
    void displayToggled(const VariableDef& def, bool on);   /* 独立于监控 */

private slots:
    void onItemChanged(QTreeWidgetItem* item, int col);
    void onImportElf();
    void onImportSvd();
    void onRemoveSelected();
    void onSearch(const QString& text);
    void onSortByColumn(int column, Qt::SortOrder order);

private:
    bool importElfFile(const QString& path, bool interactive = true);
    bool importSvdFile(const QString& path, bool interactive = true);
    QTreeWidgetItem* ensureFileRoot(const QString& file);
    void addLeafItem(QTreeWidgetItem* parent, const VariableDef& v);
    void sortTopLevel(int column, Qt::SortOrder order);   /* 只排顶层, 成员不动 */
    void setSubtreeChecked(QTreeWidgetItem* it, Qt::CheckState st);
    DwarfVar toDwarf(QTreeWidgetItem* it) const;
    QString itemPath(QTreeWidgetItem* it) const;

    enum { ColName = 0, ColAddr = 1, ColSize = 2, ColType = 3, ColDraw = 4 };
    QTreeWidget* tree_ = nullptr;
    QLineEdit*   search_ = nullptr;
    bool         userSorted_ = false;   /* 用户点过表头排序(否则导入后默认名称升序) */
};

#endif /* VARIABLE_DIALOG_H */
