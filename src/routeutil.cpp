#include "routeutil.h"
#include <QAbstractSocket>
#include <QFile>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif

namespace {
QMutex g_appliedMutex;
QStringList g_appliedCustom;
QString g_appliedGateway;
bool g_appliedChina = false;

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
bool isTunnelDevice(const QString &name)
{
    const QString n = name.toLower();
    return n.startsWith(QLatin1String("tun")) || n.startsWith(QLatin1String("utun"))
        || n.startsWith(QLatin1String("tap")) || n.startsWith(QLatin1String("ppp"))
        || n.startsWith(QLatin1String("wg")) || n.startsWith(QLatin1String("any"))
        || n.contains(QLatin1String("vpn"));
}
#endif

bool isUsableGateway(const QString &gateway)
{
    if (gateway.isEmpty() || gateway.compare(QLatin1String("on-link"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    const QHostAddress addr(gateway);
    return !addr.isNull() && addr.protocol() == QAbstractSocket::IPv4Protocol
        && addr != QHostAddress(QHostAddress::AnyIPv4);
}

#ifdef Q_OS_WIN
QString winErrorText(DWORD err)
{
    wchar_t buf[256] = {};
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                   err, 0, buf, 256, nullptr);
    const QString msg = n ? QString::fromWCharArray(buf).trimmed() : QStringLiteral("unknown");
    return QString("error %1 (%2)").arg(err).arg(msg);
}

bool winResolveInterface(const QString &gateway, NET_LUID *luid, NET_IFINDEX *ifIndex, QString *error)
{
    const QHostAddress addr(gateway);
    if (addr.isNull() || addr.protocol() != QAbstractSocket::IPv4Protocol) {
        if (error) {
            *error = QStringLiteral("invalid gateway");
        }
        return false;
    }
    const IPAddr dest = htonl(addr.toIPv4Address());
    DWORD idx = 0;
    const DWORD err = GetBestInterface(dest, &idx);
    if (err != NO_ERROR) {
        if (error) {
            *error = QString("GetBestInterface failed: %1").arg(winErrorText(err));
        }
        return false;
    }
    NET_LUID resolvedLuid = {};
    const DWORD luidErr = ConvertInterfaceIndexToLuid(idx, &resolvedLuid);
    if (luidErr != NO_ERROR) {
        if (error) {
            *error = QString("ConvertInterfaceIndexToLuid failed: %1").arg(winErrorText(luidErr));
        }
        return false;
    }
    if (luid) {
        *luid = resolvedLuid;
    }
    if (ifIndex) {
        *ifIndex = idx;
    }
    return true;
}

void winFillRow(MIB_IPFORWARD_ROW2 *row, const QString &destination, int prefix, const QString &gateway,
                NET_LUID luid, NET_IFINDEX ifIndex)
{
    InitializeIpForwardEntry(row);
    row->InterfaceLuid = luid;
    row->InterfaceIndex = ifIndex;
    row->DestinationPrefix.Prefix.si_family = AF_INET;
    row->DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    row->DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr =
        htonl(QHostAddress(destination).toIPv4Address());
    row->DestinationPrefix.PrefixLength = static_cast<UINT8>(prefix);
    row->NextHop.si_family = AF_INET;
    row->NextHop.Ipv4.sin_family = AF_INET;
    row->NextHop.Ipv4.sin_addr.S_un.S_addr = htonl(QHostAddress(gateway).toIPv4Address());
    row->Protocol = static_cast<NL_ROUTE_PROTOCOL>(MIB_IPPROTO_NETMGMT);
    row->Origin = NlroManual;
    row->ValidLifetime = 0xffffffff;
    row->PreferredLifetime = 0xffffffff;
    row->Metric = 5;
}

int winBulkAdd(const QStringList &cidrs, const QString &gateway, QString *error)
{
    NET_LUID luid = {};
    NET_IFINDEX ifIndex = 0;
    if (!winResolveInterface(gateway, &luid, &ifIndex, error)) {
        return 0;
    }
    int added = 0;
    QString firstError;
    for (const QString &cidr : cidrs) {
        const QPair<QHostAddress, int> parsed = QHostAddress::parseSubnet(cidr);
        if (parsed.first.isNull() || parsed.second < 0
            || parsed.first.protocol() != QAbstractSocket::IPv4Protocol) {
            if (firstError.isEmpty()) {
                firstError = QString("invalid IPv4 CIDR: %1").arg(cidr);
            }
            continue;
        }
        MIB_IPFORWARD_ROW2 row;
        winFillRow(&row, parsed.first.toString(), parsed.second, gateway, luid, ifIndex);
        const DWORD err = CreateIpForwardEntry2(&row);
        if (err == NO_ERROR || err == ERROR_OBJECT_ALREADY_EXISTS) {
            ++added;
        } else if (firstError.isEmpty()) {
            firstError = QString("route add failed for %1: %2").arg(cidr, winErrorText(err));
        }
    }
    if (added == 0 && !firstError.isEmpty() && error) {
        *error = firstError;
    } else if (added < cidrs.size() && !firstError.isEmpty() && error) {
        *error = QString("%1; added %2/%3").arg(firstError).arg(added).arg(cidrs.size());
    }
    return added;
}

int winBulkDel(const QStringList &cidrs, const QString &gateway, QString *error)
{
    NET_LUID luid = {};
    NET_IFINDEX ifIndex = 0;
    if (!winResolveInterface(gateway, &luid, &ifIndex, error)) {
        return 0;
    }
    int removed = 0;
    QString firstError;
    for (const QString &cidr : cidrs) {
        const QPair<QHostAddress, int> parsed = QHostAddress::parseSubnet(cidr);
        if (parsed.first.isNull() || parsed.second < 0
            || parsed.first.protocol() != QAbstractSocket::IPv4Protocol) {
            continue;
        }
        MIB_IPFORWARD_ROW2 row;
        winFillRow(&row, parsed.first.toString(), parsed.second, gateway, luid, ifIndex);
        const DWORD err = DeleteIpForwardEntry2(&row);
        if (err == NO_ERROR || err == ERROR_NOT_FOUND || err == ERROR_FILE_NOT_FOUND) {
            if (err == NO_ERROR) {
                ++removed;
            }
        } else if (firstError.isEmpty()) {
            firstError = QString("route delete failed for %1: %2").arg(cidr, winErrorText(err));
        }
    }
    if (!firstError.isEmpty() && error) {
        *error = firstError;
    }
    return removed;
}
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
QString unixCidrArg(const QString &destination, int prefix)
{
    return QString("%1/%2").arg(destination).arg(prefix);
}
#endif
} // namespace

static void hideConsole(QProcess &proc)
{
#ifdef Q_OS_WIN
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow = SW_HIDE;
    });
#else
    Q_UNUSED(proc)
#endif
}

