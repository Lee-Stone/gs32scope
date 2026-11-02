#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "core/variable_writer.h"
#include "gui/dashboard_widget.h"
#include "gui/write_dialog.h"
#include "variable_dialog.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHash>
#include <QInputDialog>
#include <QKeySequence>
#include <QSettings>
#include <QShortcut>
#include <QSet>
#include <QSignalBlocker>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyleFactory>
#include <QStyleHints>
#include <QToolBar>
#include <algorithm>
#include <cmath>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#undef interface   /* rpcndr.h 把 interface 定义成 struct, 与 gs32_config_t::interface 冲突 */
#include <cwchar>
#endif

namespace {
/* "tcp:127.0.0.1:7000" / "COM3@115200" / 原始 JSON -> 驱动 extra JSON */
QByteArray sciConfigToJson(const QString& text) {
    const QString t = text.trimmed();
    if (t.startsWith('{')) return t.toUtf8();
    if (t.startsWith("tcp:", Qt::CaseInsensitive)) {
        const QString hp = t.mid(4).trimmed();
        const int colon = hp.lastIndexOf(':');
        const QString host = colon > 0 ? hp.left(colon) : QStringLiteral("127.0.0.1");
        int port = colon > 0 ? hp.mid(colon + 1).toInt() : 7000;
        if (port <= 0) port = 7000;
        return QString("{\"transport\":\"tcp\",\"host\":\"%1\",\"port\":%2}")
            .arg(host).arg(port).toUtf8();
    }
    QString com = t, baud = QStringLiteral("115200");
    const int at = t.indexOf('@');
    if (at > 0) { com = t.left(at).trimmed(); baud = t.mid(at + 1).trimmed(); }
    bool ok = false;
    const int b = baud.toInt(&ok);
    return QString("{\"transport\":\"serial\",\"com\":\"%1\",\"baud\":%2}")
        .arg(com).arg(ok && b > 0 ? b : 115200).toUtf8();
}

/* 枚举本机串口: QueryDosDevice 列出的 COMx 设备(含虚拟串口) */
QStringList availableSerialPorts() {
    QStringList ports;
#ifdef Q_OS_WIN
    wchar_t buf[65536] = {};
    const DWORD n = QueryDosDeviceW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    if (n != 0) {
        for (const wchar_t* p = buf; *p; p += wcslen(p) + 1) {
            const QString name = QString::fromWCharArray(p);
            if (!name.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive)) continue;
            bool numeric = false;
            name.mid(3).toInt(&numeric);
            if (numeric) ports << name;
        }
    }
    ports.removeDuplicates();
    std::sort(ports.begin(), ports.end(), [](const QString& a, const QString& b) {
        return a.mid(3).toInt() < b.mid(3).toInt();   /* COM9 在 COM10 前 */
    });
#endif
    return ports;
}
} // namespace

MainWindow::MainWindow() {
    ui = new Ui::MainWindow;
    ui->setupUi(this);

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
    connect(model_, &MonitorModel::leafColorChanged, this, [this](int) {
        monDock_->rebuild();
        syncPlotChannels();
    });
    connect(model_, &MonitorModel::leavesShownChanged, this, [this] {
        syncPlotChannels();
    });

    /* 变量选择对话框 -> 模型 */
    varDialog_ = new VariableDialog(this);
    connect(varDialog_, &VariableDialog::monitorAdded,
            this, &MainWindow::onMonitorAdded);
    connect(varDialog_, &VariableDialog::monitorRemoved,
            this, &MainWindow::onMonitorRemoved);
    connect(varDialog_, &VariableDialog::elfReloadStarted,
            this, &MainWindow::onElfReloadStarted);
    connect(varDialog_, &VariableDialog::elfReloadFinished,
            this, &MainWindow::onElfReloadFinished);

    /* 恢复上次会话: 工具栏参数 + 变量对话框(重导入文件 + 重放勾选)。
     * 首次启动(未完成过正常关闭)保持空白仪表盘/波形; 正常关闭后写入 initialized. */
    bool restoredDockState = false;
    {
        /* --fresh: 自动化测试用, 模拟首次启动(清空已保存会话) */
        if (QApplication::arguments().contains("--fresh")) {
            QSettings().clear();
        }
        QSettings s;
        const bool initialized = s.value("session/initialized", false).toBool();
        const QString be = s.value("toolbar/backend").toString();
        if (!be.isEmpty())
            for (int i = 0; i < backendBox_->count(); ++i)
                if (backendBox_->itemData(i).toString() == be) {
                    backendBox_->setCurrentIndex(i); break;
                }
        /* 先按后端准备好第二组参数(TCK/波特率)与目标列表, 再恢复具体选项 */
        applyBackendUi(backendBox_->currentData().toString());
        const int di = ui->deviceBox->findText(s.value("toolbar/device").toString());
        if (di >= 0) ui->deviceBox->setCurrentIndex(di);
        int rate = s.value("toolbar/rate", -1).toInt();
        if (rate <= 0) {
            /* 旧版本存的是采样间隔 ms */
            const int ms = s.value("toolbar/interval", 20).toInt();
            rate = std::clamp(ms > 0 ? 1000 / ms : 50, 1, 10000);
        }
        const int envRate = qEnvironmentVariableIntValue("GS32SCOPE_RATE");
        if (envRate > 0) rate = envRate;   /* 自动化/测试入口 */
        intervalSpin_->setValue(std::clamp(rate, 1, 10000));
        const int ti = ui->tckBox->findText(s.value("toolbar/tck").toString());
        if (ti >= 0) ui->tckBox->setCurrentIndex(ti);
        if (initialized) {
            varDialog_->restoreState(s);
            restoreSessionLeaves(s);

            dashboard_->restoreState(s);
            const QByteArray dockState = s.value("window/dockState").toByteArray();
            if (!dockState.isEmpty()) restoredDockState = restoreState(dockState);
        } else {
            /* 首次启动: 仪表盘/波形/监控变量全部为空, 只记忆用户后续添加的内容 */
            dashboard_->clearAll();
            plot_->clearData();
            history_.clear();
            model_->clear();
            timelineStarted_ = false;
            timelineRawBaseNs_ = timelineLogicalBaseNs_ = lastLogicalNs_ = 0;
        }
        themeMode_ = s.value("appearance/theme", "system").toString();
        applyTheme(themeMode_);
    }
    if (!restoredDockState)
        QTimer::singleShot(0, this, &MainWindow::defaultDockLayout);

    /* 跟随系统: Windows 深浅色切换时实时重刷 */
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this](Qt::ColorScheme) {
        if (themeMode_ == "system") applyTheme("system");
    });

    /* --import <elf|svd>: 启动时静默导入并监控全部顶层变量/外设 */
    {
        const QStringList args = QApplication::arguments();
        const int ii = args.indexOf("--import");
        if (ii >= 0 && ii + 1 < args.size()) {
            const QString path = args[ii + 1];
            const bool isSvd = path.endsWith(".svd", Qt::CaseInsensitive) ||
                               path.endsWith(".xml", Qt::CaseInsensitive);
            const bool ok = isSvd ? varDialog_->importSvd(path)
                                  : varDialog_->importElf(path);
            if (ok) {
                varDialog_->monitorAll();
                setStatus("已导入并监控: " + path);
            } else {
                setStatus("导入失败: " + path, true);
            }
        }
    }

    /* 监控栏右键 -> 对话框 */
    connect(monDock_, &MonitorDock::removeRootRequested,
            this, &MainWindow::onRemoveRoot);
    connect(monDock_, &MonitorDock::writeRequested,
            this, &MainWindow::onWriteLeaf);

    uiTimer_.setInterval(100);
    connect(&uiTimer_, &QTimer::timeout, this, &MainWindow::onUiTick);
    uiTimer_.start();

    plotTimer_.setInterval(33);
    connect(&plotTimer_, &QTimer::timeout, this, [this] {
        if (plot_->hasData()) plot_->replot();
    });
    plotTimer_.start();
    connect(intervalSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { updatePlotRefreshRate(); });

    setStatus("就绪 - 选择后端并连接; 点「导入变量…」选择监控变量");
    updatePlotRefreshRate();
}

