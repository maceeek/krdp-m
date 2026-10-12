// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <csignal>
#include <filesystem>

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QRegularExpression>
#include <QTimer>

#include <KAboutData>
#include <KCrash>
#include <KSharedConfig>
#include <KSignalHandler>

#include <qt6keychain/keychain.h>

#include "Server.h"
#include "SessionController.h"
#include "krdp_version.h"
#include "krdpserversettings.h"

using namespace Qt::StringLiterals;

static const QString DBusService = u"org.kde.krdpserver"_s;
static const QString DBusDisplaysPath = u"/org/kde/krdpserver/Displays"_s;

static std::optional<VirtualDisplayManager::Mode> parseVirtualDisplayMode(QStringView value)
{
    if (value.compare(u"disabled", Qt::CaseInsensitive) == 0) {
        return VirtualDisplayManager::Mode::Disabled;
    }
    if (value.compare(u"extend", Qt::CaseInsensitive) == 0) {
        return VirtualDisplayManager::Mode::Extend;
    }
    if (value.compare(u"exclusive", Qt::CaseInsensitive) == 0) {
        return VirtualDisplayManager::Mode::Exclusive;
    }
    return std::nullopt;
}

/**
 * Restore the physical outputs saved by a krdpserver that did not exit cleanly.
 */
static int restoreDisplays(QApplication &application)
{
    if (!VirtualDisplayManager::hasSavedLayout()) {
        return 0;
    }

    VirtualDisplayManager manager;
    int result = 1;
    QObject::connect(&manager, &VirtualDisplayManager::restoreFinished, &application, [&result](bool success) {
        result = success ? 0 : 1;
        QCoreApplication::quit();
    });
    QTimer::singleShot(10000, &application, &QCoreApplication::quit);
    manager.restoreSavedLayout();
    application.exec();
    return result;
}

/**
 * Ask a running krdpserver to give the physical outputs back, or restore
 * them ourselves if it does not answer.
 */
static int returnDisplays(QApplication &application)
{
    auto message = QDBusMessage::createMethodCall(DBusService, DBusDisplaysPath, u"org.kde.krdpserver.Displays"_s, u"ReturnLocalDisplays"_s);
    const auto reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 2000);
    if (reply.type() == QDBusMessage::ReplyMessage) {
        return 0;
    }

    qWarning() << "krdpserver did not respond, restoring the displays directly:" << reply.errorMessage();
    return restoreDisplays(application);
}

