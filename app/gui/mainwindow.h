#ifndef MAINWINDOW_H
#define MAINWINDOW_H
/*
 * MainWindow - GS32Scope 主界面 (v1.3 三区布局)
 * 左上部: 波形窗口(单窗口, 每通道 Y 轴/Vdiv/offset)
 * 左下部: 显示变量列表
 * 右侧:   监控变量栏(树形统计)
 * 顶部:   连接/采样工具栏 + 变量选择入口
 */
#include <QMainWindow>
#include <QTimer>
#include "link/link_driver.h"
#include "core/sample_engine.h"
#include "core/monitor_model.h"
#include "gui/plot_widget.h"
#include "gui/monitor_dock.h"
#include "gui/display_list.h"

class QComboBox;
class QSpinBox;
class QLabel;
class QAction;
class QLineEdit;
class VariableDialog;
class QSplitter;

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

private:
    void buildUi();
    void applyLightTheme();   /* 浅色主题; 波形区独立保持暗色 */
    void reconfigureEngine();
    void updateStartAction();
    void setConnControlsEnabled(bool on);  /* 通讯接口/目标/采样间隔/TCK 连接期间禁用 */
    void syncPlotChannels();
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
    QSpinBox*    intervalSpin_ = nullptr;
    QLabel*      statusConn_ = nullptr;
    QLabel*      statusRate_ = nullptr;
    QLabel*      statusMsg_ = nullptr;   /* 状态栏左侧动态消息 */
    QAction*     actConn_ = nullptr;
    QAction*     actDisc_ = nullptr;
    QAction*     actStart_ = nullptr;   /* 连续采样: 文本在 连续采样/停止采样/停止中… 间切换 */
    QAction*     actSingle_ = nullptr;  /* 单次采样: 一拍即停 */
    PlotWidget*  plot_ = nullptr;
    MonitorDock* monDock_ = nullptr;
    DisplayList* dispList_ = nullptr;
};

#endif /* MAINWINDOW_H */
