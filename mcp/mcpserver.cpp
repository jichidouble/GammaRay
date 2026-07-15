/*
  mcpserver.cpp

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mcpserver.h"

#include <client/clientconnectionmanager.h>
#include <client/remotemodel.h>
#include <common/endpoint.h>
#include <common/objectbroker.h>
#include <common/objectid.h>
#include <common/objectmodel.h>
#include <common/problem.h>
#include <common/protocol.h>
#include <common/remoteviewinterface.h>
#include <common/remotemodelroles.h>
#include <common/tools/messagehandler/messagemodelroles.h>
#include <common/tools/problemreporter/problemmodelroles.h>
#include <common/tools/problemreporter/problemreporterinterface.h>
#include <config-gammaray-version.h>
#include <launcher/core/launcher.h>
#include <launcher/core/launchoptions.h>
#include <launcher/core/probeabi.h>
#include <launcher/core/probeabidetector.h>
#include <launcher/core/probefinder.h>
#include <ui/clienttoolmanager.h>
#include <widgetinspectorinterface.h>

#include <QAbstractItemModel>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSharedPointer>
#include <QTimer>

#include <algorithm>
#include <cstdio>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <unistd.h>
#endif

using namespace GammaRay;

namespace {

constexpr auto ObjectTreeModelName = "com.kdab.GammaRay.ObjectInspectorTree";
constexpr auto PropertyModelName = "com.kdab.GammaRay.ObjectInspector.properties";
constexpr auto MessageModelName = "com.kdab.GammaRay.MessageModel";
constexpr auto ProblemModelName = "com.kdab.GammaRay.ProblemModel";
constexpr auto WidgetTreeModelName = "com.kdab.GammaRay.WidgetTree";
constexpr auto WidgetRemoteViewName = "com.kdab.GammaRay.WidgetRemoteView";
constexpr auto QuickItemModelName = "com.kdab.GammaRay.QuickItemModel";
constexpr auto QuickRemoteViewName = "com.kdab.GammaRay.QuickRemoteView";

class McpProblemReporterClient : public ProblemReporterInterface
{
    Q_OBJECT
    Q_INTERFACES(GammaRay::ProblemReporterInterface)
public:
    explicit McpProblemReporterClient(QObject *parent)
        : ProblemReporterInterface(parent)
    {
    }

    void requestScan() override
    {
        Endpoint::instance()->invokeObject(objectName(), "requestScan");
    }
};

QObject *createProblemReporterClient(const QString &, QObject *parent)
{
    return new McpProblemReporterClient(parent);
}

class McpWidgetInspectorClient : public WidgetInspectorInterface
{
    Q_OBJECT
    Q_INTERFACES(GammaRay::WidgetInspectorInterface)
public:
    explicit McpWidgetInspectorClient(QObject *parent)
        : WidgetInspectorInterface(parent)
    {
    }

    void saveAsImage(const QString &fileName) override
    {
        Endpoint::instance()->invokeObject(objectName(), "saveAsImage", QVariantList() << fileName);
    }

    void saveAsSvg(const QString &fileName) override
    {
        Endpoint::instance()->invokeObject(objectName(), "saveAsSvg", QVariantList() << fileName);
    }

    void saveAsUiFile(const QString &fileName) override
    {
        Endpoint::instance()->invokeObject(objectName(), "saveAsUiFile", QVariantList() << fileName);
    }

    void analyzePainting() override
    {
        Endpoint::instance()->invokeObject(objectName(), "analyzePainting");
    }

    void requestWidgetScreenshot() override
    {
        Endpoint::instance()->invokeObject(objectName(), "requestWidgetScreenshot");
    }
};

QObject *createWidgetInspectorClient(const QString &, QObject *parent)
{
    return new McpWidgetInspectorClient(parent);
}

QJsonObject emptyObjectSchema()
{
    return {
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("additionalProperties"), false }
    };
}

QJsonObject stringSchema(const QString &description)
{
    return {
        { QStringLiteral("type"), QStringLiteral("string") },
        { QStringLiteral("description"), description }
    };
}

QJsonObject integerSchema(const QString &description, int minimum, int maximum, int defaultValue)
{
    return {
        { QStringLiteral("type"), QStringLiteral("integer") },
        { QStringLiteral("description"), description },
        { QStringLiteral("minimum"), minimum },
        { QStringLiteral("maximum"), maximum },
        { QStringLiteral("default"), defaultValue }
    };
}

QJsonObject enumSchema(const QString &description, const QStringList &values,
                       const QString &defaultValue)
{
    QJsonArray entries;
    for (const QString &value : values)
        entries.append(value);
    return {
        { QStringLiteral("type"), QStringLiteral("string") },
        { QStringLiteral("description"), description },
        { QStringLiteral("enum"), entries },
        { QStringLiteral("default"), defaultValue }
    };
}

QJsonObject toolDefinition(const QString &name, const QString &title, const QString &description,
                           const QJsonObject &inputSchema, bool readOnly, bool idempotent,
                           bool destructive, bool openWorld)
{
    return {
        { QStringLiteral("name"), name },
        { QStringLiteral("title"), title },
        { QStringLiteral("description"), description },
        { QStringLiteral("inputSchema"), inputSchema },
        { QStringLiteral("annotations"), QJsonObject { { QStringLiteral("readOnlyHint"), readOnly }, { QStringLiteral("idempotentHint"), idempotent }, { QStringLiteral("destructiveHint"), destructive }, { QStringLiteral("openWorldHint"), openWorld } } }
    };
}

int boundedInteger(const QJsonObject &object, const QString &name, int defaultValue,
                   int minimum, int maximum)
{
    const int value = object.value(name).toInt(defaultValue);
    return std::clamp(value, minimum, maximum);
}

int messageRank(int type)
{
    switch (static_cast<QtMsgType>(type)) {
    case QtDebugMsg:
        return 0;
    case QtInfoMsg:
        return 1;
    case QtWarningMsg:
        return 2;
    case QtCriticalMsg:
        return 3;
    case QtFatalMsg:
        return 4;
    }
    return 0;
}

QString messageTypeName(int type)
{
    switch (static_cast<QtMsgType>(type)) {
    case QtDebugMsg:
        return QStringLiteral("debug");
    case QtInfoMsg:
        return QStringLiteral("info");
    case QtWarningMsg:
        return QStringLiteral("warning");
    case QtCriticalMsg:
        return QStringLiteral("critical");
    case QtFatalMsg:
        return QStringLiteral("fatal");
    }
    return QStringLiteral("unknown");
}

int messageRank(const QString &type)
{
    if (type == QLatin1String("fatal"))
        return 4;
    if (type == QLatin1String("critical"))
        return 3;
    if (type == QLatin1String("warning"))
        return 2;
    if (type == QLatin1String("info"))
        return 1;
    return 0;
}

QString indexPath(const QVector<int> &rows)
{
    QStringList parts;
    parts.reserve(rows.size());
    for (int row : rows)
        parts.append(QString::number(row));
    return parts.join(QLatin1Char('/'));
}

int loadingState(const QModelIndex &index)
{
    const int state = index.data(RemoteModelRole::LoadingState).toInt();
    const int pending = RemoteModelNodeState::Empty | RemoteModelNodeState::Loading
        | RemoteModelNodeState::Outdated;
    return state & pending ? 1 : 0;
}

QModelIndex indexFromPath(QAbstractItemModel *model, const QString &path)
{
    if (!model || path.trimmed().isEmpty())
        return { };

    QModelIndex parent;
    const QStringList components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        bool ok = false;
        const int row = component.toInt(&ok);
        if (!ok || row < 0 || row >= model->rowCount(parent))
            return { };
        parent = model->index(row, 0, parent);
        if (!parent.isValid())
            return { };
    }
    return parent;
}

QModelIndex indexForObjectId(QAbstractItemModel *model, const ObjectId &id)
{
    if (!model || id.isNull())
        return { };

    const QModelIndex first = model->index(0, 0);
    if (!first.isValid())
        return { };
    const auto matches = model->match(first, ObjectModel::ObjectIdRole, QVariant::fromValue(id), 1,
                                      Qt::MatchExactly | Qt::MatchRecursive | Qt::MatchWrap);
    return matches.isEmpty() ? QModelIndex() : matches.first();
}

QJsonObject imageSizeObject(const QImage &image)
{
    return { { QStringLiteral("width"), image.width() },
             { QStringLiteral("height"), image.height() } };
}

QJsonObject countsObject(int debug, int info, int warning, int critical, int fatal)
{
    return {
        { QStringLiteral("debug"), debug },
        { QStringLiteral("info"), info },
        { QStringLiteral("warning"), warning },
        { QStringLiteral("critical"), critical },
        { QStringLiteral("fatal"), fatal }
    };
}

} // namespace

McpServer::McpServer(QObject *parent)
    : QObject(parent)
{
    ObjectBroker::registerClientObjectFactoryCallback<ProblemReporterInterface *>(
        createProblemReporterClient);
    ObjectBroker::registerClientObjectFactoryCallback<WidgetInspectorInterface *>(
        createWidgetInspectorClient);
}

McpServer::~McpServer()
{
    resetSession();
}

bool McpServer::openStandardOutput()
{
    return m_stdout.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered,
                         QFileDevice::DontCloseHandle);
}

void McpServer::handleLine(const QString &line)
{
    if (line.trimmed().isEmpty())
        return;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        sendProtocolError(QJsonValue::Null, -32700,
                          QStringLiteral("Parse error: %1").arg(parseError.errorString()));
        return;
    }
    handleRequest(document.object());
}

void McpServer::handleRequest(const QJsonObject &request)
{
    const QString method = request.value(QStringLiteral("method")).toString();
    const bool hasId = request.contains(QStringLiteral("id"));
    const QJsonValue id = hasId ? request.value(QStringLiteral("id")) : QJsonValue();

    if (request.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0")
        || method.isEmpty()) {
        if (hasId)
            sendProtocolError(id, -32600, QStringLiteral("Invalid JSON-RPC request."));
        return;
    }

    if (method == QLatin1String("initialize")) {
        if (!hasId)
            return;
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();
        const QString requestedVersion = params.value(QStringLiteral("protocolVersion")).toString();
        static const QSet<QString> supportedVersions {
            QStringLiteral("2025-11-25"),
            QStringLiteral("2025-06-18"),
            QStringLiteral("2024-11-05")
        };
        m_protocolVersion = supportedVersions.contains(requestedVersion)
            ? requestedVersion
            : QStringLiteral("2025-11-25");
        m_initialized = true;
        sendResult(id, { { QStringLiteral("protocolVersion"), m_protocolVersion }, { QStringLiteral("capabilities"), QJsonObject { { QStringLiteral("tools"), QJsonObject { { QStringLiteral("listChanged"), false } } } } }, { QStringLiteral("serverInfo"), QJsonObject { { QStringLiteral("name"), QStringLiteral("gammaray-mcp") }, { QStringLiteral("title"), QStringLiteral("GammaRay Qt Runtime Monitor") }, { QStringLiteral("version"), QStringLiteral(GAMMARAY_MCP_VERSION) } } }, { QStringLiteral("instructions"), QStringLiteral("Launch, attach, or connect to one Qt process, then inspect its "
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              "QObject tree, properties, Qt messages, and GammaRay diagnostics. "
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              "Use gammaray_disconnect before switching targets.") } });
        return;
    }

    if (method == QLatin1String("notifications/initialized")
        || method == QLatin1String("notifications/cancelled")) {
        return;
    }

    if (!m_initialized) {
        if (hasId)
            sendProtocolError(id, -32002, QStringLiteral("Server has not been initialized."));
        return;
    }

    if (method == QLatin1String("ping")) {
        if (hasId)
            sendResult(id, { });
        return;
    }
    if (method == QLatin1String("tools/list")) {
        if (hasId)
            sendResult(id, { { QStringLiteral("tools"), tools() } });
        return;
    }
    if (method == QLatin1String("tools/call")) {
        if (hasId)
            handleToolCall(id, request.value(QStringLiteral("params")).toObject());
        return;
    }

    if (hasId)
        sendProtocolError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

void McpServer::handleToolCall(const QJsonValue &id, const QJsonObject &params)
{
    const QString name = params.value(QStringLiteral("name")).toString();
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();

    if (name == QLatin1String("gammaray_status"))
        sendToolResult(id, status());
    else if (name == QLatin1String("gammaray_launch"))
        callLaunch(id, arguments);
    else if (name == QLatin1String("gammaray_attach"))
        callAttach(id, arguments);
    else if (name == QLatin1String("gammaray_connect"))
        callConnect(id, arguments);
    else if (name == QLatin1String("gammaray_disconnect"))
        callDisconnect(id);
    else if (name == QLatin1String("gammaray_list_objects"))
        callListObjects(id, arguments);
    else if (name == QLatin1String("gammaray_get_properties"))
        callGetProperties(id, arguments);
    else if (name == QLatin1String("gammaray_get_messages"))
        callGetMessages(id, arguments);
    else if (name == QLatin1String("gammaray_run_diagnostics"))
        callRunDiagnostics(id, arguments);
    else if (name == QLatin1String("gammaray_validate_runtime"))
        callValidateRuntime(id, arguments);
    else if (name == QLatin1String("gammaray_grab_widget"))
        callGrabWidget(id, arguments);
    else if (name == QLatin1String("gammaray_grab_window"))
        callGrabWindow(id, arguments);
    else if (name == QLatin1String("gammaray_list_quick_items"))
        callListQuickItems(id, arguments);
    else if (name == QLatin1String("gammaray_grab_quick_window"))
        callGrabQuickWindow(id, arguments);
    else if (name == QLatin1String("gammaray_grab_quick_item"))
        callGrabQuickItem(id, arguments);
    else
        sendProtocolError(id, -32602, QStringLiteral("Unknown tool: %1").arg(name));
}

void McpServer::sendMessage(const QJsonObject &message)
{
    if (!m_stdout.isOpen())
        return;
    QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    payload.append('\n');
    qsizetype written = 0;
    while (written < payload.size()) {
#ifdef Q_OS_WIN
        DWORD chunk = 0;
        const BOOL ok = WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), payload.constData() + written,
                                  static_cast<DWORD>(payload.size() - written), &chunk, nullptr);
        if (!ok || chunk == 0)
            break;
        written += static_cast<qsizetype>(chunk);
#else
        const ssize_t chunk = ::write(STDOUT_FILENO, payload.constData() + written,
                                      static_cast<size_t>(payload.size() - written));
        if (chunk <= 0)
            break;
        written += static_cast<qsizetype>(chunk);
#endif
    }
}

void McpServer::sendResult(const QJsonValue &id, const QJsonObject &result)
{
    sendMessage({ { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
                  { QStringLiteral("id"), id },
                  { QStringLiteral("result"), result } });
}

void McpServer::sendProtocolError(const QJsonValue &id, int code, const QString &message,
                                  const QJsonValue &data)
{
    QJsonObject error {
        { QStringLiteral("code"), code },
        { QStringLiteral("message"), message }
    };
    if (!data.isUndefined())
        error.insert(QStringLiteral("data"), data);
    sendMessage({ { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
                  { QStringLiteral("id"), id },
                  { QStringLiteral("error"), error } });
}

void McpServer::sendToolResult(const QJsonValue &id, const QJsonObject &result)
{
    const QString text = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    sendResult(id, { { QStringLiteral("content"), QJsonArray { QJsonObject { { QStringLiteral("type"), QStringLiteral("text") }, { QStringLiteral("text"), text } } } }, { QStringLiteral("structuredContent"), result }, { QStringLiteral("isError"), false } });
}

void McpServer::sendImageToolResult(const QJsonValue &id, const QJsonObject &result,
                                    const QImage &image)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        sendToolError(id, QStringLiteral("Could not encode the screenshot as PNG."));
        return;
    }

    const QString text = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    sendResult(id, { { QStringLiteral("content"), QJsonArray {
                          QJsonObject { { QStringLiteral("type"), QStringLiteral("text") },
                                        { QStringLiteral("text"), text } },
                          QJsonObject { { QStringLiteral("type"), QStringLiteral("image") },
                                        { QStringLiteral("data"), QString::fromLatin1(buffer.data().toBase64()) },
                                        { QStringLiteral("mimeType"), QStringLiteral("image/png") } }
                      } },
                     { QStringLiteral("structuredContent"), result },
                     { QStringLiteral("isError"), false } });
}

void McpServer::sendToolError(const QJsonValue &id, const QString &message,
                              const QJsonObject &details)
{
    QJsonObject result = details;
    result.insert(QStringLiteral("error"), message);
    const QString text = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    sendResult(id, { { QStringLiteral("content"), QJsonArray { QJsonObject { { QStringLiteral("type"), QStringLiteral("text") }, { QStringLiteral("text"), text } } } }, { QStringLiteral("structuredContent"), result }, { QStringLiteral("isError"), true } });
}

QJsonArray McpServer::tools() const
{
    QJsonArray result;
    result.append(toolDefinition(
        QStringLiteral("gammaray_status"), QStringLiteral("GammaRay status"),
        QStringLiteral("Report the local GammaRay/Qt environment, compatible probes, and current target session."),
        emptyObjectSchema(), true, true, false, false));

    const QJsonObject probeProperty = stringSchema(
        QStringLiteral("Optional probe ABI id from gammaray_status. Usually auto-detected."));
    const QJsonObject injectorProperty = stringSchema(
        QStringLiteral("Optional GammaRay injector name. Leave empty to use the platform default."));
    result.append(toolDefinition(
        QStringLiteral("gammaray_launch"), QStringLiteral("Launch Qt target"),
        QStringLiteral("Start an executable directly (without a shell), inject the GammaRay probe, and connect the MCP session."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("executable"), stringSchema(QStringLiteral("Absolute path to the Qt executable.")) }, { QStringLiteral("arguments"), QJsonObject { { QStringLiteral("type"), QStringLiteral("array") }, { QStringLiteral("items"), QJsonObject { { QStringLiteral("type"), QStringLiteral("string") } } }, { QStringLiteral("default"), QJsonArray() } } }, { QStringLiteral("workingDirectory"), stringSchema(QStringLiteral("Optional target working directory.")) }, { QStringLiteral("environment"), QJsonObject { { QStringLiteral("type"), QStringLiteral("object") }, { QStringLiteral("additionalProperties"), QJsonObject { { QStringLiteral("type"), QStringLiteral("string") } } } } }, { QStringLiteral("probe"), probeProperty }, { QStringLiteral("injector"), injectorProperty } } },
            { QStringLiteral("required"), QJsonArray { QStringLiteral("executable") } },
            { QStringLiteral("additionalProperties"), false } },
        false, false, false, true));

    result.append(toolDefinition(
        QStringLiteral("gammaray_attach"), QStringLiteral("Attach to Qt target"),
        QStringLiteral("Inject GammaRay into an already running local Qt process and connect the MCP session."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("pid"), QJsonObject { { QStringLiteral("type"), QStringLiteral("integer") }, { QStringLiteral("minimum"), 1 }, { QStringLiteral("description"), QStringLiteral("Operating-system process id.") } } }, { QStringLiteral("probe"), probeProperty }, { QStringLiteral("injector"), injectorProperty } } },
            { QStringLiteral("required"), QJsonArray { QStringLiteral("pid") } },
            { QStringLiteral("additionalProperties"), false } },
        false, false, false, true));

    result.append(toolDefinition(
        QStringLiteral("gammaray_connect"), QStringLiteral("Connect to GammaRay probe"),
        QStringLiteral("Connect to an existing GammaRay probe endpoint such as tcp://127.0.0.1:11732."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("url"), stringSchema(QStringLiteral("GammaRay probe URL.")) } } },
            { QStringLiteral("required"), QJsonArray { QStringLiteral("url") } },
            { QStringLiteral("additionalProperties"), false } },
        false, false, false, true));

    result.append(toolDefinition(
        QStringLiteral("gammaray_disconnect"), QStringLiteral("Disconnect target"),
        QStringLiteral("Close the current GammaRay client session without terminating the inspected application."),
        emptyObjectSchema(), false, true, false, false));

    result.append(toolDefinition(
        QStringLiteral("gammaray_list_objects"), QStringLiteral("List QObject tree"),
        QStringLiteral("Read a bounded, flattened snapshot of the live QObject tree. Returned paths identify objects for property inspection."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("pattern"), stringSchema(QStringLiteral("Optional case-insensitive regular expression matched against object name and type.")) }, { QStringLiteral("maxDepth"), integerSchema(QStringLiteral("Maximum QObject tree depth."), 0, 20, 5) }, { QStringLiteral("limit"), integerSchema(QStringLiteral("Maximum returned objects."), 1, 2000, 250) }, { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Remote model loading timeout."), 250, 30000, 5000) } } },
            { QStringLiteral("additionalProperties"), false } },
        true, true, false, false));

    result.append(toolDefinition(
        QStringLiteral("gammaray_get_properties"), QStringLiteral("Inspect QObject properties"),
        QStringLiteral("Select an QObject by a path returned from gammaray_list_objects and read its live Qt properties."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("objectPath"), stringSchema(QStringLiteral("Slash-separated row path from gammaray_list_objects, for example 0/2/1.")) }, { QStringLiteral("maxDepth"), integerSchema(QStringLiteral("Maximum nested property depth."), 0, 10, 3) }, { QStringLiteral("limit"), integerSchema(QStringLiteral("Maximum returned properties."), 1, 2000, 300) }, { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Remote model loading timeout."), 250, 30000, 5000) } } },
            { QStringLiteral("required"), QJsonArray { QStringLiteral("objectPath") } },
            { QStringLiteral("additionalProperties"), false } },
        true, true, false, false));

    result.append(toolDefinition(
        QStringLiteral("gammaray_get_messages"), QStringLiteral("Read Qt messages"),
        QStringLiteral("Read captured qDebug/qInfo/qWarning/qCritical/qFatal messages from the target process."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("minimumType"), enumSchema(QStringLiteral("Minimum included message type."), { QStringLiteral("debug"), QStringLiteral("info"), QStringLiteral("warning"), QStringLiteral("critical"), QStringLiteral("fatal") }, QStringLiteral("debug")) }, { QStringLiteral("limit"), integerSchema(QStringLiteral("Maximum newest messages returned."), 1, 2000, 200) }, { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Remote model loading timeout."), 250, 30000, 5000) } } },
            { QStringLiteral("additionalProperties"), false } },
        true, true, false, false));

    result.append(toolDefinition(
        QStringLiteral("gammaray_run_diagnostics"), QStringLiteral("Run GammaRay diagnostics"),
        QStringLiteral("Run GammaRay problem checkers (bindings, connections, thread affinity, and enabled plugin checks) and return findings."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Scan and remote model timeout."), 500, 60000, 10000) } } },
            { QStringLiteral("additionalProperties"), false } },
        true, false, false, false));

    result.append(toolDefinition(
        QStringLiteral("gammaray_validate_runtime"), QStringLiteral("Validate Qt runtime"),
        QStringLiteral("Run diagnostics and evaluate the current Qt messages and GammaRay problems against explicit pass/fail thresholds."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("maxWarningMessages"), integerSchema(QStringLiteral("Maximum allowed qWarning messages in the captured window."), 0, 100000, 0) }, { QStringLiteral("maxCriticalMessages"), integerSchema(QStringLiteral("Maximum allowed qCritical plus qFatal messages."), 0, 100000, 0) }, { QStringLiteral("failOnProblemSeverity"), enumSchema(QStringLiteral("Minimum GammaRay problem severity that fails validation."), { QStringLiteral("none"), QStringLiteral("warning"), QStringLiteral("error") }, QStringLiteral("error")) }, { QStringLiteral("messageLimit"), integerSchema(QStringLiteral("Newest messages inspected and returned."), 1, 5000, 1000) }, { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Scan and remote model timeout."), 500, 60000, 10000) } } },
            { QStringLiteral("additionalProperties"), false } },
        true, false, false, false));

    const QJsonObject screenshotProperties {
        { QStringLiteral("objectPath"), stringSchema(QStringLiteral("Slash-separated QObject path returned by gammaray_list_objects. It must identify a QWidget or a layout owned by one.")) },
        { QStringLiteral("maxWidth"), integerSchema(QStringLiteral("Maximum PNG width; larger images are scaled down without changing aspect ratio."), 1, 8192, 1920) },
        { QStringLiteral("maxHeight"), integerSchema(QStringLiteral("Maximum PNG height; larger images are scaled down without changing aspect ratio."), 1, 8192, 1080) },
        { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Remote Widget Inspector selection and image request timeout."), 250, 30000, 10000) }
    };
    const QJsonObject screenshotSchema {
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), screenshotProperties },
        { QStringLiteral("required"), QJsonArray { QStringLiteral("objectPath") } },
        { QStringLiteral("additionalProperties"), false }
    };
    result.append(toolDefinition(
        QStringLiteral("gammaray_grab_widget"), QStringLiteral("Capture QWidget PNG"),
        QStringLiteral("Capture the selected QWidget itself as an in-memory PNG through GammaRay's Widget Inspector. Returns an MCP image content item and size metadata."),
        screenshotSchema, true, true, false, false));
    result.append(toolDefinition(
        QStringLiteral("gammaray_grab_window"), QStringLiteral("Capture top-level window PNG"),
        QStringLiteral("Capture the top-level QWidget window containing the selected QWidget as an in-memory PNG through GammaRay's remote view. Returns an MCP image content item and size metadata."),
        screenshotSchema, true, true, false, false));

    result.append(toolDefinition(
        QStringLiteral("gammaray_list_quick_items"), QStringLiteral("List Qt Quick item tree"),
        QStringLiteral("Read a bounded snapshot of the active Qt Quick item hierarchy. Returned object paths identify items for property inspection and Quick capture."),
        QJsonObject {
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"), QJsonObject { { QStringLiteral("pattern"), stringSchema(QStringLiteral("Optional case-insensitive regular expression matched against item name and type.")) }, { QStringLiteral("maxDepth"), integerSchema(QStringLiteral("Maximum Qt Quick item tree depth."), 0, 20, 5) }, { QStringLiteral("limit"), integerSchema(QStringLiteral("Maximum returned Qt Quick items."), 1, 2000, 250) }, { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Remote model loading timeout."), 250, 30000, 5000) } } },
            { QStringLiteral("additionalProperties"), false } },
        true, true, false, false));

    const QJsonObject quickCaptureProperties {
        { QStringLiteral("objectPath"), stringSchema(QStringLiteral("Slash-separated QObject path returned by gammaray_list_quick_items. It must identify a QQuickItem in the active scene.")) },
        { QStringLiteral("maxWidth"), integerSchema(QStringLiteral("Maximum PNG width; larger images are scaled down without changing aspect ratio."), 1, 8192, 1920) },
        { QStringLiteral("maxHeight"), integerSchema(QStringLiteral("Maximum PNG height; larger images are scaled down without changing aspect ratio."), 1, 8192, 1080) },
        { QStringLiteral("timeoutMs"), integerSchema(QStringLiteral("Remote Quick Inspector selection and image request timeout."), 250, 30000, 10000) }
    };
    const QJsonObject quickCaptureSchema {
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), quickCaptureProperties },
        { QStringLiteral("required"), QJsonArray { QStringLiteral("objectPath") } },
        { QStringLiteral("additionalProperties"), false }
    };
    result.append(toolDefinition(
        QStringLiteral("gammaray_grab_quick_window"), QStringLiteral("Capture Qt Quick window PNG"),
        QStringLiteral("Capture the complete Qt Quick window containing the selected QQuickItem as an in-memory PNG through GammaRay's Quick Inspector remote view."),
        quickCaptureSchema, true, true, false, false));
    result.append(toolDefinition(
        QStringLiteral("gammaray_grab_quick_item"), QStringLiteral("Capture Qt Quick item PNG"),
        QStringLiteral("Capture the selected QQuickItem as an in-memory PNG by cropping GammaRay's Quick Inspector remote-view frame."),
        quickCaptureSchema, true, true, false, false));
    return result;
}

QString McpServer::stateName() const
{
    switch (m_state) {
    case SessionState::Disconnected:
        return QStringLiteral("disconnected");
    case SessionState::Launching:
        return QStringLiteral("launching");
    case SessionState::Connecting:
        return QStringLiteral("connecting");
    case SessionState::Ready:
        return QStringLiteral("ready");
    case SessionState::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("unknown");
}

QJsonObject McpServer::status() const
{
    QJsonArray probes;
    const QVector<ProbeABI> availableProbes = ProbeFinder::listProbeABIs();
    for (const ProbeABI &probe : availableProbes) {
        probes.append(QJsonObject {
            { QStringLiteral("id"), probe.id() },
            { QStringLiteral("displayName"), probe.displayString() },
            { QStringLiteral("path"), ProbeFinder::findProbe(probe) } });
    }

    QJsonObject session {
        { QStringLiteral("state"), stateName() },
        { QStringLiteral("serverUrl"), m_serverUrl.toString() }
    };
    if (!m_lastError.isEmpty())
        session.insert(QStringLiteral("lastError"), m_lastError);
    if (m_connection && m_state == SessionState::Ready) {
        session.insert(QStringLiteral("targetPid"), m_connection->endPointPid());
        session.insert(QStringLiteral("targetLabel"), m_connection->endPointLabel());
        session.insert(QStringLiteral("targetKey"), m_connection->endPointKey());

        QJsonArray targetTools;
        const auto toolManager = m_connection->toolManager();
        if (toolManager) {
            for (const ToolInfo &tool : toolManager->tools()) {
                targetTools.append(QJsonObject {
                    { QStringLiteral("id"), tool.id() },
                    { QStringLiteral("name"), tool.name() },
                    { QStringLiteral("enabled"), tool.isEnabled() },
                    { QStringLiteral("remotingSupported"), tool.remotingSupported() } });
            }
        }
        session.insert(QStringLiteral("targetTools"), targetTools);
    }

    return {
        { QStringLiteral("mcpServerVersion"), QStringLiteral(GAMMARAY_MCP_VERSION) },
        { QStringLiteral("mcpProtocolVersion"), m_protocolVersion },
        { QStringLiteral("gammaRayVersion"), QStringLiteral(GAMMARAY_VERSION_STRING) },
        { QStringLiteral("gammaRayProtocolVersion"), Protocol::version() },
        { QStringLiteral("qtRuntimeVersion"), QString::fromLatin1(qVersion()) },
        { QStringLiteral("probes"), probes },
        { QStringLiteral("session"), session }
    };
}

bool McpServer::ensureReady(const QJsonValue &id) const
{
    if (m_state == SessionState::Ready && m_connection)
        return true;
    const_cast<McpServer *>(this)->sendToolError(
        id, QStringLiteral("No ready GammaRay target session. Call gammaray_launch, "
                           "gammaray_attach, or gammaray_connect first."),
        { { QStringLiteral("state"), stateName() } });
    return false;
}

bool McpServer::prepareSession(const QJsonValue &id)
{
    if (m_state == SessionState::Error)
        resetSession();
    if (m_state != SessionState::Disconnected) {
        sendToolError(id, QStringLiteral("A GammaRay session is already active. Disconnect it first."),
                      { { QStringLiteral("state"), stateName() } });
        return false;
    }
    m_hasPendingSessionRequest = true;
    m_pendingSessionRequestId = id;
    m_launcherReportedServer = false;
    m_lastError.clear();
    return true;
}

void McpServer::callLaunch(const QJsonValue &id, const QJsonObject &arguments)
{
    startLauncherSession(id, false, arguments);
}

void McpServer::callAttach(const QJsonValue &id, const QJsonObject &arguments)
{
    startLauncherSession(id, true, arguments);
}

void McpServer::startLauncherSession(const QJsonValue &id, bool attach,
                                     const QJsonObject &arguments)
{
    if (!prepareSession(id))
        return;

    LaunchOptions options;
    if (attach) {
        const qint64 pid = arguments.value(QStringLiteral("pid")).toInteger();
        if (pid <= 0) {
            failSessionRequest(QStringLiteral("pid must be a positive integer."));
            return;
        }
        options.setPid(pid);
    } else {
        const QString executable = arguments.value(QStringLiteral("executable")).toString();
        const QFileInfo executableInfo(executable);
        if (executable.isEmpty() || !executableInfo.isFile()) {
            failSessionRequest(QStringLiteral("Executable does not exist: %1").arg(executable));
            return;
        }
        QStringList launchArguments { executableInfo.absoluteFilePath() };
        const QJsonArray jsonArguments = arguments.value(QStringLiteral("arguments")).toArray();
        for (const QJsonValue &argument : jsonArguments)
            launchArguments.append(argument.toString());
        options.setLaunchArguments(launchArguments);

        QString workingDirectory = arguments.value(QStringLiteral("workingDirectory")).toString();
        if (workingDirectory.isEmpty())
            workingDirectory = executableInfo.absolutePath();
        if (!QFileInfo(workingDirectory).isDir()) {
            failSessionRequest(QStringLiteral("Working directory does not exist: %1").arg(workingDirectory));
            return;
        }
        options.setWorkingDirectory(workingDirectory);

        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        const QJsonObject overrides = arguments.value(QStringLiteral("environment")).toObject();
        for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it)
            environment.insert(it.key(), it.value().toString());
        options.setProcessEnvironment(environment);
    }

    options.setUiMode(LaunchOptions::NoUi);
    options.setProbeSetting(QStringLiteral("ServerAddress"), QStringLiteral("tcp://127.0.0.1"));
    options.setProbeSetting(QStringLiteral("SuppressLauncherStandardOutput"), true);

    const QString injector = arguments.value(QStringLiteral("injector")).toString();
    if (!injector.isEmpty())
        options.setInjectorType(injector);

    ProbeABI targetAbi;
    const QString requestedProbe = arguments.value(QStringLiteral("probe")).toString();
    if (!requestedProbe.isEmpty()) {
        targetAbi = ProbeABI::fromString(requestedProbe);
        if (!targetAbi.isValid() || ProbeFinder::findProbe(targetAbi).isEmpty()) {
            failSessionRequest(QStringLiteral("Unknown or unavailable probe ABI: %1").arg(requestedProbe));
            return;
        }
    } else {
        ProbeABIDetector detector;
        targetAbi = attach ? detector.abiForProcess(options.pid())
                           : detector.abiForExecutable(options.absoluteExecutablePath());
        targetAbi = ProbeFinder::findBestMatchingABI(targetAbi);
        if (!targetAbi.isValid()) {
            failSessionRequest(QStringLiteral("Could not detect a compatible GammaRay probe ABI."));
            return;
        }
    }
    options.setProbeABI(targetAbi);

    m_state = SessionState::Launching;
    m_launcher = std::make_unique<Launcher>(options);
    connect(m_launcher.get(), &Launcher::started, this, [this]() {
        if (!m_launcher || !m_hasPendingSessionRequest)
            return;
        m_launcherReportedServer = true;
        m_serverUrl = m_launcher->serverAddress();
        connectToProbe(m_serverUrl);
    });
    connect(m_launcher.get(), &Launcher::finished, this, [this]() {
        if (!m_launcher || !m_hasPendingSessionRequest || m_launcherReportedServer)
            return;
        if (m_launcher->exitCode() != 0 || !m_launcher->errorMessage().isEmpty()) {
            failSessionRequest(m_launcher->errorMessage().isEmpty()
                                   ? QStringLiteral("GammaRay injection failed with exit code %1.")
                                         .arg(m_launcher->exitCode())
                                   : m_launcher->errorMessage());
        }
    });
    connect(m_launcher.get(), &Launcher::stderrMessage, this, [](const QString &message) {
        std::fprintf(stderr, "GammaRay injector: %s\n", qPrintable(message));
    });

    if (!m_launcher->start()) {
        failSessionRequest(m_launcher->errorMessage().isEmpty()
                               ? QStringLiteral("GammaRay launcher failed to start.")
                               : m_launcher->errorMessage());
        return;
    }

    QTimer::singleShot(60000, this, [this]() {
        if (m_hasPendingSessionRequest)
            failSessionRequest(QStringLiteral("Timed out waiting for the GammaRay target session."));
    });
}

void McpServer::callConnect(const QJsonValue &id, const QJsonObject &arguments)
{
    if (!prepareSession(id))
        return;
    const QUrl url(arguments.value(QStringLiteral("url")).toString());
    if (!url.isValid() || url.scheme().isEmpty()) {
        failSessionRequest(QStringLiteral("Invalid GammaRay probe URL."));
        return;
    }
    m_serverUrl = url;
    connectToProbe(url);
    QTimer::singleShot(60000, this, [this]() {
        if (m_hasPendingSessionRequest)
            failSessionRequest(QStringLiteral("Timed out connecting to the GammaRay probe."));
    });
}

void McpServer::connectToProbe(const QUrl &url)
{
    m_state = SessionState::Connecting;
    m_connection = std::make_unique<ClientConnectionManager>(nullptr, false);
    connect(m_connection.get(), &ClientConnectionManager::ready, this, [this]() {
        m_state = SessionState::Ready;
        m_lastError.clear();
        completeSessionRequest();
    });
    connect(m_connection.get(), &ClientConnectionManager::persistentConnectionError,
            this, [this](const QString &message) {
                m_state = SessionState::Error;
                m_lastError = message;
                failSessionRequest(message);
            });
    connect(m_connection.get(), &ClientConnectionManager::disconnected, this, [this]() {
        if (m_state == SessionState::Ready) {
            m_state = SessionState::Disconnected;
            m_lastError = QStringLiteral("Target connection closed.");
        }
    });
    m_connection->connectToHost(url, 10);
}

void McpServer::completeSessionRequest()
{
    if (!m_hasPendingSessionRequest)
        return;
    const QJsonValue id = m_pendingSessionRequestId;
    m_hasPendingSessionRequest = false;
    m_pendingSessionRequestId = QJsonValue();
    sendToolResult(id, status());
}

void McpServer::failSessionRequest(const QString &message)
{
    m_lastError = message.trimmed();
    m_state = SessionState::Error;
    if (!m_hasPendingSessionRequest)
        return;
    const QJsonValue id = m_pendingSessionRequestId;
    m_hasPendingSessionRequest = false;
    m_pendingSessionRequestId = QJsonValue();
    sendToolError(id, m_lastError, { { QStringLiteral("state"), stateName() } });
}

void McpServer::callDisconnect(const QJsonValue &id)
{
    const bool hadSession = m_state != SessionState::Disconnected;
    resetSession();
    sendToolResult(id, { { QStringLiteral("disconnected"), hadSession }, { QStringLiteral("state"), stateName() } });
}

void McpServer::resetSession()
{
    m_hasPendingSessionRequest = false;
    if (m_connection)
        m_connection->disconnectFromHost();
    m_connection.reset();
    m_launcher.reset();
    m_state = SessionState::Disconnected;
    m_serverUrl.clear();
    m_launcherReportedServer = false;
}

void McpServer::scheduleStableSnapshot(const QJsonValue &id, SnapshotProducer producer,
                                       int timeoutMs, int minimumWaitMs)
{
    struct State
    {
        QElapsedTimer elapsed;
        QByteArray previous;
        int stablePasses = 0;
    };
    auto state = QSharedPointer<State>::create();
    state->elapsed.start();
    auto timer = new QTimer(this);
    timer->setInterval(75);

    connect(timer, &QTimer::timeout, this, [this, timer, state, id, producer, timeoutMs, minimumWaitMs]() {
        QJsonObject snapshot = producer();
        const int loading = snapshot.take(QStringLiteral("_loading")).toInt();
        const QByteArray current = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
        if (current == state->previous)
            ++state->stablePasses;
        else
            state->stablePasses = 0;
        state->previous = current;

        const bool timedOut = state->elapsed.elapsed() >= timeoutMs;
        const bool stable = state->elapsed.elapsed() >= minimumWaitMs
            && loading == 0 && state->stablePasses >= 2;
        if (!timedOut && !stable)
            return;

        snapshot.insert(QStringLiteral("complete"), stable);
        snapshot.insert(QStringLiteral("timedOut"), timedOut && !stable);
        snapshot.insert(QStringLiteral("loadingCells"), loading);
        timer->stop();
        timer->deleteLater();
        sendToolResult(id, snapshot);
    });
    timer->start();
}

QJsonObject McpServer::objectSnapshot(QAbstractItemModel *model, const QString &pattern,
                                      int maxDepth, int limit) const
{
    QJsonArray objects;
    int loading = 0;
    int visited = 0;
    bool truncated = false;
    QRegularExpression expression;
    if (!pattern.isEmpty())
        expression = QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption);

    std::function<void(const QModelIndex &, int, QVector<int>)> visit;
    visit = [&](const QModelIndex &parent, int depth, QVector<int> path) {
        if (truncated || depth > maxDepth)
            return;
        const int rows = model->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            if (++visited > 10000) {
                truncated = true;
                return;
            }
            path.append(row);
            const QModelIndex nameIndex = model->index(row, 0, parent);
            const QModelIndex typeIndex = model->index(row, 1, parent);
            const QString name = nameIndex.data(Qt::DisplayRole).toString();
            const QString type = typeIndex.data(Qt::DisplayRole).toString();
            loading += loadingState(nameIndex) + loadingState(typeIndex);

            const bool matches = pattern.isEmpty()
                || expression.match(name + QLatin1Char(' ') + type).hasMatch();
            if (matches) {
                const ObjectId objectId = nameIndex.data(ObjectModel::ObjectIdRole).value<ObjectId>();
                QJsonObject entry {
                    { QStringLiteral("path"), indexPath(path) },
                    { QStringLiteral("depth"), depth },
                    { QStringLiteral("name"), name },
                    { QStringLiteral("type"), type }
                };
                if (!objectId.isNull())
                    entry.insert(QStringLiteral("objectId"),
                                 QStringLiteral("0x%1").arg(objectId.id(), 0, 16));
                objects.append(entry);
                if (objects.size() >= limit) {
                    truncated = true;
                    path.removeLast();
                    return;
                }
            }
            if (depth < maxDepth)
                visit(nameIndex, depth + 1, path);
            path.removeLast();
            if (truncated)
                return;
        }
    };
    visit(QModelIndex(), 0, { });

    return {
        { QStringLiteral("objects"), objects },
        { QStringLiteral("count"), objects.size() },
        { QStringLiteral("visited"), visited },
        { QStringLiteral("truncated"), truncated },
        { QStringLiteral("pattern"), pattern },
        { QStringLiteral("maxDepth"), maxDepth },
        { QStringLiteral("_loading"), loading }
    };
}

void McpServer::callListObjects(const QJsonValue &id, const QJsonObject &arguments)
{
    if (!ensureReady(id))
        return;
    const QString pattern = arguments.value(QStringLiteral("pattern")).toString();
    if (!pattern.isEmpty()) {
        const QRegularExpression expression(pattern);
        if (!expression.isValid()) {
            sendToolError(id, QStringLiteral("Invalid regular expression: %1").arg(expression.errorString()));
            return;
        }
    }
    const int maxDepth = boundedInteger(arguments, QStringLiteral("maxDepth"), 5, 0, 20);
    const int limit = boundedInteger(arguments, QStringLiteral("limit"), 250, 1, 2000);
    const int timeoutMs = boundedInteger(arguments, QStringLiteral("timeoutMs"), 5000, 250, 30000);
    QAbstractItemModel *model = ObjectBroker::model(QString::fromLatin1(ObjectTreeModelName));
    scheduleStableSnapshot(id, [this, model, pattern, maxDepth, limit]() { return objectSnapshot(model, pattern, maxDepth, limit); }, timeoutMs);
}

QJsonObject McpServer::propertySnapshot(QAbstractItemModel *model, int maxDepth, int limit) const
{
    QJsonArray properties;
    int loading = 0;
    bool truncated = false;

    std::function<void(const QModelIndex &, int, QVector<int>)> visit;
    visit = [&](const QModelIndex &parent, int depth, QVector<int> path) {
        if (truncated || depth > maxDepth)
            return;
        const int rows = model->rowCount(parent);
        const int columns = std::min(model->columnCount(parent), 8);
        for (int row = 0; row < rows; ++row) {
            path.append(row);
            QJsonArray values;
            for (int column = 0; column < columns; ++column) {
                const QModelIndex index = model->index(row, column, parent);
                values.append(index.data(Qt::DisplayRole).toString());
                loading += loadingState(index);
            }
            const auto valueAt = [&values](qsizetype column) {
                return column < values.size() ? values.at(column) : QJsonValue();
            };
            properties.append(QJsonObject {
                { QStringLiteral("path"), indexPath(path) },
                { QStringLiteral("depth"), depth },
                { QStringLiteral("name"), valueAt(0) },
                { QStringLiteral("value"), valueAt(1) },
                { QStringLiteral("type"), valueAt(2) },
                { QStringLiteral("declaredIn"), valueAt(3) },
                { QStringLiteral("columns"), values } });
            if (properties.size() >= limit) {
                truncated = true;
                path.removeLast();
                return;
            }
            if (depth < maxDepth)
                visit(model->index(row, 0, parent), depth + 1, path);
            path.removeLast();
            if (truncated)
                return;
        }
    };
    visit(QModelIndex(), 0, { });

    QJsonArray headers;
    const int columns = std::min(model->columnCount(), 8);
    for (int column = 0; column < columns; ++column)
        headers.append(model->headerData(column, Qt::Horizontal).toString());
    return {
        { QStringLiteral("headers"), headers },
        { QStringLiteral("properties"), properties },
        { QStringLiteral("count"), properties.size() },
        { QStringLiteral("truncated"), truncated },
        { QStringLiteral("_loading"), loading }
    };
}

void McpServer::callGetProperties(const QJsonValue &id, const QJsonObject &arguments)
{
    if (!ensureReady(id))
        return;
    QAbstractItemModel *objectModel = ObjectBroker::model(QString::fromLatin1(ObjectTreeModelName));
    const QString path = arguments.value(QStringLiteral("objectPath")).toString();
    const QModelIndex objectIndex = indexFromPath(objectModel, path);
    if (!objectIndex.isValid()) {
        sendToolError(id, QStringLiteral("Object path is invalid or no longer exists: %1. "
                                         "Refresh it with gammaray_list_objects.")
                              .arg(path));
        return;
    }

    QItemSelectionModel *selection = ObjectBroker::selectionModel(objectModel);
    selection->select(objectIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->setCurrentIndex(objectIndex,
                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    const int maxDepth = boundedInteger(arguments, QStringLiteral("maxDepth"), 3, 0, 10);
    const int limit = boundedInteger(arguments, QStringLiteral("limit"), 300, 1, 2000);
    const int timeoutMs = boundedInteger(arguments, QStringLiteral("timeoutMs"), 5000, 250, 30000);
    QAbstractItemModel *propertyModel = ObjectBroker::model(QString::fromLatin1(PropertyModelName));
    scheduleStableSnapshot(id, [this, propertyModel, path, maxDepth, limit]() {
        QJsonObject snapshot = propertySnapshot(propertyModel, maxDepth, limit);
        snapshot.insert(QStringLiteral("objectPath"), path);
        return snapshot; }, timeoutMs, 500);
}

void McpServer::callGrabWidget(const QJsonValue &id, const QJsonObject &arguments)
{
    callGrabImage(id, arguments, false);
}

void McpServer::callGrabWindow(const QJsonValue &id, const QJsonObject &arguments)
{
    callGrabImage(id, arguments, true);
}

void McpServer::callListQuickItems(const QJsonValue &id, const QJsonObject &)
{
    sendToolError(id, QStringLiteral("Qt Quick item discovery is not available yet."),
                  { { QStringLiteral("model"), QString::fromLatin1(QuickItemModelName) } });
}

void McpServer::callGrabQuickWindow(const QJsonValue &id, const QJsonObject &arguments)
{
    callGrabQuickImage(id, arguments, false);
}

void McpServer::callGrabQuickItem(const QJsonValue &id, const QJsonObject &arguments)
{
    callGrabQuickImage(id, arguments, true);
}

void McpServer::callGrabQuickImage(const QJsonValue &id, const QJsonObject &, bool)
{
    sendToolError(id, QStringLiteral("Qt Quick capture is not available yet."),
                  { { QStringLiteral("remoteView"), QString::fromLatin1(QuickRemoteViewName) } });
}

void McpServer::callGrabImage(const QJsonValue &id, const QJsonObject &arguments,
                              bool captureWindow)
{
    if (!ensureReady(id))
        return;
    if (m_captureRequestActive) {
        sendToolError(id, QStringLiteral("A GUI screenshot request is already in progress. Retry after it finishes."));
        return;
    }

    const QString objectPath = arguments.value(QStringLiteral("objectPath")).toString().trimmed();
    if (objectPath.isEmpty()) {
        sendToolError(id, QStringLiteral("objectPath is required for a GUI screenshot."));
        return;
    }

    struct CaptureState
    {
        QJsonValue requestId;
        QString objectPath;
        ObjectId objectId;
        int maxWidth = 1920;
        int maxHeight = 1080;
        int timeoutMs = 10000;
        bool captureWindow = false;
        bool finished = false;
        QElapsedTimer elapsed;
        QList<QMetaObject::Connection> connections;
    };

    auto state = QSharedPointer<CaptureState>::create();
    state->requestId = id;
    state->objectPath = objectPath;
    state->maxWidth = boundedInteger(arguments, QStringLiteral("maxWidth"), 1920, 1, 8192);
    state->maxHeight = boundedInteger(arguments, QStringLiteral("maxHeight"), 1080, 1, 8192);
    state->timeoutMs = boundedInteger(arguments, QStringLiteral("timeoutMs"), 10000, 250, 30000);
    state->captureWindow = captureWindow;
    state->elapsed.start();
    m_captureRequestActive = true;

    const auto complete = [this, state](const QImage &sourceImage, const QString &error) {
        if (state->finished)
            return;
        state->finished = true;
        for (const QMetaObject::Connection &connection : std::as_const(state->connections))
            QObject::disconnect(connection);
        m_captureRequestActive = false;

        if (!error.isEmpty()) {
            sendToolError(state->requestId, error,
                          { { QStringLiteral("objectPath"), state->objectPath },
                            { QStringLiteral("captureKind"), state->captureWindow ? QStringLiteral("window") : QStringLiteral("widget") } });
            return;
        }
        if (sourceImage.isNull()) {
            sendToolError(state->requestId, QStringLiteral("The Widget Inspector returned an empty image."),
                          { { QStringLiteral("objectPath"), state->objectPath } });
            return;
        }

        QImage outputImage = sourceImage;
        if (outputImage.width() > state->maxWidth || outputImage.height() > state->maxHeight) {
            outputImage = outputImage.scaled(state->maxWidth, state->maxHeight,
                                              Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }

        const QJsonObject metadata {
            { QStringLiteral("captureKind"), state->captureWindow ? QStringLiteral("window") : QStringLiteral("widget") },
            { QStringLiteral("objectPath"), state->objectPath },
            { QStringLiteral("objectId"), QStringLiteral("0x%1").arg(state->objectId.id(), 0, 16) },
            { QStringLiteral("sourceImage"), imageSizeObject(sourceImage) },
            { QStringLiteral("image"), imageSizeObject(outputImage) },
            { QStringLiteral("scaled"), outputImage.size() != sourceImage.size() },
            { QStringLiteral("mimeType"), QStringLiteral("image/png") }
        };
        sendImageToolResult(state->requestId, metadata, outputImage);
    };

    const auto requestImage = QSharedPointer<std::function<void()>>::create();
    *requestImage = [this, state, complete, requestImage]() {
        if (state->finished)
            return;
        const int remainingMs = state->timeoutMs - static_cast<int>(state->elapsed.elapsed());
        if (remainingMs <= 0) {
            complete(QImage(), QStringLiteral("Timed out waiting for a Widget Inspector screenshot."));
            return;
        }

        if (state->captureWindow) {
            auto *remoteView = qobject_cast<RemoteViewInterface *>(
                ObjectBroker::objectInternal(QString::fromLatin1(WidgetRemoteViewName),
                                             QByteArray(qobject_interface_iid<RemoteViewInterface *>())));
            if (!remoteView) {
                QTimer::singleShot(qMin(100, remainingMs), this, *requestImage);
                return;
            }

            state->connections.append(connect(remoteView, &RemoteViewInterface::frameUpdated,
                                              this, [remoteView, complete](const RemoteViewFrame &frame) {
                remoteView->clientViewUpdated();
                remoteView->setViewActive(false);
                if (!frame.isValid()) {
                    complete(QImage(), QStringLiteral("The Widget Inspector returned an invalid window frame."));
                    return;
                }
                complete(frame.image(), QString());
            }));
            remoteView->setViewActive(true);
            remoteView->requestCompleteFrame();
        } else {
            const QByteArray interfaceName(qobject_interface_iid<WidgetInspectorInterface *>());
            auto *inspector = qobject_cast<WidgetInspectorInterface *>(
                ObjectBroker::objectInternal(QString::fromUtf8(interfaceName), interfaceName));
            if (!inspector) {
                QTimer::singleShot(qMin(100, remainingMs), this, *requestImage);
                return;
            }

            state->connections.append(connect(inspector, &WidgetInspectorInterface::widgetScreenshotReceived,
                                              this, [complete](const RemoteViewFrame &frame) {
                if (!frame.isValid()) {
                    complete(QImage(), QStringLiteral("The Widget Inspector returned an invalid widget frame."));
                    return;
                }
                complete(frame.image(), QString());
            }));
            state->connections.append(connect(inspector, &WidgetInspectorInterface::widgetScreenshotFailed,
                                              this, [complete](const QString &message) {
                complete(QImage(), message.isEmpty()
                                     ? QStringLiteral("The Widget Inspector could not capture the selected widget.")
                                     : message);
            }));
            inspector->requestWidgetScreenshot();
        }

        QTimer::singleShot(remainingMs, this, [state, complete]() {
            if (!state->finished)
                complete(QImage(), QStringLiteral("Timed out waiting for the Widget Inspector image response."));
        });
    };

    const auto selectWidget = QSharedPointer<std::function<void()>>::create();
    *selectWidget = [this, state, complete, requestImage, selectWidget]() {
        if (state->finished)
            return;
        const int remainingMs = state->timeoutMs - static_cast<int>(state->elapsed.elapsed());
        if (remainingMs <= 0) {
            complete(QImage(), QStringLiteral("Timed out locating the selected QWidget in the Widget Inspector."));
            return;
        }

        QAbstractItemModel *objectModel = ObjectBroker::model(QString::fromLatin1(ObjectTreeModelName));
        const QModelIndex objectIndex = indexFromPath(objectModel, state->objectPath);
        if (!objectIndex.isValid()) {
            complete(QImage(), QStringLiteral("Object path is invalid or no longer exists: %1. Refresh it with gammaray_list_objects.")
                                .arg(state->objectPath));
            return;
        }
        state->objectId = objectIndex.data(ObjectModel::ObjectIdRole).value<ObjectId>();
        if (state->objectId.isNull()) {
            QTimer::singleShot(qMin(100, remainingMs), this, *selectWidget);
            return;
        }

        QAbstractItemModel *widgetModel = ObjectBroker::model(QString::fromLatin1(WidgetTreeModelName));
        const QModelIndex widgetIndex = indexForObjectId(widgetModel, state->objectId);
        if (!widgetIndex.isValid()) {
            QTimer::singleShot(qMin(100, remainingMs), this, *selectWidget);
            return;
        }

        QItemSelectionModel *selection = ObjectBroker::selectionModel(widgetModel);
        if (!selection) {
            complete(QImage(), QStringLiteral("The Widget Inspector selection model is unavailable."));
            return;
        }
        selection->select(widgetIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        selection->setCurrentIndex(widgetIndex,
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QTimer::singleShot(qMin(250, remainingMs), this, *requestImage);
    };

    (*selectWidget)();
}

QJsonObject McpServer::messageSnapshot(QAbstractItemModel *model, const QString &minimumType,
                                       int limit) const
{
    QJsonArray messages;
    int loading = 0;
    int debug = 0;
    int info = 0;
    int warning = 0;
    int critical = 0;
    int fatal = 0;
    const int totalRows = model->rowCount();
    const int firstScannedRow = qMax(0, totalRows - 5000);
    const int requiredRank = messageRank(minimumType);

    for (int row = firstScannedRow; row < totalRows; ++row) {
        const QModelIndex timeIndex = model->index(row, MessageModelColumn::Time);
        const int type = timeIndex.data(MessageModelRole::Type).toInt();
        switch (static_cast<QtMsgType>(type)) {
        case QtDebugMsg:
            ++debug;
            break;
        case QtInfoMsg:
            ++info;
            break;
        case QtWarningMsg:
            ++warning;
            break;
        case QtCriticalMsg:
            ++critical;
            break;
        case QtFatalMsg:
            ++fatal;
            break;
        }
        loading += loadingState(timeIndex);
        if (messageRank(type) < requiredRank)
            continue;

        const QModelIndex messageIndex = model->index(row, MessageModelColumn::Message);
        const QModelIndex categoryIndex = model->index(row, MessageModelColumn::Category);
        const QModelIndex functionIndex = model->index(row, MessageModelColumn::Function);
        const QModelIndex fileIndex = model->index(row, MessageModelColumn::File);
        loading += loadingState(messageIndex) + loadingState(categoryIndex)
            + loadingState(functionIndex) + loadingState(fileIndex);
        messages.append(QJsonObject {
            { QStringLiteral("row"), row },
            { QStringLiteral("type"), messageTypeName(type) },
            { QStringLiteral("time"), timeIndex.data(Qt::DisplayRole).toString() },
            { QStringLiteral("message"), messageIndex.data(Qt::DisplayRole).toString() },
            { QStringLiteral("category"), categoryIndex.data(Qt::DisplayRole).toString() },
            { QStringLiteral("function"), functionIndex.data(Qt::DisplayRole).toString() },
            { QStringLiteral("file"), fileIndex.data(Qt::DisplayRole).toString() },
            { QStringLiteral("line"), fileIndex.data(MessageModelRole::Line).toInt() } });
        while (messages.size() > limit)
            messages.removeFirst();
    }

    return {
        { QStringLiteral("messages"), messages },
        { QStringLiteral("count"), messages.size() },
        { QStringLiteral("capturedRows"), totalRows },
        { QStringLiteral("scannedRows"), totalRows - firstScannedRow },
        { QStringLiteral("minimumType"), minimumType },
        { QStringLiteral("counts"), countsObject(debug, info, warning, critical, fatal) },
        { QStringLiteral("_loading"), loading }
    };
}

void McpServer::callGetMessages(const QJsonValue &id, const QJsonObject &arguments)
{
    if (!ensureReady(id))
        return;
    const QString minimumType = arguments.value(QStringLiteral("minimumType"))
                                    .toString(QStringLiteral("debug"));
    const int limit = boundedInteger(arguments, QStringLiteral("limit"), 200, 1, 2000);
    const int timeoutMs = boundedInteger(arguments, QStringLiteral("timeoutMs"), 5000, 250, 30000);
    QAbstractItemModel *model = ObjectBroker::model(QString::fromLatin1(MessageModelName));
    scheduleStableSnapshot(id, [this, model, minimumType, limit]() { return messageSnapshot(model, minimumType, limit); }, timeoutMs);
}

QJsonObject McpServer::problemSnapshot(QAbstractItemModel *model) const
{
    QJsonArray problems;
    int loading = 0;
    int info = 0;
    int warning = 0;
    int error = 0;
    const int rows = model->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex descriptionIndex = model->index(row, 0);
        const QModelIndex locationIndex = model->index(row, 1);
        const int severity = descriptionIndex.data(ProblemModelRoles::SeverityRole).toInt();
        if (severity == Problem::Info)
            ++info;
        else if (severity == Problem::Warning)
            ++warning;
        else if (severity == Problem::Error)
            ++error;
        loading += loadingState(descriptionIndex) + loadingState(locationIndex);
        problems.append(QJsonObject {
            { QStringLiteral("severity"), severity == Problem::Error ? QStringLiteral("error") : severity == Problem::Warning ? QStringLiteral("warning")
                                                                                                                              : QStringLiteral("info") },
            { QStringLiteral("id"), descriptionIndex.data(ProblemModelRoles::ProblemIdRole).toString() },
            { QStringLiteral("description"), descriptionIndex.data(Qt::DisplayRole).toString() },
            { QStringLiteral("location"), locationIndex.data(Qt::DisplayRole).toString() } });
    }
    return {
        { QStringLiteral("problems"), problems },
        { QStringLiteral("count"), problems.size() },
        { QStringLiteral("counts"), QJsonObject { { QStringLiteral("info"), info }, { QStringLiteral("warning"), warning }, { QStringLiteral("error"), error } } },
        { QStringLiteral("_loading"), loading }
    };
}

void McpServer::afterProblemScan(const QJsonValue &id, int timeoutMs, SnapshotProducer producer)
{
    const QByteArray interfaceName(qobject_interface_iid<ProblemReporterInterface *>());
    QObject *object = ObjectBroker::objectInternal(QString::fromUtf8(interfaceName), interfaceName);
    auto reporter = qobject_cast<ProblemReporterInterface *>(object);
    if (!reporter) {
        sendToolError(id, QStringLiteral("The target does not expose the GammaRay problem reporter."));
        return;
    }

    auto started = QSharedPointer<bool>::create(false);
    auto startSnapshot = [this, id, timeoutMs, producer, started]() {
        if (*started)
            return;
        *started = true;
        scheduleStableSnapshot(id, producer, timeoutMs, 300);
    };
    connect(reporter, &ProblemReporterInterface::problemScansFinished,
            this, startSnapshot, Qt::SingleShotConnection);
    reporter->requestScan();
    QTimer::singleShot(qMin(2500, timeoutMs / 2), this, startSnapshot);
}

void McpServer::callRunDiagnostics(const QJsonValue &id, const QJsonObject &arguments)
{
    if (!ensureReady(id))
        return;
    const int timeoutMs = boundedInteger(arguments, QStringLiteral("timeoutMs"), 10000, 500, 60000);
    QAbstractItemModel *model = ObjectBroker::model(QString::fromLatin1(ProblemModelName));
    afterProblemScan(id, timeoutMs, [this, model]() {
        return problemSnapshot(model);
    });
}

QJsonObject McpServer::validationSnapshot(const QJsonObject &arguments) const
{
    QAbstractItemModel *problemModel = ObjectBroker::model(QString::fromLatin1(ProblemModelName));
    QAbstractItemModel *messageModel = ObjectBroker::model(QString::fromLatin1(MessageModelName));
    const int messageLimit = boundedInteger(arguments, QStringLiteral("messageLimit"), 1000, 1, 5000);
    QJsonObject problems = problemSnapshot(problemModel);
    QJsonObject messages = messageSnapshot(messageModel, QStringLiteral("debug"), messageLimit);

    const int loading = problems.take(QStringLiteral("_loading")).toInt()
        + messages.take(QStringLiteral("_loading")).toInt();
    const QJsonObject problemCounts = problems.value(QStringLiteral("counts")).toObject();
    const QJsonObject messageCounts = messages.value(QStringLiteral("counts")).toObject();
    const int maximumWarnings = boundedInteger(arguments, QStringLiteral("maxWarningMessages"), 0, 0, 100000);
    const int maximumCriticals = boundedInteger(arguments, QStringLiteral("maxCriticalMessages"), 0, 0, 100000);
    const QString failSeverity = arguments.value(QStringLiteral("failOnProblemSeverity"))
                                     .toString(QStringLiteral("error"));

    QJsonArray failures;
    const int warnings = messageCounts.value(QStringLiteral("warning")).toInt();
    const int criticals = messageCounts.value(QStringLiteral("critical")).toInt()
        + messageCounts.value(QStringLiteral("fatal")).toInt();
    if (warnings > maximumWarnings) {
        failures.append(QStringLiteral("warning messages %1 exceed allowed maximum %2")
                            .arg(warnings)
                            .arg(maximumWarnings));
    }
    if (criticals > maximumCriticals) {
        failures.append(QStringLiteral("critical/fatal messages %1 exceed allowed maximum %2")
                            .arg(criticals)
                            .arg(maximumCriticals));
    }
    if (failSeverity == QLatin1String("error")
        && problemCounts.value(QStringLiteral("error")).toInt() > 0) {
        failures.append(QStringLiteral("GammaRay reported one or more error-severity problems"));
    } else if (failSeverity == QLatin1String("warning")
               && (problemCounts.value(QStringLiteral("warning")).toInt() > 0
                   || problemCounts.value(QStringLiteral("error")).toInt() > 0)) {
        failures.append(QStringLiteral("GammaRay reported one or more warning/error problems"));
    }

    return {
        { QStringLiteral("passed"), failures.isEmpty() },
        { QStringLiteral("failures"), failures },
        { QStringLiteral("criteria"), QJsonObject { { QStringLiteral("maxWarningMessages"), maximumWarnings }, { QStringLiteral("maxCriticalMessages"), maximumCriticals }, { QStringLiteral("failOnProblemSeverity"), failSeverity } } },
        { QStringLiteral("messages"), messages },
        { QStringLiteral("diagnostics"), problems },
        { QStringLiteral("_loading"), loading }
    };
}

void McpServer::callValidateRuntime(const QJsonValue &id, const QJsonObject &arguments)
{
    if (!ensureReady(id))
        return;
    const int timeoutMs = boundedInteger(arguments, QStringLiteral("timeoutMs"), 10000, 500, 60000);
    afterProblemScan(id, timeoutMs, [this, arguments]() {
        return validationSnapshot(arguments);
    });
}

#include "mcpserver.moc"
