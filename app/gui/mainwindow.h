#ifndef MAINWINDOW_H
#define MAINWINDOW_H
/*
 * MainWindow - GS32Scope 主界面
 * 波形+仪表盘(上排等宽) / 监控变量+日志(下排等宽), 四块均为可停靠面板
 * 顶部: 连接/采样工具栏 + 变量选择入口
 */
#include <QMainWindow>
#include <QHash>
#include <QPointF>
#include <QTimer>
#include <QVector>
#include "link/link_driver.h"
#include "core/sample_engine.h"
#include "core/monitor_model.h"
#include "gui/plot_widget.h"
#include "gui/monitor_dock.h"

class QComboBox;
class QSpinBox;
class QLabel;
class QAction;
class QActionGroup;
class QLineEdit;
class QDockWidget;
class QPlainTextEdit;
class VariableDialog;
class DashboardWidget;

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;   /* 退出时保存会话(UI 设定持久化) */

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onStartClicked();
    void onSingleClicked();
    void onStopClicked();
    void onOpenVariableDialog();
    void onMonitorAdded(const DwarfVar& v);
    void onMonitorRemoved(const QString& rootPath);
    void onDisplayToggled(const VariableDef& def, bool on);
    void onRemoveRoot(int rootIdx);
    void onModelChanged();
    void onLeafShownChanged(int leafIdx);
    void onBatchReady(const SampleBatch& batch);
    void onStats(const EngineStats& st);
    void onUiTick();
    void onEngineStopped();
    void onExportCsv();
    void onClearPlot();
    void onWriteLeaf(int leafIdx);
    void onDashboardWrite(const QString& path, double value);
    void onSimDemo();
    void onStreamDemo();

private:
    void buildUi();
    void refreshPortList();   /* 目标下拉: 重扫端口, 保留当前选择, 默认第一个 */
    void applyTheme(const QString& mode);   /* light / dark / system */
    bool systemPrefersDark() const;
    void reconfigureEngine();
    void updateStartAction();
    void setConnControlsEnabled(bool on);  /* 通讯接口/目标/采样率/TCK 连接期间禁用 */
    bool startSampling(QString* err);      /* 按采样率选择 POLL 或 STREAM */
    int  rateHz() const;
    void syncPlotChannels();
    void pruneHistory();
    void defaultDockLayout();
    void applyDockSizes();
    void appendLog(const QString& msg);
    void beginTimelineSegment();
    void saveSessionLeaves(QSettings& s) const;
    void restoreSessionLeaves(QSettings& s);
    bool writeVariable(const QString& path, double value, bool confirm);
    void loadSimDemoVariables();
    bool ensureDemoReady();   /* 连接 sim/sci/can、加载演示变量、生成面板 */
    void setStatus(const QString& msg, bool isError = false);
    QLabel* statusMsgLabel();      /* 状态栏左侧消息(懒创建) */

    Ui::MainWindow* ui = nullptr;   /* mainwindow.ui 生成的界面结构 */

    LinkDriver   link_;
    QString      connectedBackend_;
    MonitorModel* model_ = nullptr;
    SampleEngine* engine_ = nullptr;
    QThread*      engineThread_ = nullptr;
    VariableDialog* varDialog_ = nullptr;
    QTimer        uiTimer_;
    bool stopping_ = false;         /* 已 requestStop, 等 stopped() 回调 */
    bool pendingRestart_ = false;   /* 停止后重启采样(模型变更) */
    bool pendingDisconnect_ = false;/* 停止后关闭连接 */

    QComboBox*   backendBox_ = nullptr;
    QSpinBox*    intervalSpin_ = nullptr;   /* 采样率(Hz) */
    QLabel*      statusConn_ = nullptr;
    QLabel*      statusRate_ = nullptr;
    QLabel*      statusMsg_ = nullptr;   /* 状态栏左侧动态消息 */
    QAction*     actConn_ = nullptr;
    QAction*     actDisc_ = nullptr;
    QAction*     actStart_ = nullptr;   /* 连续采样: 文本在 连续采样/停止采样/停止中… 间切换 */
    QAction*     actSingle_ = nullptr;  /* 单次采样: 一拍即停 */
    QAction*     actCsv_ = nullptr;     /* 导出波形 CSV */
    QAction*     actClear_ = nullptr;   /* 清空波形/历史 */
    QAction*     actSimDemo_ = nullptr; /* 一键仿真演示 */
    QAction*     themeLight_ = nullptr;
    QAction*     themeDark_ = nullptr;
    QAction*     themeSystem_ = nullptr;
    QActionGroup* themeGroup_ = nullptr;
    QString      themeMode_ = QStringLiteral("system");
    PlotWidget*  plot_ = nullptr;
    MonitorDock* monDock_ = nullptr;
    DashboardWidget* dashboard_ = nullptr;
    QDockWidget* plotDock_ = nullptr;
    QDockWidget* dashDock_ = nullptr;
    QDockWidget* logDock_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;

    /* 会话时间轴(停止/继续时去除停采时间)与各叶子历史(用于新通道回放) */
    QHash<QString, QVector<QPointF>> history_;
    qint64 timelineRawBaseNs_ = 0;
    qint64 timelineLogicalBaseNs_ = 0;
    qint64 lastLogicalNs_ = 0;
    bool   timelineStarted_ = false;
    QTimer plotTimer_;                  /* 30fps 重绘, 与采样/批处理解耦 */
};

#endif /* MAINWINDOW_H */
