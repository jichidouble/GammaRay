/*
  main.cpp

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mcpserver.h"

#include <client/clientconnectionmanager.h>
#include <common/paths.h>
#include <config-gammaray.h>

#include <QApplication>
#include <QByteArray>
#include <QThread>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <unistd.h>
#endif

class StandardInputReader : public QThread
{
    Q_OBJECT
public:
    using QThread::QThread;

signals:
    void lineReceived(const QString &line);
    void inputClosed();

protected:
    void run() override
    {
        QByteArray pending;
        char buffer[4096];
#ifdef Q_OS_WIN
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        const bool inputIsPipe = GetFileType(input) == FILE_TYPE_PIPE;
#endif
        for (;;) {
#ifdef Q_OS_WIN
            DWORD available = static_cast<DWORD>(sizeof(buffer));
            if (inputIsPipe) {
                if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
                    break;
                if (available == 0) {
                    QThread::msleep(5);
                    continue;
                }
                available = qMin(available, static_cast<DWORD>(sizeof(buffer)));
            }
            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(input, buffer, available, &bytesRead, nullptr);
            if (!ok || bytesRead == 0)
                break;
            pending.append(buffer, static_cast<qsizetype>(bytesRead));
#else
            const ssize_t bytesRead = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (bytesRead <= 0)
                break;
            pending.append(buffer, static_cast<qsizetype>(bytesRead));
#endif
            qsizetype newline = -1;
            while ((newline = pending.indexOf('\n')) >= 0) {
                QByteArray line = pending.left(newline);
                pending.remove(0, newline + 1);
                if (line.endsWith('\r'))
                    line.chop(1);
                emit lineReceived(QString::fromUtf8(line));
            }
        }
        if (!pending.isEmpty())
            emit lineReceived(QString::fromUtf8(pending));
        emit inputClosed();
    }
};

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QCoreApplication::setOrganizationName(QStringLiteral("KDAB"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("kdab.com"));
    QCoreApplication::setApplicationName(QStringLiteral("GammaRay MCP"));

    QApplication app(argc, argv);
    GammaRay::Paths::setRelativeRootPath(GAMMARAY_INVERSE_BIN_DIR);
    GammaRay::ClientConnectionManager::init();

    McpServer server;
    if (!server.openStandardOutput())
        return 1;

    StandardInputReader reader;
    QObject::connect(&reader, &StandardInputReader::lineReceived,
                     &server, &McpServer::handleLine, Qt::QueuedConnection);
    QObject::connect(&reader, &StandardInputReader::inputClosed,
                     &app, &QCoreApplication::quit, Qt::QueuedConnection);
    reader.start();

    const int result = app.exec();
    reader.wait();
    return result;
}

#include "main.moc"
