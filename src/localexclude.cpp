#include "localexclude.h"
#include "ui_localexclude.h"
#include "configmanager.h"
#include "routeutil.h"
#include "common.h"
#include <QJsonArray>

LocalExclude::LocalExclude(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::LocalExclude)
{
    ui->setupUi(this);
    setFixedSize(geometry().width(), geometry().height());
    loadFromConfig();
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &LocalExclude::onAccepted);
}

LocalExclude::~LocalExclude()
{
    delete ui;
}

void LocalExclude::loadFromConfig()
{
    QStringList lines;
    const QJsonArray arr = configManager->config["localSplitExclude"].toArray();
    for (const QJsonValue &value : arr) {
        const QString cidr = value.toString().trimmed();
        if (!cidr.isEmpty()) {
            lines << cidr;
        }
    }
    ui->plainTextEdit->setPlainText(lines.join('\n'));
}

bool LocalExclude::saveToConfig()
{
    QString parseError;
    const QStringList cidrs = RouteUtil::parseCidrList(ui->plainTextEdit->toPlainText(), &parseError);
    if (!parseError.isEmpty()) {
        error(parseError, this);
        return false;
    }

    QJsonArray arr;
    for (const QString &cidr : cidrs) {
        arr.append(cidr);
    }
    configManager->config["localSplitExclude"] = arr;
    configManager->saveConfig();
    return true;
}

void LocalExclude::onAccepted()
{
    if (saveToConfig()) {
        accept();
    }
}
