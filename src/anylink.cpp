#include "anylink.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonValue>
#include <QPointer>
#include <QStyleHints>
#include <QTextStream>
#include <QtConcurrent>
#include <QtWidgets>
#include "configmanager.h"
#include "detaildialog.h"
#include "jsonrpcwebsocketclient.h"
#include "localexclude.h"
#include "profilemanager.h"
#include "routeutil.h"
#include "textbrowser.h"
#include "ui_anylink.h"

#if defined(Q_OS_MACOS)
#include "macdockiconhandler.h"
#endif

AnyLink::AnyLink(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::AnyLink), m_vpnConnected(false)
{
    ui->setupUi(this);
#ifndef Q_OS_MACOS
    layout()->removeItem(ui->topSpacer);
    setWindowFlags(Qt::Dialog);
#else
    setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowMinimizeButtonHint
                   | Qt::WindowCloseButtonHint);
#endif
    setWindowTitle(tr("AnyLink Secure Client") + " v" + appVersion);

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    loadStyleSheet(":/resource/style.qss");
    setWindowIcon(QIcon(":/images/anylink64.png"));
#endif
    // qDebug() << screen()->devicePixelRatio() << geometry().width() << geometry().height() << QSysInfo::kernelType();
    // 需要联合使用 QSysInfo::kernelType() 和  QSysInfo::productType()
    const QString kernelType = QSysInfo::kernelType();
    if (screen()->devicePixelRatio() > 1.0 && (kernelType == "linux" || kernelType == "darwin")) {
        setFixedSize(geometry().width(), geometry().height());
        // setFixedSize(560, 390);
    } else {
        setFixedSize(490, 330);
    }

    center();
    ui->lineEditOTP->setFocus();
    connect(ui->lineEditOTP, &QLineEdit::returnPressed, this, [this]() {
        if (!ui->lineEditOTP->text().isEmpty()) {
            if (rpc->isConnected()) {
                connectVPN();
            }
        }
    });

    profileManager = new ProfileManager(this);

    if(profileManager->loadProfile(Json)) {
        profileManager->updateModel();
        ui->comboBoxHost->setModel(profileManager->model);
        // update by vpnConnected
        QString lastProfile = configManager->config["lastProfile"].toString();
        if(!lastProfile.isEmpty() && profileManager->model->stringList().contains(lastProfile)) {
            ui->comboBoxHost->setCurrentText(lastProfile);
        } else {
            ui->comboBoxHost->setCurrentIndex(0);
        }

        // 每个 profile 的密码被读取后都会发送 keyRestored
        connect(profileManager, &ProfileManager::keyRestored, this, [this](const QString &profile) {
            // keychain 中的密码是异步获取的
            QTimer::singleShot(500, this, [this, profile]() {
                if (configManager->config["autoLogin"].toBool()) {
                    const QString lastProfile = configManager->config["lastProfile"].toString();
                    if (lastProfile == profile) {
                        connectVPN();
                    }
                }
            });
        });
    }
    // exit
}

AnyLink::~AnyLink()
{
    RouteUtil::removeLocalExcludes();
    delete ui;
}

void AnyLink::closeEvent(QCloseEvent *event)
{
    if(m_vpnConnected) {
        hide();
        event->accept();
        if(!trayIcon->isVisible()) {
            trayIcon->show();
        }
    } else {
        qApp->quit();
    }
}

void AnyLink::showEvent(QShowEvent *event)
{
    if(trayIcon == nullptr) {
        QTimer::singleShot(50, this, [this]() { afterShowOneTime(); });
    }
    event->accept();
}

void AnyLink::center()
{
    QRect screenGeometry = screen()->geometry();
    QRect windowGeometry = frameGeometry();
    QPoint centerPoint = screenGeometry.center() - windowGeometry.center();
    if (screen()->devicePixelRatio() > 1.0) {
        centerPoint -= QPoint(0,120);
    }
    // 将窗口移动到居中位置
    move(centerPoint);
}

