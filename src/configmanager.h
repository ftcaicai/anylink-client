#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include "common.h"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>

class ConfigManager : public QObject
{
    Q_OBJECT
public:
    explicit ConfigManager(QObject *parent = nullptr);

    QJsonObject config{{"lastProfile", ""},
                       {"autoLogin", false},
                       {"minimize", true},
                       {"block", true},
                       {"debug", false},
                       {"local", true},
                       {"no_dtls", false},
                       {"cisco_compat", false},
                       {"bypassChina", false},
                       {"localSplitExclude", QJsonArray()}};
    bool loadConfig(SaveFormat saveFormat);
    void saveConfig(SaveFormat saveFormat);
    void saveConfig();
};

#endif // CONFIGMANAGER_H