/* ---------------- UI / 主题 ---------------- */
namespace {
QString makeStyleSheet(bool dark) {
    const QString bg      = dark ? "#1e2228" : "#f3f4f6";
    const QString panel   = dark ? "#23272e" : "#ffffff";
    const QString panel2  = dark ? "#2a2f37" : "#f3f4f6";
    const QString text    = dark ? "#e6e9ee" : "#1f2328";
    const QString muted   = dark ? "#aeb7c4" : "#57606a";
    const QString border  = dark ? "#3a404a" : "#d8dbe0";
    const QString border2 = dark ? "#4a5260" : "#c9ced6";
    const QString disabled= dark ? "#6b7482" : "#9aa0a6";
    const QString btn     = dark ? "#2c313a" : "#ffffff";
    const QString btnHover= dark ? "#3a4250" : "#e6e8eb";
    const QString spinBtn = dark ? "#343a45" : "#eef1f5";
    const QString sel     = dark ? "#2f4a7a" : "#cfe3ff";
    const QString selText = dark ? "#dbeafe" : "#0b57d0";
    const QString arrowUp   = dark ? ":/icons/arrow_up_light.svg" : ":/icons/arrow_up.svg";
    const QString arrowDown = dark ? ":/icons/arrow_down_light.svg" : ":/icons/arrow_down.svg";

    QString q;
    q += "QMenuBar{background:" + bg + ";color:" + text + ";border-bottom:1px solid " + border + ";}";
    q += "QMenuBar::item{padding:5px 14px;background:transparent;border-radius:4px;}";
    q += "QMenuBar::item:selected{background:" + sel + ";color:" + selText + ";}";
    q += "QMenu{background:" + panel + ";color:" + text + ";border:1px solid " + border2 + ";}";
    q += "QMenu::item{padding:6px 26px;}";
    q += "QMenu::item:selected{background:" + sel + ";color:" + selText + ";}";
    q += "QMenu::separator{height:1px;background:" + border + ";margin:4px 8px;}";
    q += "QToolBar{background:" + bg + ";border-bottom:1px solid " + border + ";spacing:6px;padding:3px;}";
    q += "QToolBar QLabel{font-size:14px;}";
    q += "QToolButton{background:" + btn + ";color:" + text + ";border:1px solid " + border2 +
         ";border-radius:4px;padding:4px 10px;}";
    q += "QToolButton:hover{background:" + sel + ";border-color:#5b8def;}";
    q += "QToolButton:pressed{background:" + sel + ";}";
    q += "QToolButton:disabled{background:" + bg + ";color:" + disabled + ";border-color:" + border + ";}";
    q += "QToolButton::menu-indicator{image:url(" + arrowDown +
         ");width:9px;height:9px;subcontrol-origin:padding;subcontrol-position:right center;right:3px;}";
    q += "QComboBox,QSpinBox,QLineEdit{background:" + panel + ";color:" + text + ";border:1px solid " +
         border2 + ";border-radius:4px;padding:3px 8px;min-height:22px;font-size:14px;}";
    q += "QComboBox{padding-right:24px;}";
    q += "QSpinBox#intervalSpin{padding-left:0px;padding-right:0px;}";
    q += "QSpinBox#intervalSpin::up-button,QSpinBox#intervalSpin::down-button{width:0px;height:0px;border:none;background:transparent;}";
    q += "QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:top right;width:24px;"
         "background:" + spinBtn + ";border-left:1px solid " + border2 +
         ";border-top-right-radius:4px;border-bottom-right-radius:4px;}";
    q += "QComboBox::down-arrow{image:url(" + arrowDown + ");width:12px;height:12px;}";
    q += "QSpinBox{padding-right:24px;}";
    q += "QSpinBox::up-button{subcontrol-origin:border;subcontrol-position:top right;width:22px;"
         "height:50%;background:" + spinBtn + ";border-left:1px solid " + border2 +
         ";border-top-right-radius:4px;}";
    q += "QSpinBox::down-button{subcontrol-origin:border;subcontrol-position:bottom right;width:22px;"
         "height:50%;background:" + spinBtn + ";border-left:1px solid " + border2 +
         ";border-bottom-right-radius:4px;}";
    q += "QSpinBox::up-arrow{image:url(" + arrowUp + ");width:10px;height:10px;}";
    q += "QSpinBox::down-arrow{image:url(" + arrowDown + ");width:10px;height:10px;}";
    q += "QToolButton#spinUp,QToolButton#spinDown{background:" + spinBtn + ";color:" + text +
         ";border:1px solid " + border2 + ";border-radius:3px;padding:0;"
         "font-weight:700;font-size:10px;}";
    q += "QToolButton#spinUp:hover,QToolButton#spinDown:hover{background:" + sel + ";}";
    q += "QComboBox:disabled,QSpinBox:disabled,QLineEdit:disabled{color:" + disabled + ";}";
    q += "QTreeWidget,QTableWidget,QListWidget,QTableView{background:" + panel + ";color:" + text +
         ";border:none;font-size:13px;gridline-color:" + border + ";}";
    q += "QTreeWidget::item:selected,QTableWidget::item:selected,QListWidget::item:selected{background:" +
         sel + ";color:" + selText + ";}";
    q += "QHeaderView::section{background:" + panel2 + ";color:" + muted +
         ";border:none;border-right:1px solid " + border + ";padding:4px 6px;}";
    q += "QTableCornerButton::section{background:" + panel2 + ";border:none;}";
    q += "QTreeWidget::indicator,QListWidget::indicator{width:14px;height:14px;}";
    q += "QTreeWidget::indicator:unchecked{border:1px solid " + border2 +
         ";border-radius:3px;background:" + panel + ";}";
    q += "QTreeWidget::indicator:checked{border:1px solid #4d8dff;border-radius:3px;"
         "background:#0a6cff;image:url(:/icons/check.svg);}";
    q += "QPushButton{background:" + btn + ";color:" + text + ";border:1px solid " + border2 +
         ";border-radius:4px;padding:4px 14px;}";
    q += "QPushButton:hover{background:" + btnHover + ";}";
    q += "QPushButton:disabled{color:" + disabled + ";background:" + bg + ";border-color:" + border + ";}";
    q += "QDockWidget{color:" + text + ";font-size:14px;font-weight:600;}";
    q += "QDockWidget::title{background:" + panel2 + ";color:" + muted +
         ";padding:5px;text-align:center;}";
    q += "QPlainTextEdit#logView{background:" + panel + ";color:" + text +
         ";border:none;font-size:12px;selection-background-color:" + sel + ";}";
    q += "QTabWidget::pane{border:1px solid " + border + ";background:" + panel + ";}";
    q += "QTabBar::tab{background:" + btn + ";color:" + text + ";border:1px solid " + border2 +
         ";border-bottom:none;padding:5px 16px;margin-right:2px;"
         "border-top-left-radius:4px;border-top-right-radius:4px;}";
    q += "QTabBar::tab:selected{background:" + panel + ";color:#2f7bff;font-weight:600;}";
    q += "QTabBar::tab:hover{background:" + sel + ";}";
    q += "QSplitter::handle{background:" + border + ";}";
    q += "QStatusBar{background:" + bg + ";color:" + muted + ";}";
    q += "QToolTip{background:" + panel + ";color:" + text + ";border:1px solid " + border2 + ";}";
    q += "QComboBox QAbstractItemView{background:" + panel + ";color:" + text + ";border:1px solid " +
         border2 + ";selection-background-color:" + sel + ";selection-color:" + selText + ";}";
    q += "QLineEdit#valueEdit{background:" + panel2 + ";border:1px solid " + border2 +
         ";border-radius:4px;padding:2px 6px;min-height:22px;}";
    q += "QPushButton#valueApply{padding:3px 8px;min-height:24px;}";
    return q;
}
} // namespace