QString RouteUtil::runCommand(const QString &program, const QStringList &args, int *exitCode, int timeoutMs)
{
    return runCommandWithInput(program, args, QByteArray(), exitCode, timeoutMs);
}

QString RouteUtil::runCommandWithInput(const QString &program, const QStringList &args,
                                       const QByteArray &input, int *exitCode, int timeoutMs)
{
    QProcess proc;
    hideConsole(proc);
    proc.start(program, args);
    if (!proc.waitForStarted(3000)) {
        if (exitCode) {
            *exitCode = -1;
        }
        return QString();
    }
    if (!input.isEmpty()) {
        proc.write(input);
        proc.closeWriteChannel();
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        if (exitCode) {
            *exitCode = -1;
        }
        return QString();
    }
    if (exitCode) {
        *exitCode = proc.exitCode();
    }
    return QString::fromLocal8Bit(proc.readAllStandardOutput() + proc.readAllStandardError());
}

QString RouteUtil::prefixToNetmask(int prefix)
{
    if (prefix <= 0) {
        return QStringLiteral("0.0.0.0");
    }
    if (prefix >= 32) {
        return QStringLiteral("255.255.255.255");
    }
    const quint32 mask = ~quint32(0) << (32 - prefix);
    return QHostAddress(mask).toString();
}

bool RouteUtil::splitCidr(const QString &cidr, QString *destination, QString *netmask, int *prefix,
                          QString *error)
{
    const QString normalized = normalizeCidr(cidr, error);
    if (normalized.isEmpty()) {
        return false;
    }
    const QPair<QHostAddress, int> parsed = QHostAddress::parseSubnet(normalized);
    if (parsed.first.isNull() || parsed.second < 0 || parsed.first.protocol() != QAbstractSocket::IPv4Protocol) {
        if (error) {
            *error = QString("invalid IPv4 CIDR: %1").arg(cidr);
        }
        return false;
    }
    if (destination) {
        *destination = parsed.first.toString();
    }
    if (prefix) {
        *prefix = parsed.second;
    }
    if (netmask) {
        *netmask = prefixToNetmask(parsed.second);
    }
    return true;
}

