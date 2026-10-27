#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "variable_dialog.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QHash>
#include <QSettings>
#include <QSet>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyleFactory>
#include <QToolBar>
#include <functional>

MainWindow::MainWindow() {
    ui = new Ui::MainWindow;
    ui->setupUi(this);
    applyLightTheme();

    model_ = new MonitorModel(this);
    engine_ = new SampleEngine();
    engineThread_ = new QThread(this);
    engine_->moveToThread(engineThread_);
    connect(engineThread_, &QThread::finished, engine_, &QObject::deleteLater);
    connect(engine_, &SampleEngine::batchReady, this, &MainWindow::onBatchReady,
            Qt::QueuedConnection);
    connect(engine_, &SampleEngine::statsUpdated, this, &MainWindow::onStats,
            Qt::QueuedConnection);
    connect(engine_, &SampleEngine::stopped, this, &MainWindow::onEngineStopped,
            Qt::QueuedConnection);
    engineThread_->start();

    buildUi();

    /* 模型 -> 界面 */
    connect(model_, &MonitorModel::changed, this, &MainWindow::onModelChanged);
    connect(model_, &MonitorModel::leafShownChanged,
            this, &MainWindow::onLeafShownChanged);

    /* 变量选择对话框 -> 模型 */
    varDialog_ = new VariableDialog(this);
    connect(varDialog_, &VariableDialog::monitorAdded,
            this, &MainWindow::onMonitorAdded);
    connect(varDialog_, &VariableDialog::monitorRemoved,
            this, &MainWindow::onMonitorRemoved);
    connect(varDialog_, &VariableDialog::displayToggled,
            this, &MainWindow::onDisplayToggled);

    /* 恢复上次会话: 工具栏参数 + 变量对话框(重导入文件 + 重放勾选) */
    {
        QSettings s;
        const QString be = s.value("toolbar/backend").toString();
        if (!be.isEmpty())
            for (int i = 0; i < backendBox_->count(); ++i)
                if (backendBox_->itemData(i).toString() == be) {
                    backendBox_->setCurrentIndex(i); break;
                }
        const int di = ui->deviceBox->findText(s.value("toolbar/device").toString());
        if (di >= 0) ui->deviceBox->setCurrentIndex(di);
        if (s.contains("toolbar/interval"))
            intervalSpin_->setValue(s.value("toolbar/interval").toInt());
        const int ti = ui->tckBox->findText(s.value("toolbar/tck").toString());
        if (ti >= 0) ui->tckBox->setCurrentIndex(ti);
        varDialog_->restoreState(s);
    }

    /* 监控栏右键 -> 对话框 */
    connect(monDock_, &MonitorDock::addVariableRequested,
            this, &MainWindow::onOpenVariableDialog);
    connect(monDock_, &MonitorDock::removeRootRequested,
            this, &MainWindow::onRemoveRoot);

    /* 绘制列表 -> 波形垂直参数 */
    connect(dispList_, &DisplayList::channelParamChanged,
            this, [this](const QString& path, double div, double offset, bool autoMode) {
        plot_->setChannelDivOffset(path, div, offset, autoMode);
    });

    uiTimer_.setInterval(100);
    connect(&uiTimer_, &QTimer::timeout, this, &MainWindow::onUiTick);
    uiTimer_.start();

    setStatus("就绪 - 选择后端并连接; 点「变量…」选择监控变量");
}

