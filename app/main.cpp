#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>

#include "../common/protocol.h"
#include "../client/service_manager_dialog.h"
#include "client_application.h"
#include "server_application.h"

#pragma comment(lib, "Wtsapi32.lib")

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    if (!NetUtil::InitWinsock()) {
        QMessageBox::critical(nullptr, "Error", "WSAStartup failed!");
        return 1;
    }

    QDialog startupDlg;
    startupDlg.setWindowTitle("JC Tool");
    startupDlg.setFixedSize(300, 200);
    QVBoxLayout layout(&startupDlg);

    QLabel* label = new QLabel("Select application mode:");
    label->setAlignment(Qt::AlignCenter);
    layout.addWidget(label);

    QPushButton* btnClient = new QPushButton("Start as Client");
    QPushButton* btnServer = new QPushButton("Start as Server");
    QPushButton* btnServiceMgr = new QPushButton("Service Manager...");
    btnClient->setMinimumHeight(35);
    btnServer->setMinimumHeight(35);
    btnServiceMgr->setMinimumHeight(35);
    layout.addWidget(btnClient);
    layout.addWidget(btnServer);
    layout.addSpacing(4);
    layout.addWidget(btnServiceMgr);

    int mode = 0;
    QObject::connect(btnClient, &QPushButton::clicked, [&](){ mode = 1; startupDlg.accept(); });
    QObject::connect(btnServer, &QPushButton::clicked, [&](){ mode = 2; startupDlg.accept(); });
    QObject::connect(btnServiceMgr, &QPushButton::clicked, [&]() {
        ServiceManagerDialog dlg;
        dlg.exec();
    });

    if (startupDlg.exec() != QDialog::Accepted) {
        WSACleanup();
        return 0;
    }

    int result = 0;
    if (mode == 1) {
        ClientApplication clientApp;
        result = clientApp.exec();
    } else if (mode == 2) {
        ServerApplication serverApp;
        result = serverApp.exec();
    }

    WSACleanup();
    return result;
}