QString RouteUtil::normalizeCidr(const QString &line, QString *error)
{
    QString text = line.trimmed();
    if (text.isEmpty() || text.startsWith('#')) {
        return QString();
    }
    if (!text.contains('/')) {
        text += QLatin1String("/32");
    }
    const QPair<QHostAddress, int> parsed = QHostAddress::parseSubnet(text);
    if (parsed.first.isNull() || parsed.second < 0) {
        if (error) {
            *error = QString("invalid CIDR: %1").arg(line.trimmed());
        }
        return QString();
    }
    if (parsed.first.protocol() != QAbstractSocket::IPv4Protocol) {
        if (error) {
            *error = QString("IPv6 is not supported: %1").arg(line.trimmed());
        }
        return QString();
    }
    if (parsed.first == QHostAddress(QHostAddress::AnyIPv4) && parsed.second == 0) {
        if (error) {
            *error = QString("0.0.0.0/0 is not allowed");
        }
        return QString();
    }
    return QString("%1/%2").arg(parsed.first.toString()).arg(parsed.second);
}

QStringList RouteUtil::parseCidrList(const QString &text, QString *error)
{
    QStringList result;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")));
    for (int i = 0; i < lines.size(); ++i) {
        const QString raw = lines.at(i).trimmed();
        if (raw.isEmpty() || raw.startsWith('#')) {
            continue;
        }
        QString lineError;
        const QString cidr = normalizeCidr(raw, &lineError);
        if (cidr.isEmpty()) {
            if (error) {
                *error = QString("line %1: %2").arg(i + 1).arg(lineError);
            }
            return QStringList();
        }
        if (!result.contains(cidr)) {
            result << cidr;
        }
    }
    return result;
}

QStringList RouteUtil::chinaRoutes()
{
    static QMutex cacheMutex;
    static QStringList cached;
    static bool loaded = false;
    QMutexLocker locker(&cacheMutex);
    if (loaded) {
        return cached;
    }
    loaded = true;
    QFile file(QStringLiteral(":/resource/chnroutes.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return cached;
    }
    while (!file.atEnd()) {
        const QString cidr = normalizeCidr(QString::fromUtf8(file.readLine()));
        if (!cidr.isEmpty()) {
            cached << cidr;
        }
    }
    return cached;
}

QString RouteUtil::defaultGateway(QString *error, const QString &skipInterfaceIp)
{
    QString fallback;
#if defined(Q_OS_WIN)
    int exitCode = 0;
    const QString output = runCommand(QStringLiteral("route"), {QStringLiteral("print"), QStringLiteral("-4")},
                                      &exitCode);
    const QStringList lines = output.split('\n');
    for (const QString &raw : lines) {
        const QStringList parts = raw.trimmed().simplified().split(' ');
        if (parts.size() < 3 || parts.at(0) != QLatin1String("0.0.0.0")
            || parts.at(1) != QLatin1String("0.0.0.0")) {
            continue;
        }
        const QString gateway = parts.at(2);
        if (!isUsableGateway(gateway)) {
            continue;
        }
        const QString iface = parts.size() >= 4 ? parts.at(3) : QString();
        if (!skipInterfaceIp.isEmpty() && iface == skipInterfaceIp) {
            if (fallback.isEmpty()) {
                fallback = gateway;
            }
            continue;
        }
        return gateway;
    }
#elif defined(Q_OS_LINUX)
    int exitCode = 0;
    const QString output =
        runCommand(QStringLiteral("ip"),
                   {QStringLiteral("-4"), QStringLiteral("route"), QStringLiteral("show"), QStringLiteral("default")},
                   &exitCode);
    const QStringList lines = output.split('\n');
    for (const QString &raw : lines) {
        const QStringList parts = raw.trimmed().simplified().split(' ');
        const int via = parts.indexOf(QStringLiteral("via"));
        if (via < 0 || via + 1 >= parts.size()) {
            continue;
        }
        const QString gateway = parts.at(via + 1);
        if (!isUsableGateway(gateway)) {
            continue;
        }
        const int dev = parts.indexOf(QStringLiteral("dev"));
        const QString device = (dev >= 0 && dev + 1 < parts.size()) ? parts.at(dev + 1) : QString();
        const int src = parts.indexOf(QStringLiteral("src"));
        const QString srcIp = (src >= 0 && src + 1 < parts.size()) ? parts.at(src + 1) : QString();
        if (isTunnelDevice(device) || (!skipInterfaceIp.isEmpty() && srcIp == skipInterfaceIp)) {
            if (fallback.isEmpty()) {
                fallback = gateway;
            }
            continue;
        }
        return gateway;
    }
#elif defined(Q_OS_MACOS)
    int exitCode = 0;
    const QString output =
        runCommand(QStringLiteral("netstat"),
                   {QStringLiteral("-rn"), QStringLiteral("-f"), QStringLiteral("inet")}, &exitCode);
    const QStringList lines = output.split('\n');
    for (const QString &raw : lines) {
        const QStringList parts = raw.trimmed().simplified().split(' ');
        if (parts.size() < 4 || parts.at(0) != QLatin1String("default")) {
            continue;
        }
        const QString gateway = parts.at(1);
        if (!isUsableGateway(gateway)) {
            continue;
        }
        const QString netif = parts.last();
        if (isTunnelDevice(netif) || (!skipInterfaceIp.isEmpty() && gateway == skipInterfaceIp)) {
            if (fallback.isEmpty()) {
                fallback = gateway;
            }
            continue;
        }
        return gateway;
    }
    if (fallback.isEmpty()) {
        const QString legacy =
            runCommand(QStringLiteral("route"),
                       {QStringLiteral("-n"), QStringLiteral("get"), QStringLiteral("default")}, &exitCode);
        const QStringList legacyLines = legacy.split('\n');
        for (const QString &raw : legacyLines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("gateway:"))) {
                const QString gateway = line.mid(QStringLiteral("gateway:").size()).trimmed();
                if (isUsableGateway(gateway)) {
                    return gateway;
                }
            }
        }
    }