/* ---------------- UI ---------------- */
/* 浅色主题: 除波形区(PlotWidget 独立暗色)外, 全部白/浅灰底 + 深色文字 */
void MainWindow::applyLightTheme() {
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(0xF3, 0xF4, 0xF6), base(0xFF, 0xFF, 0xFF),
        text(0x1F, 0x23, 0x28), hl(0x0A, 0x6C, 0xFF), btn(0xE6, 0xE8, 0xEB);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, btn);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, hl);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x9A, 0xA0, 0xA6));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x8B, 0x94, 0x9E));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x8B, 0x94, 0x9E));
    QApplication::setPalette(p);
    setStyleSheet(
        "QToolBar{background:#f3f4f6;border-bottom:1px solid #d8dbe0;spacing:6px;}"
        "QToolButton{color:#1f2328;padding:4px 10px;}"
        "QToolButton:hover{background:#e0e3e8;border-radius:4px;}"
        "QToolButton:disabled{color:#9aa0a6;}"
        "QComboBox,QSpinBox,QLineEdit{background:#ffffff;color:#1f2328;"
        "border:1px solid #c9ced6;border-radius:4px;padding:3px 8px;min-height:20px;}"
        "QComboBox:disabled,QSpinBox:disabled,QLineEdit:disabled{color:#9aa0a6;}"
        "QTreeWidget,QListWidget{background:#ffffff;color:#1f2328;border:none;font-size:13px;}"
        /* 交替行色不用 QSS(::item:alternate 会盖住 item 的 BackgroundRole,
         * 如变量对话框文件行的淡蓝色), 交给 palette::AlternateBase */
        "QTreeWidget::item:selected,QListWidget::item:selected{background:#cfe3ff;color:#0b57d0;}"
        "QHeaderView::section{background:#f3f4f6;color:#57606a;border:none;"
        "border-right:1px solid #d8dbe0;padding:4px 6px;}"
        "QTreeWidget::indicator,QListWidget::indicator{width:14px;height:14px;}"
        "QTreeWidget::indicator:unchecked{border:1px solid #8b949e;"
        "border-radius:3px;background:#ffffff;}"
        "QTreeWidget::indicator:checked{border:1px solid #4d8dff;"
        "border-radius:3px;background:#0a6cff;image:url(:/icons/check.svg);}"
        "QPushButton{background:#ffffff;color:#1f2328;border:1px solid #c9ced6;"
        "border-radius:4px;padding:4px 14px;}"
        "QPushButton:hover{background:#e6e8eb;}"
        "QPushButton:disabled{color:#9aa0a6;}"
        "QDockWidget::title{background:#f3f4f6;color:#57606a;padding:5px;}"
        "QSplitter::handle{background:#d8dbe0;}"
        "QStatusBar{background:#f3f4f6;color:#57606a;}");
}

/* 退出时保存会话: 工具栏参数 + 变量对话框导入文件与勾选状态 */
void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue("toolbar/backend", backendBox_->currentData().toString());
    s.setValue("toolbar/device", ui->deviceBox->currentText());
    s.setValue("toolbar/interval", intervalSpin_->value());
    s.setValue("toolbar/tck", ui->tckBox->currentText());
    varDialog_->saveState(s);
    QMainWindow::closeEvent(e);
}

MainWindow::~MainWindow() {
    /* 优雅退出: 先停 worker(可能阻塞至本次读取返回), 再收线程,
     * 否则 QThread 带着运行中的线程被析构会 qFatal 崩溃 */
    if (engine_) {
        engine_->requestStop();
        engine_->stop();
    }
    if (engineThread_) {
        engineThread_->quit();
        engineThread_->wait();
    }
    delete ui;
}

/* setupUi 之后的代码侧收尾: 控件指针绑定/动态内容/代码专属属性 */
void MainWindow::buildUi() {
    backendBox_ = ui->backendBox;
    intervalSpin_ = ui->intervalSpin;
    statusConn_ = ui->statusConn;
    statusRate_ = ui->statusRate;
    /* .ui 里拖入 QStatusBar 的控件只设了 parent, 不会自动加入布局:
     * 显式挂到状态栏(消息在左, 连接/速率常驻右侧), 否则悬浮在 (0,0) 与消息重叠 */
    statusBar()->removeWidget(statusConn_);
    statusBar()->removeWidget(statusRate_);
    statusBar()->addWidget(statusMsgLabel());
    statusBar()->addPermanentWidget(statusConn_);
    statusBar()->addPermanentWidget(statusRate_);
    actConn_ = ui->actConn;
    actDisc_ = ui->actDisc;
    actStart_ = ui->actStart;
    actSingle_ = ui->actSingle;
    plot_ = ui->plot;
    monDock_ = ui->monDock;
    dispList_ = ui->dispList;

    /* 后端显示名映射(实际后端名存 userData, 连接时取回) */
    const QStringList backends = LinkDriver::scanDrivers();
    if (backends.isEmpty()) {
        backendBox_->addItem("(未找到驱动)");
    } else {
        static const QHash<QString, QString> dispNames{
            {"gslink", "GS32-Link"}, {"jlink", "JLink"},
            {"sim", "Simulation"}, {"openocd", "OpenOCD"}};
        for (const QString& b : backends)
            backendBox_->addItem(dispNames.value(b, b), b);
    }

    /* 波形:绘制列表 = 3:1 */
    ui->mainSplitter->setStretchFactor(0, 3);
    ui->mainSplitter->setStretchFactor(1, 1);
    ui->mainSplitter->setSizes({540, 180});

    monDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    monDock_->setMonitorModel(model_);
    dispList_->setMonitorModel(model_);

    connect(actConn_, &QAction::triggered, this, &MainWindow::onConnectClicked);
    connect(actDisc_, &QAction::triggered, this, &MainWindow::onDisconnectClicked);
    connect(actStart_, &QAction::triggered, this, &MainWindow::onStartClicked);
    connect(actSingle_, &QAction::triggered, this, &MainWindow::onSingleClicked);
    connect(ui->actVar, &QAction::triggered, this, &MainWindow::onOpenVariableDialog);
}