void AnyLink::loadStyleSheet(const QString &styleSheetFile)
{
    QFile file(styleSheetFile);
    if (!file.open(QFile::ReadOnly)) {
        return;
    }
    const QString styleSheet = QLatin1String(file.readAll());
    qApp->setStyleSheet(styleSheet);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
        style()->unpolish(this);
        style()->polish(this);
        update();
    }, Qt::UniqueConnection);
#endif
}

void AnyLink::createTrayActions()
{
    actionConnect = new QAction(tr("Connect Gateway"), this);
    // not lambda must have this
    connect(actionConnect, &QAction::triggered, this, &AnyLink::connectVPN);

    actionDisconnect = new QAction(tr("Disconnect Gateway"), this);
    connect(actionDisconnect, &QAction::triggered, this, &AnyLink::disconnectVPN);

    actionConfig = new QAction(tr("Show Panel"), this);
    connect(actionConfig, &QAction::triggered, this, [this]() {
        // 有最小化按钮并最小化时 show 不起作用
        showNormal();
    });

    actionQuit = new QAction(tr("Quit"), this);
    connect(actionQuit, &QAction::triggered, this, [this]() {
        // if not connected, the app will quit, see closeEvent
        close();
        if(m_vpnConnected) {
            qApp->quit();
        }
    });
}

void AnyLink::createTrayIcon()
{
    trayIconMenu = new QMenu(this);
    trayIconMenu->addAction(actionConnect);
    trayIconMenu->addAction(actionDisconnect);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(actionConfig);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(actionQuit);

    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setContextMenu(trayIconMenu);
    trayIcon->setIcon(iconNotConnected);

#if defined(Q_OS_MACOS)
    // Note: On macOS, the Dock icon is used to provide the tray's functionality.
    MacDockIconHandler* dockIconHandler = MacDockIconHandler::instance();
    connect(dockIconHandler, &MacDockIconHandler::dockIconClicked, this, [this]() { showNormal(); });
    trayIconMenu->setAsDockMenu();
#endif

#if defined(Q_OS_WIN)
    connect(trayIcon,
            &QSystemTrayIcon::activated,
            this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    showNormal();
                }
            });
#endif
}

