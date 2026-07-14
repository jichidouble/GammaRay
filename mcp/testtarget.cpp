/*
  testtarget.cpp

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

class MonitoredWorker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int counter READ counter NOTIFY counterChanged)
    Q_PROPERTY(QString state READ state WRITE setState NOTIFY stateChanged)
public:
    explicit MonitoredWorker(QObject *parent = nullptr)
        : QObject(parent)
    {
        setObjectName(QStringLiteral("runtimeWorker"));
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            ++m_counter;
            emit counterChanged();
            qInfo() << "GammaRay MCP test heartbeat" << m_counter;
        });
        m_timer.start(250);
    }

    int counter() const
    {
        return m_counter;
    }
    QString state() const
    {
        return m_state;
    }
    void setState(const QString &state)
    {
        if (m_state == state)
            return;
        m_state = state;
        emit stateChanged();
    }

signals:
    void counterChanged();
    void stateChanged();

private:
    QTimer m_timer;
    int m_counter = 0;
    QString m_state = QStringLiteral("running");
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setObjectName(QStringLiteral("gammarayMcpTestApplication"));

    QObject service(&app);
    service.setObjectName(QStringLiteral("testService"));
    MonitoredWorker worker(&service);
    QObject child(&worker);
    child.setObjectName(QStringLiteral("leafObject"));

    QTimer::singleShot(750, []() {
        qWarning() << "GammaRay MCP deterministic test warning";
    });
    const int durationMs = app.arguments().value(1, QStringLiteral("30000")).toInt();
    QTimer::singleShot(qMax(durationMs, 1000), &app, &QCoreApplication::quit);
    return app.exec();
}

#include "testtarget.moc"