#else
    Q_UNUSED(skipInterfaceIp)
    if (error) {
        *error = QStringLiteral("unsupported platform");
    }
    return QString();
#endif
    if (!fallback.isEmpty()) {
        return fallback;
    }
    if (error) {
        *error = QStringLiteral("failed to detect default gateway");
    }
    return QString();
}

bool RouteUtil::addExcludeRoute(const QString &cidr, const QString &gateway, QString *error)
{
    QString destination;
    QString netmask;
    int prefix = 0;
    if (!splitCidr(cidr, &destination, &netmask, &prefix, error)) {
        return false;
    }

    int exitCode = 0;
    QString output;
#if defined(Q_OS_WIN)
    output = runCommand(QStringLiteral("route"),
                        {QStringLiteral("add"), destination, QStringLiteral("mask"), netmask, gateway,
                         QStringLiteral("metric"), QStringLiteral("5")},
                        &exitCode);
#elif defined(Q_OS_LINUX)
    output = runCommand(QStringLiteral("ip"),
                        {QStringLiteral("route"), QStringLiteral("add"),
                         QString("%1/%2").arg(destination).arg(prefix), QStringLiteral("via"), gateway,
                         QStringLiteral("metric"), QStringLiteral("5")},
                        &exitCode);
#elif defined(Q_OS_MACOS)
    QStringList args{QStringLiteral("-n"), QStringLiteral("add")};
    if (prefix == 32) {
        args << QStringLiteral("-host") << destination << gateway;
    } else {
        args << QStringLiteral("-net") << QString("%1/%2").arg(destination).arg(prefix) << gateway;
    }
    output = runCommand(QStringLiteral("route"), args, &exitCode);
#else
    Q_UNUSED(gateway)
    if (error) {
        *error = QStringLiteral("unsupported platform");
    }
    return false;
#endif
    if (exitCode != 0) {
        const QString detail = output.simplified();
        if (detail.contains(QLatin1String("exist"), Qt::CaseInsensitive)) {
            return true;
        }
        if (error) {
            *error = detail.isEmpty() ? QString("route add failed for %1").arg(cidr)
                                      : QString("route add failed for %1: %2").arg(cidr, detail);
        }
        return false;
    }
    return true;
}