void AnyLink::initConfig()
{
    ui->checkBoxAutoLogin->setChecked(configManager->config["autoLogin"].toBool());
    ui->checkBoxMinimize->setChecked(configManager->config["minimize"].toBool());
    ui->checkBoxBlock->setChecked(configManager->config["block"].toBool());
    ui->checkBoxDebug->setChecked(configManager->config["debug"].toBool());
    ui->checkBoxLang->setChecked(configManager->config["local"].toBool());
    ui->checkBoxCiscoCompat->setChecked(configManager->config["cisco_compat"].toBool());
    ui->checkBoxDtls->setChecked(configManager->config["no_dtls"].toBool());
    ui->checkBoxBypassChina->setChecked(configManager->config["bypassChina"].toBool());

    connect(ui->checkBoxAutoLogin, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["autoLogin"] = checked;
        saveConfig();
    });
    connect(ui->checkBoxMinimize, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["minimize"] = checked;
        saveConfig();
    });
    connect(ui->checkBoxBlock, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["block"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxDebug, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["debug"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxLang, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["local"] = checked;
        saveConfig();
    });
    connect(ui->checkBoxCiscoCompat, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["cisco_compat"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxDtls, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["no_dtls"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxBypassChina, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["bypassChina"] = checked;
        saveConfig();
    });
}

void AnyLink::afterShowOneTime()
{
    createTrayActions();
    createTrayIcon();
    initConfig();
    profileManager->afterShowOneTime();
    detailDialog = new DetailDialog(this);

    // 每隔 60 秒获取 DTLS 状态，因为是 afterShowOneTime，不能关闭定时器
    connect(&timer, &QTimer::timeout, this, [this]() {
        if (!configManager->config["no_dtls"].toBool()) {
            rpc->callAsync("status", STATUS, [this](const QJsonValue &result) {
                const QJsonObject &status = result.toObject();
                if (!status.contains("code")) {
                    ui->labelChannelType->setText(status["DtlsConnected"].toBool() ? "DTLS" : "TLS");
                    ui->labelDtlsCipherSuite->setText(status["DTLSCipherSuite"].toString());
                }
            });
        }
    });

    connect(this, &AnyLink::vpnConnected, this, [this]() {
        getVPNStatus();
        m_vpnConnected = true;
        activeDisconnect = false;
        trayIcon->setIcon(iconConnected);
        ui->buttonConnect->setText(tr("Disconnect"));
        ui->comboBoxHost->setEnabled(false);
        ui->lineEditOTP->setEnabled(false);
        ui->buttonProfile->setEnabled(false);
        ui->tabSetting->setEnabled(false);

        actionConnect->setEnabled(false);
        actionDisconnect->setEnabled(true);
        if(ui->checkBoxMinimize->isChecked()) {
            close();
            trayIcon->setToolTip(tr("Connected to: ") + currentProfile.value("host").toString());
        }
        if (configManager->config["lastProfile"].toString() != ui->comboBoxHost->currentText()) {
            configManager->config["lastProfile"] = ui->comboBoxHost->currentText();
            saveConfig();
        }

        timer.start(60 * 1000);
    });

    connect(this, &AnyLink::vpnClosed, [this]() {
        m_vpnConnected = false;
        trayIcon->setIcon(iconNotConnected);
        trayIcon->setToolTip("");

        ui->buttonConnect->setText(tr("Connect"));
        ui->comboBoxHost->setEnabled(true);
        ui->lineEditOTP->setEnabled(true);
        ui->buttonProfile->setEnabled(true);
        ui->tabSetting->setEnabled(true);

        actionConnect->setEnabled(true);
        actionDisconnect->setEnabled(false);
        resetVPNStatus();

        timer.stop();
    });

    connect(qApp, &QApplication::aboutToQuit, this, [this]() {
        if(m_vpnConnected) {
            disconnectVPN();
        }
    });

    rpc = new JsonRpcWebSocketClient(this);
    connect(rpc, &JsonRpcWebSocketClient::error, this, [this](const QString &error) {
        Q_UNUSED(error)
        ui->statusBar->setText(tr("Failed to connect to vpnagent, please reinstall the software!"));
        ui->buttonConnect->setEnabled(false);
        emit vpnClosed();
        if(isHidden()) {
            show();
        }
    });
    connect(rpc, &JsonRpcWebSocketClient::connected, this, [this]() {
        configVPN();
    });
    // may be exited normally by other clients, do not automatically reconnect
    rpc->registerCallback(DISCONNECT, [this](const QJsonValue & result) {
        ui->progressBar->stop();
        ui->statusBar->setText(result.toString());
        emit vpnClosed();
        if(!activeDisconnect && isHidden()) {
            show();
        }
    });
    // unusual exited
    rpc->registerCallback(ABORT, [this](const QJsonValue & result) {
        ui->statusBar->setText(result.toString());
        emit vpnClosed();
        if (!activeDisconnect) {
            // 快速重连，不需要再次进行用户认证
            QTimer::singleShot(1500, this, [this]() { connectVPN(true); });
        }
    });
    rpc->connectToServer(QUrl("ws://127.0.0.1:6210/rpc"));
}

void AnyLink::resetVPNStatus()
{
    removeLocalSplitExclude();

    ui->labelChannelType->clear();
    ui->labelTlsCipherSuite->clear();
    ui->labelDtlsCipherSuite->clear();
    ui->labelDTLSPort->clear();
    ui->labelServerAddress->clear();
    ui->labelLocalAddress->clear();
    ui->labelVPNAddress->clear();
    ui->labelMTU->clear();
    ui->labelDNS->clear();

    ui->buttonDetails->setEnabled(false);
    detailDialog->clear();

    ui->lineEditOTP->clear();
}

void AnyLink::saveConfig()
{
    configManager->saveConfig();
}

static QString formatRouteList(const QJsonArray &routes)
{
    QStringList lines;
    for (const QJsonValue &value : routes) {
        const QString cidr = value.toString();
        const QPair<QHostAddress, int> parsed = QHostAddress::parseSubnet(cidr);
        if (parsed.second >= 0) {
            lines << QString("%1/%2").arg(parsed.first.toString()).arg(parsed.second);
        } else if (!cidr.isEmpty()) {
            lines << cidr;
        }
    }
    return lines.isEmpty() ? QStringLiteral("(empty)") : lines.join(", ");
}

void AnyLink::logRouteInfo(const QJsonObject &status)
{
    const QString includes = formatRouteList(status["SplitInclude"].toArray());
    const QString excludes = formatRouteList(status["SplitExclude"].toArray());
    const QString localExcludes = formatRouteList(configManager->config["localSplitExclude"].toArray());
    const QString dns = status["DNS"].toVariant().toStringList().join(",");
    const QString vpnAddress = status["VPNAddress"].toString();
    const QString serverAddress = status["ServerAddress"].toString();
    const QString localAddress = status["LocalAddress"].toString();

    qInfo().noquote() << QString("VPN routes: server=%1 local=%2 vpn=%3 dns=%4")
                             .arg(serverAddress, localAddress, vpnAddress, dns);
    const bool bypassChina = configManager->config["bypassChina"].toBool();
    const int chinaCount = bypassChina ? RouteUtil::chinaRoutes().size() : 0;
    const QString chinaStatus = bypassChina
        ? QString("enabled, prefixes=%1").arg(chinaCount)
        : QStringLiteral("disabled");

    qInfo().noquote() << "SplitInclude (secured):" << includes;
    qInfo().noquote() << "SplitExclude (excluded):" << excludes;
    qInfo().noquote() << "Local SplitExclude:" << localExcludes;
    qInfo().noquote() << "China bypass:" << chinaStatus;

    QFile logFile(tempLocation + "/vpnagent.log");
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Failed to append route info to vpnagent.log";
        return;
    }

    QTextStream out(&logFile);
    out.setEncoding(QStringConverter::Utf8);
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    out << ts << " [INFO] VPN address: " << vpnAddress
        << ", local: " << localAddress
        << ", server: " << serverAddress
        << ", DNS: " << dns << "\n";
    out << ts << " [INFO] SplitInclude (secured): " << includes << "\n";
    out << ts << " [INFO] SplitExclude (excluded): " << excludes << "\n";
    out << ts << " [INFO] Local SplitExclude: " << localExcludes << "\n";
    out << ts << " [INFO] China bypass: " << chinaStatus << "\n";
}

