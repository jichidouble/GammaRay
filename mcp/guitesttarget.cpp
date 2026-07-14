/*
  guitesttarget.cpp

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setObjectName(QStringLiteral("gammarayMcpGuiTestApplication"));

    QMainWindow window;
    window.setObjectName(QStringLiteral("gammarayMcpGuiTestWindow"));
    window.setWindowTitle(QStringLiteral("GammaRay MCP GUI Capture Test"));
    window.resize(720, 420);

    auto *dashboard = new QFrame(&window);
    dashboard->setObjectName(QStringLiteral("runtimeDashboard"));
    dashboard->setFrameShape(QFrame::StyledPanel);
    dashboard->setStyleSheet(QStringLiteral("QFrame { background: #1f2937; color: white; }"));
    auto *layout = new QVBoxLayout(dashboard);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("Qt runtime GUI inspection"), dashboard);
    title->setObjectName(QStringLiteral("dashboardTitle"));
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 700; color: #f9fafb;"));
    layout->addWidget(title);

    auto *status = new QLabel(dashboard);
    status->setObjectName(QStringLiteral("runtimeStatusLabel"));
    status->setMinimumHeight(74);
    status->setAlignment(Qt::AlignCenter);
    status->setStyleSheet(
        QStringLiteral("QLabel { background: #065f46; border: 2px solid #34d399; "
                       "border-radius: 8px; font-size: 20px; color: white; }"));
    layout->addWidget(status);

    auto *button = new QPushButton(QStringLiteral("Refresh runtime state"), dashboard);
    button->setObjectName(QStringLiteral("screenshotButton"));
    button->setMinimumHeight(48);
    button->setStyleSheet(
        QStringLiteral("QPushButton { background: #2563eb; color: white; border-radius: 6px; "
                       "font-size: 16px; font-weight: 600; }"));
    layout->addWidget(button);
    layout->addStretch();

    int tick = 0;
    const auto refreshStatus = [&status, &tick]() {
        ++tick;
        status->setText(QStringLiteral("Healthy — sample %1 — %2")
                            .arg(tick)
                            .arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    };
    refreshStatus();
    QObject::connect(button, &QPushButton::clicked, &window, refreshStatus);
    auto *timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, refreshStatus);
    timer->start(250);

    window.setCentralWidget(dashboard);
    window.show();

    const int durationMs = app.arguments().value(1, QStringLiteral("120000")).toInt();
    QTimer::singleShot(qMax(durationMs, 1000), &app, &QCoreApplication::quit);
    return app.exec();
}