/* ---------------- 连接 ---------------- */
void MainWindow::onConnectClicked() {
    const QString backend = backendBox_->currentData().toString();
    if (backend.isEmpty()) return;
    if (!link_.load(backend)) { setStatus("加载驱动失败: " + link_.lastError(), true); return; }
    gs32_config_t cfg{};
    const QByteArray bBackend = backend.toUtf8();
    /* 目标下拉框: RV32 → riscv32(驱动内部识别名) */
    const QString devText = ui->deviceBox->currentText();
    const QByteArray bDevice = (devText == "RV32" ? QString("riscv32") : devText).toUtf8();
    const QByteArray bIf = backend == "gslink"
                         ? QByteArray("jtag") : QByteArray("swd");
    cfg.backend = bBackend.constData();
    cfg.device = bDevice.constData();
    cfg.interface = bIf.constData();
    const auto* tck = findChild<QComboBox*>("tckBox");
    cfg.speed_khz = tck ? uint32_t(tck->currentText().toInt() * 1000) : 4000;
    if (!link_.open(cfg)) { setStatus("连接失败: " + link_.lastErrorText(), true); link_.close(); return; }
    connectedBackend_ = backend;
    actConn_->setEnabled(false); actDisc_->setEnabled(true);
    setConnControlsEnabled(false);   /* 连接期间锁定通讯接口/目标/采样间隔/TCK */
    updateStartAction();   /* 连续/单次采样按钮同条件使能 */
    statusConn_->setText("已连接: " + backendBox_->currentText());
    setStatus(QString("已连接 %1").arg(backendBox_->currentText()));
}

void MainWindow::onDisconnectClicked() {
    if (engine_->running()) {
        /* 非阻塞停止: 等读取返回后再关链路(否则读线程仍在用 FT 句柄) */
        pendingDisconnect_ = true;
        onStopClicked();
        setStatus("等待采样线程退出…");
        return;
    }
    link_.close();
    connectedBackend_.clear();
    actConn_->setEnabled(true); actDisc_->setEnabled(false);
    actStart_->setEnabled(false); actSingle_->setEnabled(false);
    setConnControlsEnabled(true);
    statusConn_->setText("未连接");
    setStatus("已断开");
}

/* ---------------- 采样(单按钮: 开始采样 ⇄ 停止采样) ---------------- */
void MainWindow::onStartClicked() {
    if (stopping_) return;                    /* 停止中: 按钮已禁用, 防重入 */
    if (engine_->running()) {                 /* 采样中 → 执行停止 */
        stopping_ = true;
        /* 非阻塞: worker 最迟在本次 gs32_read 返回后退出并发 stopped(),
         * 绝不在 UI 线程 join(读取耗时可达秒级, 会冻结界面) */
        engine_->requestStop();
        actStart_->setEnabled(false);
        actSingle_->setEnabled(false);
        actStart_->setText("停止中…");
        /* 采样间隔等连接参数在断开连接前保持禁用(setConnControlsEnabled) */
        statusRate_->clear();
        if (!pendingDisconnect_) setStatus("正在停止采样…");
        return;
    }
    engine_->configure(&link_, intervalSpin_->value(), model_);
    QString err;
    if (!engine_->start(&err)) { setStatus("启动采样失败: " + err, true); return; }
    actStart_->setText("停止采样");
    actSingle_->setEnabled(false);
    intervalSpin_->setEnabled(false);
    setStatus(QString("采样中: %1 叶子 @ %2ms").arg(model_->leafCount()).arg(intervalSpin_->value()));
}