void AnyLink::appendAgentLog(const QString &message)
{
    qInfo().noquote() << message;
    QFile logFile(tempLocation + "/vpnagent.log");
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream out(&logFile);
    out.setEncoding(QStringConverter::Utf8);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
        << " [INFO] " << message << "\n";
}

void AnyLink::applyLocalSplitExclude()
{
    QStringList cidrs;
    const QJsonArray arr = configManager->config["localSplitExclude"].toArray();
    for (const QJsonValue &value : arr) {
        const QString cidr = value.toString().trimmed();
        if (!cidr.isEmpty()) {
            cidrs << cidr;
        }
    }
    const bool bypassChina = configManager->config["bypassChina"].toBool();
    const QString vpnAddress = ui->labelVPNAddress->text();
    if (bypassChina) {
        appendAgentLog(QString("China bypass: enabled, prefixes=%1").arg(RouteUtil::chinaRoutes().size()));
    } else {
        appendAgentLog(QStringLiteral("China bypass: disabled"));
    }

    QPointer<AnyLink> self(this);
    (void)QtConcurrent::run([self, cidrs, bypassChina, vpnAddress]() {
        const RouteApplyResult result = RouteUtil::applyLocalExcludes(cidrs, bypassChina, vpnAddress);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) {
                return;
            }
            for (const QString &line : result.logs) {
                self->appendAgentLog(line);
            }
            if (result.bypassChina) {
                self->appendAgentLog(QString("China bypass: added %1 prefixes").arg(result.chinaAdded));
            }
            if (!result.gatewayError.isEmpty()) {
                const QString msg = self->tr("Failed to apply local SplitExclude: %1").arg(result.gatewayError);
                self->appendAgentLog(msg);
                error(msg, self);
                return;
            }
            QStringList failures;
            if (!result.customFailures.isEmpty()) {
                failures << self->tr("Failed to apply local SplitExclude (try running as administrator):\n%1")
                                .arg(result.customFailures.join('\n'));
            }
            if (!result.chinaError.isEmpty()) {
                failures << self->tr("Failed to apply China bypass (try running as administrator):\n%1")
                                .arg(result.chinaError);
            }
            if (!failures.isEmpty()) {
                const QString msg = failures.join(QLatin1String("\n\n"));
                self->appendAgentLog(msg);
                error(msg, self);
            }
        }, Qt::QueuedConnection);
    });
}