bool RouteUtil::delExcludeRoute(const QString &cidr, const QString &gateway, QString *error)
{
    QString destination;
    QString netmask;
    int prefix = 0;
    if (!splitCidr(cidr, &destination, &netmask, &prefix, error)) {
        return false;
    }

    int exitCode = 0;
    QString output;
#if defined(Q_OS_WIN)
    output = runCommand(QStringLiteral("route"),
                        {QStringLiteral("delete"), destination, QStringLiteral("mask"), netmask, gateway},
                        &exitCode);
#elif defined(Q_OS_LINUX)
    output = runCommand(QStringLiteral("ip"),
                        {QStringLiteral("route"), QStringLiteral("del"),
                         QString("%1/%2").arg(destination).arg(prefix), QStringLiteral("via"), gateway},
                        &exitCode);
#elif defined(Q_OS_MACOS)
    QStringList args{QStringLiteral("-n"), QStringLiteral("delete")};
    if (prefix == 32) {
        args << QStringLiteral("-host") << destination << gateway;
    } else {
        args << QStringLiteral("-net") << QString("%1/%2").arg(destination).arg(prefix) << gateway;
    }
    output = runCommand(QStringLiteral("route"), args, &exitCode);
#else
    Q_UNUSED(gateway)
    Q_UNUSED(output)
    if (error) {
        *error = QStringLiteral("unsupported platform");
    }
    return false;
#endif
    if (exitCode != 0) {
        if (error) {
            const QString detail = output.simplified();
            *error = detail.isEmpty() ? QString("route delete failed for %1").arg(cidr)
                                      : QString("route delete failed for %1: %2").arg(cidr, detail);
        }
        return false;
    }
    return true;
}

int RouteUtil::addExcludeRoutesBulk(const QStringList &cidrs, const QString &gateway, QString *error)
{
    if (cidrs.isEmpty()) {
        return 0;
    }
#if defined(Q_OS_WIN)
    return winBulkAdd(cidrs, gateway, error);
#elif defined(Q_OS_LINUX)
    QByteArray batch;
    for (const QString &cidr : cidrs) {
        QString destination;
        QString netmask;
        int prefix = 0;
        if (!splitCidr(cidr, &destination, &netmask, &prefix, nullptr)) {
            continue;
        }
        batch += QString("route replace %1 via %2 metric 5\n")
                     .arg(unixCidrArg(destination, prefix), gateway)
                     .toUtf8();
    }
    int exitCode = 0;
    const QString output =
        runCommandWithInput(QStringLiteral("ip"), {QStringLiteral("-force"), QStringLiteral("-batch"), QStringLiteral("-")},
                            batch, &exitCode, 120000);
    if (exitCode != 0) {
        const QString detail = output.simplified();
        const bool denied = detail.contains(QLatin1String("Permission denied"), Qt::CaseInsensitive)
            || detail.contains(QLatin1String("Operation not permitted"), Qt::CaseInsensitive);
        if (denied || (!detail.contains(QLatin1String("File exists"), Qt::CaseInsensitive)
                       && !detail.contains(QLatin1String("No such process"), Qt::CaseInsensitive))) {
            if (error) {
                *error = detail.isEmpty() ? QStringLiteral("ip batch add failed") : detail;
            }
            return 0;
        }
    }
    return cidrs.size();
#elif defined(Q_OS_MACOS)
    QByteArray script;
    for (const QString &cidr : cidrs) {
        QString destination;
        QString netmask;
        int prefix = 0;
        if (!splitCidr(cidr, &destination, &netmask, &prefix, nullptr)) {
            continue;
        }
        if (prefix == 32) {
            script += QString("route -n add -host %1 %2 >/dev/null 2>&1\n")
                          .arg(destination, gateway)
                          .toUtf8();
        } else {
            script += QString("route -n add -net %1 %2 >/dev/null 2>&1\n")
                          .arg(unixCidrArg(destination, prefix), gateway)
                          .toUtf8();
        }
    }
    int exitCode = 0;
    runCommandWithInput(QStringLiteral("/bin/sh"), {QStringLiteral("-s")}, script, &exitCode, 180000);
    Q_UNUSED(exitCode)
    return cidrs.size();
#else
    Q_UNUSED(gateway)
    if (error) {
        *error = QStringLiteral("unsupported platform");
    }
    return 0;
#endif
}