/* 单次采样: 读取一拍即自动停止; 期间两按钮禁用, 单次按钮显示等待状态 */
void MainWindow::onSingleClicked() {
    if (stopping_ || engine_->running()) return;
    engine_->configure(&link_, intervalSpin_->value(), model_);
    QString err;
    if (!engine_->startSingle(&err)) { setStatus("启动采样失败: " + err, true); return; }
    actStart_->setEnabled(false);
    actSingle_->setEnabled(false);
    actSingle_->setText("等待停止");
    setStatus(QString("单次采样中: %1 叶子").arg(model_->leafCount()));
}

void MainWindow::onStopClicked() { onStartClicked(); }   /* 旧入口: 转发到切换逻辑 */

/* worker 线程已完全退出: 在此做所有需要引擎静止的操作 */
void MainWindow::onEngineStopped() {
    stopping_ = false;
    actStart_->setText("连续采样");
    actSingle_->setText("单次采样");
    if (pendingDisconnect_) {
        pendingDisconnect_ = false;
        link_.close();
        connectedBackend_.clear();
        actConn_->setEnabled(true); actDisc_->setEnabled(false);
        actStart_->setEnabled(false); actSingle_->setEnabled(false);
        setConnControlsEnabled(true);
        statusConn_->setText("未连接");
        setStatus("已断开");
        return;
    }
    updateStartAction();
    if (pendingRestart_) {
        pendingRestart_ = false;
        engine_->configure(&link_, intervalSpin_->value(), model_);
        QString err;
        if (model_->leafCount() && !engine_->start(&err)) {
            updateStartAction();
            setStatus("重启采样失败: " + err, true);
        } else if (engine_->running()) {
            actStart_->setText("停止采样");
            actSingle_->setEnabled(false);
            intervalSpin_->setEnabled(false);
            setStatus(QString("采样中: %1 叶子 @ %2ms")
                          .arg(model_->leafCount()).arg(intervalSpin_->value()));
        }
        return;
    }
    setStatus("采样已停止");
}

void MainWindow::updateStartAction() {
    /* stopping_ 期间禁止启动: configure 会重建 regions_, 旧 worker 还在读;
     * 连续/单次两按钮同条件使能 */
    const bool ok = !stopping_ && !engine_->running() &&
                    !connectedBackend_.isEmpty() && model_->leafCount() > 0;
    actStart_->setEnabled(ok);
    actSingle_->setEnabled(ok);
}

/* 连接参数区: 连接成功后禁用(防止运行中切换接口/目标/参数), 断开后恢复 */
void MainWindow::setConnControlsEnabled(bool on) {
    backendBox_->setEnabled(on);
    ui->deviceBox->setEnabled(on);
    intervalSpin_->setEnabled(on);
    ui->tckBox->setEnabled(on);
}

void MainWindow::reconfigureEngine() {
    if (engine_->running()) {
        /* 采样中模型变更: 先非阻塞停止, stopped() 回调里再重启 */
        pendingRestart_ = true;
        engine_->requestStop();
        /* 连接期间采样间隔保持禁用, 重启参数沿用当前值 */
        actStart_->setEnabled(false);
        actSingle_->setEnabled(false);
        actStart_->setText("停止中…");
        return;
    }
    updateStartAction();
}

/* ---------------- 变量选择 ---------------- */
void MainWindow::onOpenVariableDialog() {
    varDialog_->show();
    varDialog_->raise();
    varDialog_->activateWindow();
}

namespace {
void collectLeafPaths(const DwarfVar& v, QSet<QString>& out) {
    if (v.isStruct) {
        for (const auto& m : v.members) collectLeafPaths(m, out);
    } else {
        out.insert(v.name);
    }
}
} // namespace

void MainWindow::onMonitorAdded(const DwarfVar& v) {
    if (!v.isStruct) {
        const int li = model_->leafIndexByPath(v.name);
        if (li >= 0) {
            /* 已存在(可能是绘制专用叶子) → 升级为监控 */
            model_->setLeafMonitored(li, true);
            setStatus(QString("已监控: %1").arg(v.name));
            return;
        }
        for (int i = 0; i < model_->rootCount(); ++i)
            if (model_->root(i).name == v.name) return;
        model_->addDwarfRoot(v);
    } else {
        /* 结构体: 先移除已被单独监控/绘制的同名叶子根, 再整体监控 */
        QSet<QString> memberPaths;
        collectLeafPaths(v, memberPaths);
        for (int i = model_->rootCount() - 1; i >= 0; --i) {
            const auto& r = model_->root(i);
            if (!r.isStruct && memberPaths.contains(r.name))
                model_->removeRoot(i);
        }
        model_->addDwarfRoot(v);
    }
    setStatus(QString("已监控: %1").arg(v.name));
}

