#include "user_select_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

UserSelectDialog::UserSelectDialog(const std::vector<UserListEntry>& users,
                                     QWidget* parent)
    : QDialog(parent), users_(users) {
    setWindowTitle("选择目标用户");
    setMinimumWidth(350);
    createUI();
}

void UserSelectDialog::createUI() {
    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* title = new QLabel("请选择要连接的远程桌面用户：");
    layout->addWidget(title);

    userList_ = new QListWidget();
    for (const auto& u : users_) {
        QString text = QString::fromStdString(u.name)
                       + (u.hasActiveSession ? "  [已登录]" : "  [未登录]");
        userList_->addItem(text);
    }
    connect(userList_, &QListWidget::currentRowChanged,
            this, &UserSelectDialog::onSelectionChanged);
    layout->addWidget(userList_);

    lblStatus_ = new QLabel("");
    layout->addWidget(lblStatus_);

    password_ = new QLineEdit();
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText("Windows 密码（未登录用户需要）");
    password_->setVisible(false);
    layout->addWidget(password_);

    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnConfirm_ = new QPushButton("连接");
    btnConfirm_->setMinimumHeight(32);
    btnConfirm_->setEnabled(false);
    connect(btnConfirm_, &QPushButton::clicked, this, &UserSelectDialog::onConfirm);
    btnLayout->addWidget(btnConfirm_);

    QPushButton* btnCancel = new QPushButton("取消");
    btnCancel->setMinimumHeight(32);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(btnCancel);

    layout->addLayout(btnLayout);
}

void UserSelectDialog::onSelectionChanged() {
    int row = userList_->currentRow();
    if (row < 0 || row >= (int)users_.size()) {
        btnConfirm_->setEnabled(false);
        password_->setVisible(false);
        return;
    }

    btnConfirm_->setEnabled(true);
    const auto& u = users_[row];
    selectedUser_ = u.name;

    if (u.hasActiveSession) {
        lblStatus_->setText(QString::fromStdString(u.name) + " 已登录，无需密码");
        password_->setVisible(false);
    } else {
        lblStatus_->setText(QString::fromStdString(u.name) + " 未登录，请输入密码");
        password_->setVisible(true);
        password_->setFocus();
    }
}

void UserSelectDialog::onConfirm() {
    int row = userList_->currentRow();
    if (row < 0 || row >= (int)users_.size())
        return;

    const auto& u = users_[row];
    if (!u.hasActiveSession && password_->text().isEmpty()) {
        QMessageBox::warning(this, "密码为空",
            "未登录用户需要输入 Windows 密码");
        password_->setFocus();
        return;
    }

    accept();
}