bool MainWindow::systemPrefersDark() const {
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) return true;
    if (scheme == Qt::ColorScheme::Light) return false;
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                  QSettings::NativeFormat);
    return reg.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
}

void MainWindow::applyTheme(const QString& mode) {
    themeMode_ = (mode == "light" || mode == "dark") ? mode : QStringLiteral("system");
    const bool dark = themeMode_ == "dark" ||
                      (themeMode_ == "system" && systemPrefersDark());

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    if (dark) {
        p.setColor(QPalette::Window, QColor("#1e2228"));
        p.setColor(QPalette::WindowText, QColor("#e6e9ee"));
        p.setColor(QPalette::Base, QColor("#23272e"));
        p.setColor(QPalette::AlternateBase, QColor("#262b33"));
        p.setColor(QPalette::Text, QColor("#e6e9ee"));
        p.setColor(QPalette::Button, QColor("#2c313a"));
        p.setColor(QPalette::ButtonText, QColor("#e6e9ee"));
        p.setColor(QPalette::Highlight, QColor("#2f4a7a"));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::ToolTipBase, QColor("#23272e"));
        p.setColor(QPalette::ToolTipText, QColor("#e6e9ee"));
        p.setColor(QPalette::PlaceholderText, QColor("#8a94a6"));
        p.setColor(QPalette::Mid, QColor("#3a404a"));
        p.setColor(QPalette::Midlight, QColor("#343a45"));
        p.setColor(QPalette::Light, QColor("#4a5260"));
        p.setColor(QPalette::Dark, QColor("#15181d"));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor("#6b7482"));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#6b7482"));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#6b7482"));
    } else {
        p.setColor(QPalette::Window, QColor(0xF3, 0xF4, 0xF6));
        p.setColor(QPalette::WindowText, QColor(0x1F, 0x23, 0x28));
        p.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::AlternateBase, QColor(0xF3, 0xF4, 0xF6));
        p.setColor(QPalette::Text, QColor(0x1F, 0x23, 0x28));
        p.setColor(QPalette::Button, QColor(0xE6, 0xE8, 0xEB));
        p.setColor(QPalette::ButtonText, QColor(0x1F, 0x23, 0x28));
        p.setColor(QPalette::Highlight, QColor(0x0A, 0x6C, 0xFF));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::ToolTipBase, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::ToolTipText, QColor(0x1F, 0x23, 0x28));
        p.setColor(QPalette::PlaceholderText, QColor(0x9A, 0xA0, 0xA6));
        p.setColor(QPalette::Mid, QColor(0xD8, 0xDB, 0xE0));
        p.setColor(QPalette::Midlight, QColor(0xE5, 0xE7, 0xEB));
        p.setColor(QPalette::Light, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::Dark, QColor(0xC9, 0xCE, 0xD6));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x8B, 0x94, 0x9E));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x8B, 0x94, 0x9E));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x8B, 0x94, 0x9E));
    }
    QApplication::setPalette(p);
    qApp->setStyleSheet(makeStyleSheet(dark));
    if (plot_) plot_->setDarkTheme(dark);

    if (themeLight_) themeLight_->setChecked(themeMode_ == "light");
    if (themeDark_) themeDark_->setChecked(themeMode_ == "dark");
    if (themeSystem_) themeSystem_->setChecked(themeMode_ == "system");
    if (dashboard_) {
        dashboard_->refresh();
        dashboard_->update();
    }
    if (plot_) plot_->replot();
}