void MainWindow::onMonitorRemoved(const QString& rootPath) {
    for (int i = 0; i < model_->rootCount(); ++i)
        if (model_->root(i).name == rootPath) { model_->removeRoot(i); return; }
}

void MainWindow::onDisplayToggled(const VariableDef& d, bool on) {
    int li = model_->leafIndexByPath(d.name);
    if (on) {
        if (li < 0) li = model_->addDrawOnlyRoot(d);   /* 独立绘制: 不进监控列表 */
        if (li >= 0) model_->setLeafShown(li, true);
    } else if (li >= 0) {
        model_->setLeafShown(li, false);
    }
}

void MainWindow::onRemoveRoot(int rootIdx) {
    model_->removeRoot(rootIdx);
}

/* ---------------- 模型 -> 界面 ---------------- */
void MainWindow::onModelChanged() {
    monDock_->rebuild();
    dispList_->rebuild();
    syncPlotChannels();
    reconfigureEngine();
    updateStartAction();
}

void MainWindow::onLeafShownChanged(int) {
    dispList_->rebuild();
    syncPlotChannels();
}

void MainWindow::syncPlotChannels() {
    QSet<QString> want;
    for (int li = 0; li < model_->leafCount(); ++li) {
        const auto& lf = model_->leaf(li);
        if (lf.shown) {
            want.insert(lf.path);
            if (!plot_->hasChannel(lf.path))
                plot_->addChannel(lf.path, model_->root(lf.rootIdx).color);
            else
                plot_->setChannelColor(lf.path, model_->root(lf.rootIdx).color);
        }
    }
    for (const QString& name : plot_->channelNames())
        if (!want.contains(name))
            plot_->removeChannel(name);
}

/* ---------------- 数据流 ---------------- */
void MainWindow::onBatchReady(const SampleBatch& batch) {
    if (batch.tNs.isEmpty()) return;
    const double tLastSec = batch.tNs.last() / 1e9;
    for (int k = 0; k < batch.values.size() && k < model_->leafCount(); ++k) {
        model_->updateLeafValue(k, batch.values[k].last());
        const auto& lf = model_->leaf(k);
        if (!lf.shown) continue;
        for (int i = 0; i < batch.tNs.size(); ++i)
            plot_->appendData(lf.path, batch.tNs[i] / 1e9, batch.values[k][i]);
    }
    plot_->trimBefore(tLastSec);
    plot_->replot();
}

void MainWindow::onStats(const EngineStats& st) {
    if (!st.error.isEmpty()) { setStatus("读取失败: " + st.error, true); return; }
    /* 抖动插桩: wake = 线程唤醒滞后峰值, write = FT_Write 提交耗时峰值(1s 窗口) */
    const QString jitter = QString(" | 抖动 %1ms | 写入 %2ms")
                               .arg(st.wakeLagMs, 0, 'f', 1)
                               .arg(st.writeMs >= 0 ? QString::number(st.writeMs, 'f', 2)
                                                    : QString("—"));
    statusRate_->setText(QString("实际 %1 Hz | 读取 %2 ms%3%4")
                             .arg(st.actualHz, 0, 'f', 1)
                             .arg(st.readMs, 0, 'f', 2)
                             .arg(jitter)
                             .arg(st.overrun ? " | 超载!" : ""));
}

void MainWindow::onUiTick() {
    monDock_->refresh();
}

/* 状态栏左侧消息 label(懒创建, buildUi 与 setStatus 共用) */
QLabel* MainWindow::statusMsgLabel() {
    if (!statusMsg_) {
        statusMsg_ = new QLabel(this);
        statusMsg_->setObjectName("statusMsg");
    }
    return statusMsg_;
}

void MainWindow::setStatus(const QString& msg, bool isError) {
    QLabel* lb = statusMsgLabel();
    lb->setStyleSheet(isError ? "color:#ff5c7a;" : "");
    lb->setText(msg);
}