int RouteUtil::delExcludeRoutesBulk(const QStringList &cidrs, const QString &gateway, QString *error)
{
    if (cidrs.isEmpty()) {
        return 0;
    }
#if defined(Q_OS_WIN)
    return winBulkDel(cidrs, gateway, error);
#elif defined(Q_OS_LINUX)
    QByteArray batch;
    for (const QString &cidr : cidrs) {
        QString destination;
        QString netmask;
        int prefix = 0;
        if (!splitCidr(cidr, &destination, &netmask, &prefix, nullptr)) {
            continue;
        }
        batch += QString("route del %1 via %2\n").arg(unixCidrArg(destination, prefix), gateway).toUtf8();
    }
    int exitCode = 0;
    const QString output =
        runCommandWithInput(QStringLiteral("ip"), {QStringLiteral("-force"), QStringLiteral("-batch"), QStringLiteral("-")},
                            batch, &exitCode, 120000);
    if (exitCode != 0) {
        const QString detail = output.simplified();
        const bool denied = detail.contains(QLatin1String("Permission denied"), Qt::CaseInsensitive)
            || detail.contains(QLatin1String("Operation not permitted"), Qt::CaseInsensitive);
        if (denied) {
            if (error) {
                *error = detail;
            }
            return 0;
        }
    }
    return cidrs.size();
#elif defined(Q_OS_MACOS)
    QByteArray script;
    for (const QString &cidr : cidrs) {
        QString destination;
        QString netmask;
        int prefix = 0;
        if (!splitCidr(cidr, &destination, &netmask, &prefix, nullptr)) {
            continue;
        }
        if (prefix == 32) {
            script += QString("route -n delete -host %1 %2 >/dev/null 2>&1\n")
                          .arg(destination, gateway)
                          .toUtf8();
        } else {
            script += QString("route -n delete -net %1 %2 >/dev/null 2>&1\n")
                          .arg(unixCidrArg(destination, prefix), gateway)
                          .toUtf8();
        }
    }
    int exitCode = 0;
    runCommandWithInput(QStringLiteral("/bin/sh"), {QStringLiteral("-s")}, script, &exitCode, 180000);
    Q_UNUSED(exitCode)
    return cidrs.size();
#else
    Q_UNUSED(gateway)
    if (error) {
        *error = QStringLiteral("unsupported platform");
    }
    return 0;
#endif
}

RouteApplyResult RouteUtil::applyLocalExcludes(const QStringList &customCidrs, bool bypassChina,
                                               const QString &skipInterfaceIp)
{
    QMutexLocker locker(&g_appliedMutex);
    RouteApplyResult result;
    result.bypassChina = bypassChina;

    clearAppliedRoutes();

    if (customCidrs.isEmpty() && !bypassChina) {
        return result;
    }

    QString gwError;
    const QString gateway = defaultGateway(&gwError, skipInterfaceIp);
    if (gateway.isEmpty()) {
        result.gatewayError = gwError;
        return result;
    }
    result.gateway = gateway;
    g_appliedGateway = gateway;

    for (const QString &cidr : customCidrs) {
        QString routeError;
        if (addExcludeRoute(cidr, gateway, &routeError)) {
            g_appliedCustom << cidr;
            ++result.customAdded;
            result.logs << QString("Local SplitExclude added: %1 via %2").arg(cidr, gateway);
        } else {
            result.customFailures << routeError;
        }
    }

    if (bypassChina) {
        const QStringList china = chinaRoutes();
        result.chinaTotal = china.size();
        if (china.isEmpty()) {
            result.chinaError = QStringLiteral("China IP list is empty");
        } else {
            QString chinaError;
            const int added = addExcludeRoutesBulk(china, gateway, &chinaError);
            result.chinaAdded = added;
            if (added > 0) {
                g_appliedChina = true;
            }
            if (!chinaError.isEmpty() || added == 0) {
                result.chinaError = chinaError.isEmpty()
                    ? QStringLiteral("failed to add China bypass routes")
                    : chinaError;
            }
        }
    }
    return result;
}

RouteApplyResult RouteUtil::clearAppliedRoutes()
{
    RouteApplyResult result;
    result.gateway = g_appliedGateway;
    if (g_appliedCustom.isEmpty() && !g_appliedChina) {
        return result;
    }

    const QString gateway = g_appliedGateway;
    if (g_appliedChina) {
        QString chinaError;
        result.chinaRemoved = delExcludeRoutesBulk(chinaRoutes(), gateway, &chinaError);
        result.chinaError = chinaError;
        g_appliedChina = false;
    }
    for (const QString &cidr : g_appliedCustom) {
        QString routeError;
        if (!delExcludeRoute(cidr, gateway, &routeError)) {
            result.logs << QString("Local SplitExclude remove failed: %1").arg(routeError);
        } else {
            result.logs << QString("Local SplitExclude removed: %1").arg(cidr);
        }
    }
    result.customAdded = g_appliedCustom.size();
    g_appliedCustom.clear();
    g_appliedGateway.clear();
    return result;
}

RouteApplyResult RouteUtil::removeLocalExcludes()
{
    QMutexLocker locker(&g_appliedMutex);
    return clearAppliedRoutes();
}