void AnyLink::removeLocalSplitExclude()
{
    QPointer<AnyLink> self(this);
    (void)QtConcurrent::run([self]() {
        const RouteApplyResult result = RouteUtil::removeLocalExcludes();
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) {
                return;
            }
            for (const QString &line : result.logs) {
                self->appendAgentLog(line);
            }
            if (result.chinaRemoved > 0) {
                self->appendAgentLog(QString("China bypass: removed %1 prefixes").arg(result.chinaRemoved));
            }
        }, Qt::QueuedConnection);
    });
}

/**
 * called by JsonRpcWebSocketClient::connected and every time setting changed
 */
void AnyLink::configVPN()
{
    if(rpc->isConnected()) {
        QJsonObject args{{"log_level", ui->checkBoxDebug->isChecked() ? "Debug" : "Info"},
                         {"log_path", tempLocation},
                         {"skip_verify", !ui->checkBoxBlock->isChecked()},
                         {"cisco_compat", ui->checkBoxCiscoCompat->isChecked()},
                         {"no_dtls", ui->checkBoxDtls->isChecked()},
                         {"agent_name", agentName},
                         {"agent_version", appVersion}};
        rpc->callAsync("config", CONFIG, args, [this](const QJsonValue & result) {
            ui->statusBar->setText(result.toString());
        });
    }
}

void AnyLink::connectVPN(bool reconnect)
{
    if(rpc->isConnected()) {
        // profile may be modified, and may not emit currentTextChanged signal
        // must not affected by QComboBox::currentTextChanged

        QString method = "connect";
        int id = CONNECT;
        if(reconnect) {
            method = "reconnect";
            id = RECONNECT;
        } else {
            const QString name = ui->comboBoxHost->currentText();
            if (name.isEmpty()) {
                return;
            }
            QJsonObject profile = profileManager->profiles[name].toObject();
            currentProfile = profile;
            const QString otp = ui->lineEditOTP->text();
            if(!otp.isEmpty()) {
                currentProfile["password"] = profile["password"].toString() + otp;
            }
        }
        ui->progressBar->start();
        trayIcon->setIcon(iconConnecting);

        rpc->callAsync(method, id, currentProfile, [this, reconnect](const QJsonValue &result) {
            ui->progressBar->stop();
            if(result.isObject()) {  // error object
                // dialog
                //                ui->statusBar->setText(result.toObject().value("message").toString());
                if (reconnect) {
                    // 当快速重连失败，再次尝试完全重新连接，用于服务端可能已经移除session的情况
                    QTimer::singleShot(3000, this, [this]() { connectVPN(); });
                } else {
                    if (isHidden()) {
                        show();
                    }
                    error(result.toObject().value("message").toString(), this);
                }
            } else {
                ui->statusBar->setText(result.toString());
                emit vpnConnected();
            }
        });
    }
}

