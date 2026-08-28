#ifndef ROUTEUTIL_H
#define ROUTEUTIL_H

#include <QString>
#include <QStringList>

struct RouteApplyResult
{
    QString gateway;
    QStringList logs;
    QString gatewayError;
    QStringList customFailures;
    QString chinaError;
    int customAdded = 0;
    int chinaAdded = 0;
    int chinaTotal = 0;
    int chinaRemoved = 0;
    bool bypassChina = false;
};

class RouteUtil
{
public:
    static QStringList parseCidrList(const QString &text, QString *error);
    static QString normalizeCidr(const QString &line, QString *error = nullptr);
    static QStringList chinaRoutes();

    static QString defaultGateway(QString *error, const QString &skipInterfaceIp = QString());
    static bool addExcludeRoute(const QString &cidr, const QString &gateway, QString *error);
    static bool delExcludeRoute(const QString &cidr, const QString &gateway, QString *error);

    static RouteApplyResult applyLocalExcludes(const QStringList &customCidrs, bool bypassChina,
                                               const QString &skipInterfaceIp);
    static RouteApplyResult removeLocalExcludes();

private:
    static QString prefixToNetmask(int prefix);
    static bool splitCidr(const QString &cidr, QString *destination, QString *netmask, int *prefix,
                          QString *error);
    static QString runCommand(const QString &program, const QStringList &args, int *exitCode,
                              int timeoutMs = 8000);
    static QString runCommandWithInput(const QString &program, const QStringList &args,
                                       const QByteArray &input, int *exitCode, int timeoutMs);
    static int addExcludeRoutesBulk(const QStringList &cidrs, const QString &gateway, QString *error);
    static int delExcludeRoutesBulk(const QStringList &cidrs, const QString &gateway, QString *error);
    static RouteApplyResult clearAppliedRoutes();
};

#endif // ROUTEUTIL_H