int main(int argc, char **argv)
{
    QApplication application{argc, argv};
    application.setApplicationName(u"krdp-server"_s);
    application.setApplicationDisplayName(u"KRDP Server"_s);

    KAboutData about(u"krdp-server"_s, u"KRDP Server"_s, QStringLiteral(KRdp_VERSION_STRING));
    about.setDesktopFileName(u"org.kde.krdpserver"_s);
    KAboutData::setApplicationData(about);

    KCrash::initialize();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"An RDP server that exposes the current desktop session over the RDP protocol.\nNote that a valid TLS certificate and key is needed. If not provided, a temporary certificate will be generated."_s);
    parser.addOptions({
        {{u"u"_s, u"username"_s}, u"The username to use for login"_s, u"username"_s},
        {{u"p"_s, u"password"_s}, u"The password to use for login. Requires username to be passed as well."_s, u"password"_s},
        {u"address"_s, u"The address to listen on for connections. Defaults to 0.0.0.0"_s, u"address"_s},
        {u"port"_s, u"The port to use for connections. Defaults to 3389."_s, u"port"_s, u"3389"_s},
        {u"certificate"_s, u"The TLS certificate file to use."_s, u"certificate"_s, u"server.crt"_s},
        {u"certificate-key"_s, u"The TLS certificate key to use."_s, u"certificate-key"_s, u"server.key"_s},
        {u"monitor"_s, u"The index of the monitor to use when streaming."_s, u"monitor"_s, u"-1"_s},
        {u"virtual-monitor"_s,
         u"Creates a new virtual output to connect to (WIDTHxHEIGHT@SCALE, e.g. 1920x1080@1). Incompatible with --monitor."_s,
         u"data"_s,
         u"1920x1080@1"_s},
        {u"quality"_s, u"Encoding quality of the stream, from 0 (lowest) to 100 (highest)"_s, u"quality"_s},
        {u"virtual-display"_s,
         u"How remote sessions use the displays: disabled (stream the physical monitors), extend (add a virtual monitor matching the client) or exclusive (only use a virtual monitor matching the client, physical monitors are turned off while connected)."_s,
         u"mode"_s},
        {u"return-displays"_s, u"Turn the physical monitors back on while a remote session uses them exclusively, then exit."_s},
        {u"restore-displays"_s, u"Restore the physical monitors if a previous instance did not exit cleanly, then exit."_s},
#ifdef WITH_PLASMA_SESSION
        {u"plasma"_s, u"Use Plasma protocols instead of XDP"_s},
#endif
    });
    about.setupCommandLine(&parser);
    parser.process(application);
    about.processCommandLine(&parser);

    if (parser.isSet(u"return-displays"_s)) {
        return returnDisplays(application);
    }
    if (parser.isSet(u"restore-displays"_s)) {
        return restoreDisplays(application);
    }

    auto config = ServerConfig::self();

    auto parserValueWithDefault = [&parser](QAnyStringView option, auto defaultValue) {
        auto optionString = option.toString();
        if (parser.isSet(optionString)) {
            return QVariant(parser.value(optionString)).value<decltype(defaultValue)>();
        } else {
            return defaultValue;
        }
    };

    QHostAddress address = QHostAddress::Any;
    if (parser.isSet(u"address"_s)) {
        address = QHostAddress(parser.value(u"address"_s));
    }
    auto port = parserValueWithDefault(u"port", config->listenPort());
    auto certificate = std::filesystem::path(parserValueWithDefault(u"certificate", config->certificate()).toStdString());
    auto certificateKey = std::filesystem::path(parserValueWithDefault(u"certificate-key", config->certificateKey()).toStdString());

    KRdp::Server server(nullptr);

    server.setAddress(address);
    server.setPort(port);

    server.setTlsCertificate(certificate);
    server.setTlsCertificateKey(certificateKey);

    // Use parsed username/pw if set
    if (parser.isSet(u"username"_s)) {
        KRdp::User user;
        user.name = parser.value(u"username"_s);
        user.password = parser.value(u"password"_s);
        server.addUser(user);
    }
    // Otherwise use KCM username list
    else {
        server.setUsePAMAuthentication(config->systemUserEnabled());

        const auto users = config->users();
        for (const auto &userName : users) {
            const auto readJob = new QKeychain::ReadPasswordJob(QLatin1StringView("KRDP"));
            readJob->setKey(QLatin1StringView(userName.toLatin1()));
            QObject::connect(readJob, &QKeychain::ReadPasswordJob::finished, &server, [userName, readJob, &server]() {
                KRdp::User user;
                if (readJob->error() != QKeychain::Error::NoError) {
                    qWarning() << "requestPassword: Failed to read password of " << userName << " because of error: " << readJob->error();
                    return;
                }
                user.name = userName;
                user.password = readJob->textData();
                server.addUser(user);
            });
            readJob->start();
        }
        if (users.isEmpty() && !server.usePAMAuthentication()) {
            qWarning() << "No users configured for login. Either pass a username/password or configure users using kcm_krdp.";
            return -1;
        }
    }

    SessionController controller(&server, parser.isSet(u"plasma"_s) ? SessionController::SessionType::Plasma : SessionController::SessionType::Portal);
    if (parser.isSet(u"virtual-monitor"_s)) {
        const QString vmData = parser.value(u"virtual-monitor"_s);
        const QRegularExpression rx(uR"((\d+)x(\d+)@([\d.]+))"_s);
        const auto match = rx.match(vmData);
        if (!match.hasMatch()) {
            qWarning() << "failed to parse" << vmData << ".  Should be WIDTHxHEIGHT@SCALE";
            return 1;
        }
        controller.setVirtualMonitor({vmData, {match.capturedView(1).toInt(), match.capturedView(2).toInt()}, match.capturedView(3).toDouble()});
    } else {
        controller.setMonitorIndex(parser.isSet(u"monitor"_s) ? std::optional(parser.value(u"monitor"_s).toInt()) : std::nullopt);
    }
    controller.setQuality(parserValueWithDefault(u"quality", config->quality()));

    auto virtualDisplayMode = static_cast<VirtualDisplayManager::Mode>(config->virtualDisplayMode());
    if (parser.isSet(u"virtual-display"_s)) {
        const auto mode = parseVirtualDisplayMode(parser.value(u"virtual-display"_s));
        if (!mode) {
            qWarning() << "failed to parse" << parser.value(u"virtual-display"_s) << ". Expected disabled, extend or exclusive";
            return 1;
        }
        virtualDisplayMode = *mode;
    } else if (parser.isSet(u"virtual-monitor"_s) && virtualDisplayMode == VirtualDisplayManager::Mode::Disabled) {
        virtualDisplayMode = VirtualDisplayManager::Mode::Extend;
    }
    controller.setVirtualDisplayMode(virtualDisplayMode);
    controller.setVirtualDisplayMaximumSize(QSize(config->maximumWidth(), config->maximumHeight()));
    controller.setLocalTakeoverPolicy(config->detectLocalInput(), config->lockOnLocalTakeover(), config->disconnectOnLocalTakeover());

    // Restore the physical monitors before exiting, if we turned them off.
    KSignalHandler::self()->watchSignal(SIGINT);
    KSignalHandler::self()->watchSignal(SIGTERM);
    QObject::connect(KSignalHandler::self(), &KSignalHandler::signalReceived, &controller, [&controller](int) {
        controller.shutdown([]() {
            QCoreApplication::exit(0);
        });
    });

    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(DBusDisplaysPath, &controller, QDBusConnection::ExportScriptableSlots) || !bus.registerService(DBusService)) {
        qWarning() << "Could not register on D-Bus, --return-displays will restore the displays directly";
    }

    if (!server.start()) {
        return -1;
    }

    return application.exec();
}