void AnyLink::disconnectVPN()
{
    if(rpc->isConnected()) {
        ui->progressBar->start();
        // because on_buttonConnect_clicked, must check m_vpnConnected outside
        rpc->callAsync("disconnect", DISCONNECT);
        activeDisconnect = true;
    }
}

void AnyLink::getVPNStatus()
{
    rpc->callAsync("status", STATUS, [this](const QJsonValue & result) {
        const QJsonObject &status = result.toObject();
        // qDebug() << status;
        if(!status.contains("code")) {
            ui->labelChannelType->setText(status["DtlsConnected"].toBool() ? "DTLS" : "TLS");
            ui->labelTlsCipherSuite->setText(status["TLSCipherSuite"].toString());
            ui->labelDtlsCipherSuite->setText(status["DTLSCipherSuite"].toString());
            ui->labelDTLSPort->setText(status["DTLSPort"].toString());
            ui->labelServerAddress->setText(status["ServerAddress"].toString());
            ui->labelLocalAddress->setText(status["LocalAddress"].toString());
            ui->labelVPNAddress->setText(status["VPNAddress"].toString());
            ui->labelMTU->setText(QString::number(status["MTU"].toInt()));
            ui->labelDNS->setText(status["DNS"].toVariant().toStringList().join(","));

            if (!ui->buttonDetails->isEnabled()) {
                ui->buttonDetails->setEnabled(true);
                QJsonArray excludes = status["SplitExclude"].toArray();
                const QJsonArray localExcludes = configManager->config["localSplitExclude"].toArray();
                for (const QJsonValue &value : localExcludes) {
                    if (!excludes.contains(value)) {
                        excludes.append(value);
                    }
                }
                detailDialog->setRoutes(excludes, status["SplitInclude"].toArray());
                logRouteInfo(status);
                applyLocalSplitExclude();
            }
        }
    });
}

void AnyLink::on_buttonConnect_clicked()
{
    if(rpc->isConnected()) {
        if(m_vpnConnected) {
            disconnectVPN();
        } else {
            connectVPN();
        }
    }
}

void AnyLink::on_buttonProfile_clicked()
{
    profileManager->exec();
}

void AnyLink::on_buttonViewLog_clicked()
{
    QString filePath = tempLocation + "/vpnagent.log";
    QFile loadFile(filePath);
    if(!loadFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error(tr("Couldn't open log file"), this);
        return;
    }
    TextBrowser textBrowser(tr("Log Viewer"),this);


    QString data = loadFile.readAll();
    textBrowser.setText(data);
    loadFile.close();

    // 创建文件系统监视器
    QFileSystemWatcher watcher;
    watcher.addPath(filePath);

    // 监视文件变化的信号槽连接
    QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, [&]() {
        QFile updatedFile(filePath);
        if (updatedFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // 重新读取文件内容
            data = updatedFile.readAll();
            textBrowser.setText(data);
            updatedFile.close();
        }
    });

    textBrowser.exec();
}

void AnyLink::on_buttonDetails_clicked()
{
    detailDialog->exec();
}

void AnyLink::on_buttonSecurityTips_clicked()
{
    QString readme = "README.md";
    if (QLocale::system().name() == "zh_CN") {
        readme = "README_zh_CN.md";
    }
    QFile loadFile(":/resource/" + readme);
    if(!loadFile.open(QIODevice::ReadOnly)) {
        error(tr("Couldn't open README.md"), this);
        return;
    }
    QByteArray data = loadFile.readAll();
    TextBrowser textBrowser(tr("Security Tips"),this);
    textBrowser.setMarkdown(data);
    textBrowser.exec();
}

void AnyLink::on_buttonLocalSplitExclude_clicked()
{
    LocalExclude dialog(this);
    dialog.exec();
}