/* 退出时保存会话: 工具栏参数 + 变量对话框导入文件与勾选状态 */
void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue("toolbar/backend", backendBox_->currentData().toString());
    s.setValue("toolbar/device", ui->deviceBox->currentText());
    s.setValue("toolbar/rate", intervalSpin_->value());
    s.setValue("toolbar/tck", ui->tckBox->currentText());
    s.setValue("appearance/theme", themeMode_);
    s.setValue("session/initialized", true);
    s.setValue("window/dockState", saveState());
    varDialog_->saveState(s);
    saveSessionLeaves(s);
    dashboard_->saveState(s);
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
    monDock_ = ui->monDock;

    /* 后端显示名映射(实际后端名存 userData, 连接时取回) */
    const QStringList backends = LinkDriver::scanDrivers();
    if (backends.isEmpty()) {
        backendBox_->addItem("(未找到驱动)");
    } else {
        QHash<QString, QString> dispNames{
            {"gslink", "OpenOCD"},
            {"sci", "Uart"}};
        /* Simulation 仅用于内部测试: --demo / --stream 或 GS32SCOPE_ENABLE_SIM */
        const QStringList appArgs = QApplication::arguments();
        if (appArgs.contains("--demo") || appArgs.contains("--stream") ||
            qEnvironmentVariableIsSet("GS32SCOPE_ENABLE_SIM")) {
            dispNames.insert("sim", "Simulation");
        }

        for (const QString& b : backends) {
            if (!dispNames.contains(b)) continue;
            backendBox_->addItem(dispNames.value(b), b);
        }
        if (backendBox_->count() > 0) backendBox_->setCurrentIndex(0);

        /* 切换通讯接口: 立即刷新第二组参数(Uart -> 波特率)与目标(串口)列表 */
        connect(backendBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { applyBackendUi(backendBox_->currentData().toString()); });
        /* 展开"目标"下拉时再枚举一次串口, 插拔设备后无需重启 */
        connect(ui->deviceBox, &ComboBox::popupAboutToShow,
                this, &MainWindow::refreshDeviceList);
    }

    /* 四块停靠面板: 波形+仪表盘(上排等宽) / 监控+日志(下排等宽) */

    plot_ = new PlotWidget(this);
    dashboard_ = new DashboardWidget(this);
    dashboard_->setMonitorModel(model_);
    connect(dashboard_, &DashboardWidget::writeRequested,
            this, &MainWindow::onDashboardWrite);

    plotDock_ = new QDockWidget("波形", this);
    plotDock_->setObjectName("plotDock");
    plotDock_->setMinimumWidth(300);
    plotDock_->setWidget(plot_);
    plotDock_->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    plotDock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    plotDock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    plotDock_->setMinimumHeight(480);

    dashDock_ = new QDockWidget("组件", this);
    dashDock_->setObjectName("dashDock");
    dashDock_->setMinimumWidth(300);
    dashDock_->setWidget(dashboard_);
    dashDock_->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    dashDock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    dashDock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    dashDock_->setMinimumHeight(480);

    logView_ = new QPlainTextEdit(this);
    logView_->setObjectName("logView");
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(5000);
    logView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    {
        QFont mono("Consolas");
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSize(9);
        logView_->setFont(mono);
    }
    logDock_ = new QDockWidget("日志", this);
    logDock_->setObjectName("logDock");
    logDock_->setWidget(logView_);
    logDock_->setFeatures(QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable |
                          QDockWidget::DockWidgetClosable);
    logDock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    logDock_->setMinimumHeight(110);
    logDock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    monDock_->setFeatures(QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable |
                          QDockWidget::DockWidgetClosable);
    monDock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    monDock_->setMinimumWidth(540);
    monDock_->setMinimumHeight(110);
    monDock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    monDock_->setMonitorModel(model_);

    setDockOptions(QMainWindow::AnimatedDocks);
    addDockWidget(Qt::TopDockWidgetArea, plotDock_);
    addDockWidget(Qt::TopDockWidgetArea, dashDock_);
    splitDockWidget(plotDock_, dashDock_, Qt::Horizontal);
    addDockWidget(Qt::BottomDockWidgetArea, monDock_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    splitDockWidget(monDock_, logDock_, Qt::Horizontal);

    connect(actConn_, &QAction::triggered, this, &MainWindow::onConnectClicked);
    connect(actDisc_, &QAction::triggered, this, &MainWindow::onDisconnectClicked);
    connect(actStart_, &QAction::triggered, this, &MainWindow::onStartClicked);
    connect(actSingle_, &QAction::triggered, this, &MainWindow::onSingleClicked);
    connect(ui->actVar, &QAction::triggered, this, &MainWindow::onOpenVariableDialog);

    /* 波形数据操作: 导出 CSV / 清空; 放到菜单里, 避免工具栏拥挤 */
    actCsv_ = new QAction("导出 CSV", this);
    actClear_ = new QAction("清空波形", this);
    /* 快捷键保留功能但不显示在菜单里 */
    auto* shortcutCsv = new QShortcut(QKeySequence("Ctrl+Shift+E"), this);
    connect(shortcutCsv, &QShortcut::activated, this, &MainWindow::onExportCsv);
    auto* shortcutClear = new QShortcut(QKeySequence("Ctrl+Shift+L"), this);
    connect(shortcutClear, &QShortcut::activated, this, &MainWindow::onClearPlot);
    connect(actCsv_, &QAction::triggered, this, &MainWindow::onExportCsv);
    connect(actClear_, &QAction::triggered, this, &MainWindow::onClearPlot);

    auto* dataMenu = menuBar()->addMenu("数据(&D)");
    dataMenu->addAction(actCsv_);
    dataMenu->addSeparator();
    dataMenu->addAction("清空变量", this, [this] {
        plot_->clearData();
        history_.clear();
        timelineStarted_ = false;
        timelineRawBaseNs_ = timelineLogicalBaseNs_ = lastLogicalNs_ = 0;
        model_->clear();
        varDialog_->clearMonitoredChecks();
        setStatus("已清空监控变量");
    });
    dataMenu->addAction(actClear_);
    dataMenu->addAction("清空组件", this, [this] {
        dashboard_->clearAll();
        setStatus("已清空组件");
    });
    auto* viewMenu = menuBar()->addMenu("视图(&V)");
    auto addDockToggle = [viewMenu](QDockWidget* dock) {
        if (!dock) return;
        QAction* act = viewMenu->addAction(dock->windowTitle());
        act->setCheckable(true);
        act->setChecked(dock->isVisible());
        QObject::connect(act, &QAction::triggered, dock,
                         [dock](bool on) { dock->setVisible(on); });
        QObject::connect(dock, &QDockWidget::visibilityChanged, act,
                         [act](bool visible) {
            QSignalBlocker block(act);
            act->setChecked(visible);
        });
    };
    addDockToggle(plotDock_);
    addDockToggle(dashDock_);
    addDockToggle(monDock_);
    addDockToggle(logDock_);
    viewMenu->addSeparator();
    viewMenu->addAction("重置面板布局", this, &MainWindow::defaultDockLayout);

    /* 顶部参数区统一高度; 宽度按内容分配, 避免大框小字/小框挤字 */
    const int kCtlH = 30;
    for (QWidget* w : {static_cast<QWidget*>(backendBox_),
                       static_cast<QWidget*>(ui->deviceBox),
                       static_cast<QWidget*>(intervalSpin_),
                       static_cast<QWidget*>(ui->tckBox)}) {
        w->setFixedHeight(kCtlH);
    }
    /* 下拉框按内容调整首选宽度, 避免把工具栏按钮挤进扩展菜单 */
    for (QComboBox* cb : {static_cast<QComboBox*>(backendBox_),
                          static_cast<QComboBox*>(ui->deviceBox),
                          static_cast<QComboBox*>(ui->tckBox)}) {
        cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        cb->setMinimumContentsLength(7);
    }
    backendBox_->setMinimumWidth(110);
    ui->deviceBox->setMinimumWidth(110);
    intervalSpin_->setMinimumWidth(110);
    ui->tckBox->setMinimumWidth(110);
    /* 顶部控件填满整条工具栏, 文本完整可见 */
    for (QWidget* w : {static_cast<QWidget*>(backendBox_),
                       static_cast<QWidget*>(ui->deviceBox),
                       static_cast<QWidget*>(intervalSpin_),
                       static_cast<QWidget*>(ui->tckBox)}) {
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    for (QLabel* lb : {ui->labelBackend, ui->labelTarget,
                       ui->labelInterval, ui->labelTck}) {
        lb->setFixedHeight(kCtlH);
        lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    ui->labelInterval->setText("采样率(Hz):");
    ui->labelBackend->setText("通讯接口:");
    ui->labelTarget->setText("目标:");
    ui->labelTck->setText("TCK(MHz):");
    ui->mainToolBar->setMovable(false);

    auto* themeMenu = menuBar()->addMenu("主题(&T)");
    themeGroup_ = new QActionGroup(this);
    themeGroup_->setExclusive(true);
    themeLight_ = themeMenu->addAction("浅色");
    themeDark_ = themeMenu->addAction("深色");
    themeSystem_ = themeMenu->addAction("跟随系统");
    const struct { QAction* act; const char* mode; } themes[] = {
        {themeLight_, "light"}, {themeDark_, "dark"}, {themeSystem_, "system"},
    };
    for (const auto& t : themes) {
        t.act->setCheckable(true);
        t.act->setData(QString::fromLatin1(t.mode));
        themeGroup_->addAction(t.act);
    }
    connect(themeGroup_, &QActionGroup::triggered, this, [this](QAction* a) {
        const QString mode = a->data().toString();
        QSettings().setValue("appearance/theme", mode);
        applyTheme(mode);
        menuBar()->setActiveAction(nullptr);   /* 清掉菜单栏残留高亮 */
    });
}

/* 第二组参数随后端切换: Uart 用波特率, 其它后端用 TCK */
void MainWindow::applyBackendUi(const QString& backend) {
    const bool uart = (backend == "sci");
    ui->labelTck->setText(uart ? " 波特率: " : " TCK(MHz): ");
    const QString keep = ui->tckBox->currentText();
    QSignalBlocker block(ui->tckBox);
    ui->tckBox->clear();
    if (uart) {
        for (int b : {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
            ui->tckBox->addItem(QString::number(b));
        int idx = ui->tckBox->findText(keep);
        if (idx < 0) idx = ui->tckBox->findText(QStringLiteral("115200"));
        ui->tckBox->setCurrentIndex(idx >= 0 ? idx : 0);
    } else {
        for (int m : {1, 2, 4, 8, 16}) ui->tckBox->addItem(QString::number(m));
        int idx = ui->tckBox->findText(keep);
        if (idx < 0) idx = ui->tckBox->findText(QStringLiteral("4"));
        ui->tckBox->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    refreshDeviceList();
}

/* 目标下拉: Uart 时枚举可用串口并默认选第一个; 其它后端固定 RV32 */
void MainWindow::refreshDeviceList() {
    const QString backend = backendBox_->currentData().toString();
    const QString keep = ui->deviceBox->currentText().trimmed();

    if (backend != "sci") {
        if (ui->deviceBox->count() == 1 &&
            ui->deviceBox->itemText(0) == QStringLiteral("RV32")) return;
        QSignalBlocker block(ui->deviceBox);
        ui->deviceBox->clear();
        ui->deviceBox->addItem(QStringLiteral("RV32"));
        return;
    }

    const QStringList ports = availableSerialPorts();
    QSignalBlocker block(ui->deviceBox);
    ui->deviceBox->clear();
    if (ports.isEmpty()) {
        ui->deviceBox->addItem(QStringLiteral("(无串口)"));
        return;
    }
    ui->deviceBox->addItems(ports);
    const int idx = ports.indexOf(keep);
    ui->deviceBox->setCurrentIndex(idx >= 0 ? idx : 0);
}

void MainWindow::defaultDockLayout() {
    if (!monDock_ || !plotDock_ || !dashDock_ || !logDock_) return;
    for (QDockWidget* d : std::initializer_list<QDockWidget*>{
             plotDock_, dashDock_, static_cast<QDockWidget*>(monDock_), logDock_}) {
        d->setFloating(false);
        d->show();
        removeDockWidget(d);
    }
    addDockWidget(Qt::TopDockWidgetArea, plotDock_);
    addDockWidget(Qt::TopDockWidgetArea, dashDock_);
    splitDockWidget(plotDock_, dashDock_, Qt::Horizontal);
    addDockWidget(Qt::BottomDockWidgetArea, monDock_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    splitDockWidget(monDock_, logDock_, Qt::Horizontal);
    monDock_->show();
    plotDock_->show();
    dashDock_->show();
    logDock_->show();
    applyDockSizes();
    QTimer::singleShot(120, this, &MainWindow::applyDockSizes);
    QTimer::singleShot(400, this, &MainWindow::applyDockSizes);
}

void MainWindow::applyDockSizes() {
    if (!monDock_ || !plotDock_ || !dashDock_ || !logDock_) return;
    const int half = std::max(320, width() / 2);
    const int bottom = std::max(160, height() / 3);
    resizeDocks({plotDock_, dashDock_}, {half, half}, Qt::Horizontal);
    resizeDocks({monDock_, logDock_}, {half, half}, Qt::Horizontal);
    resizeDocks({monDock_, logDock_}, {bottom}, Qt::Vertical);
}

void MainWindow::appendLog(const QString& msg) {
    if (!logView_ || msg.isEmpty()) return;
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    logView_->appendPlainText(QStringLiteral("[%1] %2").arg(ts, msg));
}

void MainWindow::beginTimelineSegment() {
    /* 下一次 batch 重新取 raw 基准, 逻辑时间从 lastLogicalNs_ 继续;
     * 停采期间不累积时间, 恢复时波形不会跳过空白区间. */
    timelineStarted_ = false;
}

/* ---------------- 连接 ---------------- */
void MainWindow::onConnectClicked() {
    const QString backend = backendBox_->currentData().toString();
    if (backend.isEmpty()) return;
    if (!link_.load(backend)) { setStatus("加载驱动失败: " + link_.lastError(), true); return; }
    /* 驱动日志转发到日志面板(驱动线程 → UI 线程 queued), 替代弹控制台 */
    link_.setLogHandler([this](const QString& s) {
        const QString line = s.trimmed();
        if (line.isEmpty()) return;
        /* DMI 每次访问都打印会以 kHz 级别刷日志, 拖垮 GUI; 只保留 init/error 级别 */
        if (line.startsWith("[dmi")) return;
        QMetaObject::invokeMethod(this, [this, line]() { appendLog(line); },
                                  Qt::QueuedConnection);
    });
    gs32_config_t cfg{};
    const QByteArray bBackend = backend.toUtf8();
    QByteArray bExtra;
    if (backend == "sci" || backend == "can") {
        QString sciCfg = qEnvironmentVariable("GS32SCOPE_SCI_CONFIG");
        if (sciCfg.isEmpty()) {
            if (backend == "sci") {
                /* Uart: 串口号来自"目标"下拉, 波特率来自第二组参数 */
                const QString com = ui->deviceBox->currentText().trimmed();
                const int baud = ui->tckBox->currentText().toInt();
                if (com.isEmpty() || com.startsWith('(')) {
                    setStatus("未选择串口: 请连接设备后重新展开「目标」下拉", true);
                    return;
                }
                sciCfg = QString("%1@%2").arg(com).arg(baud > 0 ? baud : 115200);
            } else {
                sciCfg = QStringLiteral("tcp:127.0.0.1:7001");
            }
        }
        bExtra = sciConfigToJson(sciCfg);
    }
    /* 目标固定 RV32 → 驱动内部识别名 riscv32 */
    const QByteArray bDevice = QByteArrayLiteral("riscv32");
    const QByteArray bIf = backend == "gslink" ? QByteArray("jtag")
                         : (backend == "sci" ? QByteArray("serial")
                         : (backend == "can" ? QByteArray("can") : QByteArray("swd")));
    cfg.backend = bBackend.constData();
    cfg.device = bDevice.constData();
    cfg.interface = bIf.constData();
    cfg.extra = bExtra.isEmpty() ? nullptr : bExtra.constData();
    const auto* tck = findChild<QComboBox*>("tckBox");
    if (backend == "sci")   /* Uart: 第二组参数是波特率, 不是 kHz */
        cfg.speed_khz = tck ? uint32_t(tck->currentText().toInt()) : 115200;
    else
        cfg.speed_khz = tck ? uint32_t(tck->currentText().toInt() * 1000) : 4000;
    if (!link_.open(cfg)) { setStatus("连接失败: " + link_.lastErrorText(), true); link_.close(); return; }
    connectedBackend_ = backend;
    /* 新连接 = 新会话时间轴, 清空旧波形/历史 */
    plot_->clearData();
    history_.clear();
    model_->resetStats();
    timelineStarted_ = false;
    timelineRawBaseNs_ = timelineLogicalBaseNs_ = lastLogicalNs_ = 0;
    actConn_->setEnabled(false); actDisc_->setEnabled(true);
    setConnControlsEnabled(false);   /* 连接期间锁定通讯接口/目标/TCK, 采样率未采样时仍可调 */
    updateStartAction();   /* 连续/单次采样按钮同条件使能 */
    statusConn_->setText("已连接: " + backendBox_->currentText());
    if (backend == "sci") {
        const QString envCfg = qEnvironmentVariable("GS32SCOPE_SCI_CONFIG");
        const QString where = envCfg.isEmpty()
            ? ui->deviceBox->currentText() + " @ " + ui->tckBox->currentText()
            : envCfg;
        statusConn_->setText("已连接: Uart (" + where + ")");
    }
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
int MainWindow::rateHz() const {
    return std::clamp(intervalSpin_->value(), 1, 10000);
}

/* 波形刷新跟随采样率: 低频按采样周期刷新, 高频封顶约 60fps.
 * 1kHz -> 16ms; 50Hz -> 20ms; 10Hz -> 100ms; 1Hz -> 1s. */
void MainWindow::updatePlotRefreshRate() {
    const int hz = rateHz();
    const int ms = hz <= 60 ? int(std::lround(1000.0 / hz)) : 16;
    plotTimer_.setInterval(std::clamp(ms, 16, 1000));
}

/* 真实后端优先目标侧 STREAM: 采样率由目标定时器决定, 不受串口往返限制;
 * Simulation 在低频仍走 POLL, 保持既有演示行为。 */
bool MainWindow::startSampling(QString* err) {
    const int hz = rateHz();
    const int intervalMs = std::max(1, int(std::lround(1000.0 / hz)));
    engine_->configure(&link_, intervalMs, model_);
    const bool canStream = link_.hasCap(GS32_CAP_STREAM);
    if (hz > 1000 && !canStream) {
        if (err) *err = QStringLiteral("当前后端不支持 %1Hz；高采样率需要 STREAM 后端").arg(hz);
        return false;
    }
    const bool useStream = canStream && (hz > 1000 || connectedBackend_ != "sim");
    bool ok = false;
    if (useStream) {
        ok = engine_->startStream(uint32_t(std::llround(1e9 / double(hz))), err);
    } else {
        ok = engine_->start(err);
    }
    if (ok) beginTimelineSegment();
    return ok;
}

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
        /* 采样率等连接参数在断开连接前保持禁用(setConnControlsEnabled) */
        statusRate_->clear();
        if (!pendingDisconnect_) setStatus("正在停止采样…");
        return;
    }
    QString err;
    if (!startSampling(&err)) { setStatus("启动采样失败: " + err, true); return; }
    actStart_->setText("停止采样");
    actSingle_->setEnabled(false);
    intervalSpin_->setEnabled(false);
    setStatus(QString("采样中: %1 叶子 @ %2 Hz (%3)")
                  .arg(model_->leafCount())
                  .arg(rateHz())
                  .arg(engine_->isStreaming() ? "STREAM" : "POLL"));
}

/* 单次采样: 读取一拍即自动停止; 期间两按钮禁用, 单次按钮显示等待状态 */
void MainWindow::onSingleClicked() {
    if (stopping_ || engine_->running()) return;
    const int intervalMs = std::max(1, int(std::lround(1000.0 / rateHz())));
    engine_->configure(&link_, intervalMs, model_);
    QString err;
    if (!engine_->startSingle(&err)) { setStatus("启动采样失败: " + err, true); return; }
    beginTimelineSegment();
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
    intervalSpin_->setEnabled(!connectedBackend_.isEmpty());
    if (pendingRestart_) {
        pendingRestart_ = false;
        QString err;
        if (model_->leafCount() && !startSampling(&err)) {
            updateStartAction();
            setStatus("重启采样失败: " + err, true);
        } else if (engine_->running()) {
            actStart_->setText("停止采样");
            actSingle_->setEnabled(false);
            intervalSpin_->setEnabled(false);
            setStatus(QString("采样中: %1 叶子 @ %2 Hz (%3)")
                          .arg(model_->leafCount())
                          .arg(rateHz())
                          .arg(engine_->isStreaming() ? "STREAM" : "POLL"));
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

/* 连接参数区: 连接成功后锁接口/目标/TCK; 采样率在连接但未采样时仍可调 */
void MainWindow::setConnControlsEnabled(bool on) {
    backendBox_->setEnabled(on);
    ui->deviceBox->setEnabled(on);
    ui->tckBox->setEnabled(on);
    intervalSpin_->setEnabled(on || (!connectedBackend_.isEmpty() &&
                                     !engine_->running() && !stopping_));
}

void MainWindow::reconfigureEngine() {
    if (engine_->running()) {
        /* 采样中模型变更: 先非阻塞停止, stopped() 回调里再重启 */
        pendingRestart_ = true;
        engine_->requestStop();
        /* 连接期间采样率保持禁用, 重启参数沿用当前值 */
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

void MainWindow::onRemoveRoot(int rootIdx) {
    if (rootIdx < 0 || rootIdx >= model_->rootCount()) return;
    const QString rootPath = model_->root(rootIdx).name;
    model_->removeRoot(rootIdx);
    /* 监控表按根移除后, 导入变量表格同步取消勾选 */
    if (varDialog_) varDialog_->uncheckMonitored(rootPath);
}

/* ---------------- 模型 -> 界面 ---------------- */
void MainWindow::onModelChanged() {
    pruneHistory();
    monDock_->rebuild();
    syncPlotChannels();
    dashboard_->refresh();
    reconfigureEngine();
    updateStartAction();
}

void MainWindow::onLeafShownChanged(int) {
    syncPlotChannels();
}

/* 移除已不存在的叶子的历史, 防止长期运行内存增长 */
void MainWindow::pruneHistory() {
    if (elfReloading_) return;   /* ELF 重载过程中路径会短暂消失, 结束后再清 */
    for (auto it = history_.begin(); it != history_.end();) {
        if (model_->leafIndexByPath(it.key()) < 0) it = history_.erase(it);
        else ++it;
    }
}

/* ELF 重新导入时, 快照波形/HEX/颜色, 重新加载后按变量名恢复 */
void MainWindow::onElfReloadStarted() {
    elfUiState_.clear();
    for (int i = 0; i < model_->leafCount(); ++i) {
        const auto& lf = model_->leaf(i);
        ElfLeafState st;
        st.shown = lf.shown;
        st.hex = lf.hex;
        st.color = lf.color;
        st.addr = lf.def.addr;
        elfUiState_.insert(lf.path, st);
    }
    elfReloading_ = true;
}

void MainWindow::onElfReloadFinished() {
    elfReloading_ = false;
    for (auto it = elfUiState_.constBegin(); it != elfUiState_.constEnd(); ++it) {
        const int li = model_->leafIndexByPath(it.key());
        if (li < 0) continue;
        const ElfLeafState& st = it.value();
        const auto& lf = model_->leaf(li);
        if (st.addr != lf.def.addr) {
            /* 地址变了, 旧历史和旧曲线不再可信 */
            history_.remove(it.key());
            if (plot_->hasChannel(it.key())) plot_->removeChannel(it.key());
        }
        if (lf.shown != st.shown) model_->setLeafShown(li, st.shown);
        if (lf.hex != st.hex) model_->setLeafHex(li, st.hex);
        if (st.color.isValid() && lf.color != st.color)
            model_->setLeafColor(li, st.color);
    }
    elfUiState_.clear();
    pruneHistory();
    monDock_->rebuild();
    syncPlotChannels();
    reconfigureEngine();
    updateStartAction();
}

/* 会话叶子: 除 ELF/SVD 重导入外, 也持久化仿真/绘制专用等扁平变量 */
void MainWindow::saveSessionLeaves(QSettings& s) const {
    if (!model_) return;
    s.beginWriteArray("session/leaves", model_->leafCount());
    for (int i = 0; i < model_->leafCount(); ++i) {
        const auto& lf = model_->leaf(i);
        s.setArrayIndex(i);
        s.setValue("path", lf.path);
        s.setValue("addr", lf.def.addr);
        s.setValue("size", lf.def.size);
        s.setValue("type", int(lf.def.type));
        s.setValue("bitOffset", lf.def.bitOffset);
        s.setValue("bitWidth", lf.def.bitWidth);
        s.setValue("monitored", lf.monitored);
        s.setValue("shown", lf.shown);
        s.setValue("hex", lf.hex);
        s.setValue("color", lf.color.name(QColor::HexRgb));
    }
    s.endArray();
}

void MainWindow::restoreSessionLeaves(QSettings& s) {
    const int n = s.beginReadArray("session/leaves");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        VariableDef d;
        d.name = s.value("path").toString();
        if (d.name.isEmpty()) continue;
        d.addr = s.value("addr").toUInt();
        d.size = s.value("size").toUInt();
        d.type = VariableDef::Type(s.value("type", int(VariableDef::Float)).toInt());
        d.bitOffset = s.value("bitOffset", -1).toInt();
        d.bitWidth = s.value("bitWidth").toInt();
        const bool monitored = s.value("monitored", true).toBool();
        const bool shown = s.value("shown", false).toBool();
        bool hex = s.value("hex", false).toBool();
        const QColor savedColor(s.value("color").toString());
        int li = model_->leafIndexByPath(d.name);
        if (li >= 0) {
            if (model_->leaf(li).monitored != monitored)
                model_->setLeafMonitored(li, monitored);
            if (model_->leaf(li).shown != shown)
                model_->setLeafShown(li, shown);
        } else if (monitored) {
            model_->addLeafRoot(d);
            li = model_->leafIndexByPath(d.name);
            if (li >= 0 && shown) model_->setLeafShown(li, true);
        } else if (shown) {
            model_->addDrawOnlyRoot(d);   /* 默认 shown=true */
            li = model_->leafIndexByPath(d.name);
        }
        if (li >= 0 && model_->leaf(li).hex != hex)
            model_->setLeafHex(li, hex);
        if (li >= 0 && savedColor.isValid() &&
            model_->leaf(li).color != savedColor)
            model_->setLeafColor(li, savedColor);
    }
    s.endArray();
}

void MainWindow::syncPlotChannels() {
    QSet<QString> want;
    for (int li = 0; li < model_->leafCount(); ++li) {
        const auto& lf = model_->leaf(li);
        if (lf.shown) {
            want.insert(lf.path);
            if (!plot_->hasChannel(lf.path)) {
                plot_->addChannel(lf.path, lf.color);
                /* 加入显示集时回放已经采到的历史 */
                if (history_.contains(lf.path))
                    plot_->setChannelData(lf.path, history_.value(lf.path));
            } else {
                plot_->setChannelColor(lf.path, lf.color);
            }
        }
    }
    for (const QString& name : plot_->channelNames())
        if (!want.contains(name))
            plot_->removeChannel(name);
}

/* ---------------- 数据流 ---------------- */
void MainWindow::onBatchReady(const SampleBatch& batch) {
    if (batch.tNs.isEmpty()) return;

    /* 逻辑时间轴: 每次开始采样建立新 raw 基准, 从 lastLogicalNs_ 继续.
     * 停止期间 raw 时间仍在走, 但逻辑时间不累积, 恢复后不会跳空. */
    if (!timelineStarted_) {
        timelineRawBaseNs_ = batch.tNs.first();
        timelineLogicalBaseNs_ = lastLogicalNs_;
        timelineStarted_ = true;
    }
    const qint64 offsetNs = timelineLogicalBaseNs_ - timelineRawBaseNs_;
    const qint64 lastLogical = batch.tNs.last() + offsetNs;
    lastLogicalNs_ = lastLogical;

    constexpr double kHistorySec = 60.0;
    constexpr int kMaxHistoryPoints = 30000;   /* 每叶子约 30s@1kHz 或 60s@500Hz */
    const double tNow = double(lastLogical) / 1e9;

    for (int k = 0; k < batch.values.size() && k < model_->leafCount(); ++k) {
        const auto& vals = batch.values[k];
        /* 统计必须覆盖 batch 内全部样本(旧实现只取 last(), 会漏掉批内尖峰) */
        model_->updateLeafValues(k, vals);

        const auto& lf = model_->leaf(k);
        const int n = std::min(batch.tNs.size(), vals.size());
        QVector<QPointF> pts;
        pts.reserve(n);
        for (int i = 0; i < n; ++i) {
            const double t = double(batch.tNs[i] + offsetNs) / 1e9;
            pts.append(QPointF(t, vals[i]));
        }

        /* 所有叶子都保留历史, 这样后加入显示集的通道能立即回放 */
        QVector<QPointF>& hist = history_[lf.path];
        hist += pts;
        if (hist.size() > kMaxHistoryPoints)
            hist.remove(0, hist.size() - kMaxHistoryPoints);
        while (!hist.isEmpty() && hist.first().x() < tNow - kHistorySec)
            hist.removeFirst();

        if (lf.shown && plot_->hasChannel(lf.path))
            plot_->appendBatch(lf.path, pts);
    }
    plot_->trimBefore(tNow);
    /* 实际重绘由 plotTimer_ 完成, 其周期随采样率调整 */
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
    dashboard_->refresh();
}

void MainWindow::onExportCsv() {
    if (!plot_->hasData()) {
        setStatus("当前没有波形数据可导出", true);
        return;
    }
    /* GS32SCOPE_CSV 供自动化/无头测试直接指定输出文件; 正常使用走文件对话框 */
    QString path = qEnvironmentVariable("GS32SCOPE_CSV");
    if (path.isEmpty())
        path = QFileDialog::getSaveFileName(
            this, "导出波形 CSV", "gs32scope_wave.csv", "CSV 文件 (*.csv)");
    if (path.isEmpty()) return;
    QString err;
    if (plot_->exportCsv(path, &err))
        setStatus("已导出: " + path);
    else
        setStatus("导出失败: " + err, true);
}

void MainWindow::onClearPlot() {
    plot_->clearData();
    history_.clear();
    timelineStarted_ = false;
    timelineRawBaseNs_ = timelineLogicalBaseNs_ = lastLogicalNs_ = 0;
    setStatus("已清空波形与历史");
}

/* ---------------- 写入 ---------------- */
bool MainWindow::writeVariable(const QString& path, double value, bool confirm) {
    const int li = model_->leafIndexByPath(path);
    if (li < 0) { setStatus("写入失败: 未找到变量 " + path, true); return false; }
    if (connectedBackend_.isEmpty() || !link_.isLoaded()) {
        setStatus("写入失败: 尚未连接目标", true);
        return false;
    }
    const MonitorLeaf& lf = model_->leaf(li);
    if (confirm) {
        const QString msg = QString("确认写入？\n\n%1\n地址 0x%2\n新值 %3")
                                .arg(path)
                                .arg(lf.def.addr, 8, 16, QChar('0'))
                                .arg(QString::number(value, 'g', 9));
        if (QMessageBox::question(this, "确认写入", msg) != QMessageBox::Yes)
            return false;
    }
    const WriteResult r = VariableWriter::write(&link_, lf.def, value);
    if (!r.ok) {
        setStatus("写入失败: " + r.error, true);
        return false;
    }
    model_->setLeafCurrent(li, r.readback);
    setStatus(QString("写入成功: %1 = %2 (回读 %3)")
                  .arg(path, QString::number(value, 'g', 9),
                       QString::number(r.readback, 'g', 9)));
    return true;
}

void MainWindow::onWriteLeaf(int leafIdx) {
    if (leafIdx < 0 || leafIdx >= model_->leafCount()) return;
    if (connectedBackend_.isEmpty() || !link_.isLoaded()) {
        setStatus("写入失败: 尚未连接目标", true);
        return;
    }
    const MonitorLeaf& lf = model_->leaf(leafIdx);
    WriteDialog dlg(lf.def, lf.cur, this);
    if (dlg.exec() == QDialog::Accepted)
        writeVariable(lf.path, dlg.value(), false);
}

void MainWindow::onDashboardWrite(const QString& path, double value) {
    writeVariable(path, value, false);
}

/* ---------------- 仿真演示 ---------------- */
void MainWindow::loadSimDemoVariables() {
    /* 真实后端上 0x2010xxxx 是目标内存(ILM/代码区), 不能伪造变量更不能写入;
     * 仿真演示变量只在 Simulation 后端加载。 */
    if (connectedBackend_ != "sim") return;
    struct DemoVar {
        const char* name;
        uint32_t addr;
        VariableDef::Type type;
        bool plot;
    };
    static const DemoVar kDemo[] = {
        {"sim.speed",   0x20100000u, VariableDef::Float,  true},
        {"sim.current", 0x20100004u, VariableDef::Float,  true},
        {"sim.target",  0x20100008u, VariableDef::Float,  false},
        {"sim.enable",  0x2010000Cu, VariableDef::Uint32, false},
        {"sim.temp",    0x20100010u, VariableDef::Float,  true},
    };
    int added = 0;
    for (const auto& d : kDemo) {
        const QString name = QString::fromLatin1(d.name);
        if (model_->leafIndexByPath(name) >= 0) continue;
        VariableDef def;
        def.name = name;
        def.addr = d.addr;
        def.size = 4;
        def.type = d.type;
        model_->addLeafRoot(def);
        if (d.plot) {
            const int li = model_->leafIndexByPath(name);
            if (li >= 0) model_->setLeafShown(li, true);
        }
        ++added;
    }
    if (connectedBackend_ == "sim" || connectedBackend_ == "sci" ||
        connectedBackend_ == "can") {
        /* 给演示面板一个确定的初始状态: 目标 80, 使能关闭 */
        writeVariable("sim.target", 80.0, false);
        writeVariable("sim.enable", 0.0, false);
    }
    setStatus(QString("仿真演示变量已加载(%1 个新增)").arg(added));
}

bool MainWindow::ensureDemoReady() {
    /* 自动化/测试可用 GS32SCOPE_BACKEND 强制后端(sim/sci/can) */
    const QString forced = qEnvironmentVariable("GS32SCOPE_BACKEND");
    if (!forced.isEmpty()) {
        const int fi = backendBox_->findData(forced);
        if (fi >= 0) backendBox_->setCurrentIndex(fi);
    }
    if (connectedBackend_.isEmpty()) {
        const QString want = backendBox_->currentData().toString();
        if (want != "sim" && want != "sci" && want != "can") {
            const int idx = backendBox_->findData("sim");
            if (idx < 0) { setStatus("未找到 Simulation/SCI/CAN 驱动", true); return false; }
            backendBox_->setCurrentIndex(idx);
        }
        onConnectClicked();
        if (connectedBackend_.isEmpty()) return false;
    }
    if (connectedBackend_ != "sim" && connectedBackend_ != "sci" &&
        connectedBackend_ != "can") {
        setStatus("仿真演示需要 Simulation/SCI/CAN 后端, 请先断开当前连接", true);
        return false;
    }

    loadSimDemoVariables();
    if (dashDock_) { dashDock_->show(); dashDock_->raise(); }
    if (dashboard_->itemCount() == 0) dashboard_->createDemoPanel();
    return true;
}

void MainWindow::onSimDemo() {
    if (!ensureDemoReady()) return;
    if (!engine_->running() && !stopping_) onStartClicked();
}

void MainWindow::onStreamDemo() {
    if (!ensureDemoReady()) return;
    if (plotDock_) { plotDock_->show(); plotDock_->raise(); }  /* 10kHz 演示聚焦波形 */
    intervalSpin_->setValue(10000);                           /* 10kHz -> STREAM */
    if (!engine_->running() && !stopping_) onStartClicked();
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
    appendLog(msg);
}
