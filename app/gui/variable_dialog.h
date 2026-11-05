#ifndef VARIABLE_DIALOG_H
#define VARIABLE_DIALOG_H
/*
 * VariableDialog - 变量选择对话框(非模态)
 * 列: 变量(节点勾选框=监控) | 地址 | 大小 | 类型
 * 文件根无勾选框；波形选择统一在监控表完成。
 */
#include "core/variable_def.h"
#include "symbols/dwarf_parser.h"
#include <QDialog>
#include <QLineEdit>
#include <QSettings>
#include <QTimer>
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
    /* 最近一次 ELF 导入的 DWARF 变量树(供数组组件构建目录) */
    const QVector<DwarfVar>& elfVars() const { return elfVars_; }
    /* 监控表按根移除时, 同步取消导入表格中的勾选框(不触发二次移除) */
    void uncheckMonitored(const QString& path);
    void clearMonitoredChecks();   /* 数据菜单清空变量时同步取消所有勾选 */

signals:
    void monitorAdded(const DwarfVar& v);
    void monitorRemoved(const QString& rootPath);
    void elfReloadStarted();    /* MainWindow 快照波形/格式状态 */
    void elfReloadFinished();   /* 重新应用快照 */

private slots:
    void onItemChanged(QTreeWidgetItem* item, int col);
    void onImportElf();
    void onImportSvd();
    void onRemoveSelected();
    void onSearch(const QString& text);
    void onSortByColumn(int column, Qt::SortOrder order);
    void checkElfChanged();

private:
    bool importElfFile(const QString& path, bool interactive = true);
    bool importSvdFile(const QString& path, bool interactive = true);
    QTreeWidgetItem* ensureFileRoot(const QString& file);
    void addLeafItem(QTreeWidgetItem* parent, const VariableDef& v);
    void sortTopLevel(int column, Qt::SortOrder order);   /* 只排顶层, 成员不动 */
    void setSubtreeChecked(QTreeWidgetItem* it, Qt::CheckState st);
    DwarfVar toDwarf(QTreeWidgetItem* it) const;
    QString itemPath(QTreeWidgetItem* it) const;

    /* ELF 生命周期: 只保留一个 ELF; 重新导入时保留勾选项, 文件变化时提示 */
    QVector<DwarfVar> elfVars_;
    QStringList collectElfCheckedPaths() const;
    void removeElfRoots(QStringList* checkedPaths);
    void recheckPaths(const QStringList& paths);
    void updateElfSignature();
    bool reloadElfFile(const QString& path);

    enum { ColName = 0, ColAddr = 1, ColSize = 2, ColType = 3 };
    QTreeWidget* tree_ = nullptr;
    QLineEdit*   search_ = nullptr;
    QTimer       elfTimer_;
    QString      elfPath_;
    qint64       elfMtime_ = 0;
    qint64       elfSize_ = -1;
    bool         autoReloadElf_ = false;
    bool         reloadingElf_ = false;
    bool         elfPromptOpen_ = false;
    bool         userSorted_ = false;   /* 用户点过表头排序(否则导入后默认名称升序) */
};

#endif /* VARIABLE_DIALOG_H */
