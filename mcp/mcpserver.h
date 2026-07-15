/*
  mcpserver.h

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef GAMMARAY_MCPSERVER_H
#define GAMMARAY_MCPSERVER_H

#include <QFile>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QAbstractItemModel;
class QImage;
QT_END_NAMESPACE

namespace GammaRay {
class ClientConnectionManager;
class Launcher;
}

class McpServer : public QObject
{
    Q_OBJECT
public:
    explicit McpServer(QObject *parent = nullptr);
    ~McpServer() override;

    bool openStandardOutput();

public slots:
    void handleLine(const QString &line);

private:
    enum class SessionState
    {
        Disconnected,
        Launching,
        Connecting,
        Ready,
        Error
    };

    using SnapshotProducer = std::function<QJsonObject()>;

    void handleRequest(const QJsonObject &request);
    void handleToolCall(const QJsonValue &id, const QJsonObject &params);

    void sendMessage(const QJsonObject &message);
    void sendResult(const QJsonValue &id, const QJsonObject &result);
    void sendProtocolError(const QJsonValue &id, int code, const QString &message,
                           const QJsonValue &data = QJsonValue());
    void sendToolResult(const QJsonValue &id, const QJsonObject &result);
    void sendImageToolResult(const QJsonValue &id, const QJsonObject &result,
                             const QImage &image);
    void sendToolError(const QJsonValue &id, const QString &message,
                       const QJsonObject &details = QJsonObject());

    QJsonArray tools() const;
    QJsonObject status() const;
    QString stateName() const;

    void callLaunch(const QJsonValue &id, const QJsonObject &arguments);
    void callAttach(const QJsonValue &id, const QJsonObject &arguments);
    void callConnect(const QJsonValue &id, const QJsonObject &arguments);
    void callDisconnect(const QJsonValue &id);
    void callListObjects(const QJsonValue &id, const QJsonObject &arguments);
    void callGetProperties(const QJsonValue &id, const QJsonObject &arguments);
    void callGetMessages(const QJsonValue &id, const QJsonObject &arguments);
    void callRunDiagnostics(const QJsonValue &id, const QJsonObject &arguments);
    void callValidateRuntime(const QJsonValue &id, const QJsonObject &arguments);
    void callGrabWidget(const QJsonValue &id, const QJsonObject &arguments);
    void callGrabWindow(const QJsonValue &id, const QJsonObject &arguments);
    void callGrabImage(const QJsonValue &id, const QJsonObject &arguments,
                       bool captureWindow);
    void callListQuickItems(const QJsonValue &id, const QJsonObject &arguments);
    void callGrabQuickWindow(const QJsonValue &id, const QJsonObject &arguments);
    void callGrabQuickItem(const QJsonValue &id, const QJsonObject &arguments);
    void callGrabQuickImage(const QJsonValue &id, const QJsonObject &arguments,
                            bool cropToItem);

    bool ensureReady(const QJsonValue &id) const;
    bool prepareSession(const QJsonValue &id);
    void startLauncherSession(const QJsonValue &id, bool attach, const QJsonObject &arguments);
    void connectToProbe(const QUrl &url);
    void completeSessionRequest();
    void failSessionRequest(const QString &message);
    void resetSession();

    void scheduleStableSnapshot(const QJsonValue &id, SnapshotProducer producer, int timeoutMs,
                                int minimumWaitMs = 300);
    void afterProblemScan(const QJsonValue &id, int timeoutMs, SnapshotProducer producer);

    QJsonObject objectSnapshot(QAbstractItemModel *model, const QString &pattern,
                               int maxDepth, int limit) const;
    QJsonObject quickItemSnapshot(QAbstractItemModel *quickModel,
                                  QAbstractItemModel *objectModel, const QString &pattern,
                                  int maxDepth, int limit) const;
    QJsonObject propertySnapshot(QAbstractItemModel *model, int maxDepth, int limit) const;
    QJsonObject messageSnapshot(QAbstractItemModel *model, const QString &minimumType,
                                int limit) const;
    QJsonObject problemSnapshot(QAbstractItemModel *model) const;
    QJsonObject validationSnapshot(const QJsonObject &arguments) const;

    QFile m_stdout;
    bool m_initialized = false;
    QString m_protocolVersion = QStringLiteral("2025-11-25");

    SessionState m_state = SessionState::Disconnected;
    QString m_lastError;
    QUrl m_serverUrl;
    std::unique_ptr<GammaRay::Launcher> m_launcher;
    std::unique_ptr<GammaRay::ClientConnectionManager> m_connection;

    bool m_hasPendingSessionRequest = false;
    bool m_launcherReportedServer = false;
    bool m_captureRequestActive = false;
    QJsonValue m_pendingSessionRequestId;
};

#endif // GAMMARAY_MCPSERVER_H
