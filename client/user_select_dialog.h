#ifndef USER_SELECT_DIALOG_H
#define USER_SELECT_DIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <string>
#include <vector>
#include "../common/direct_desktop_protocol.h"

class UserSelectDialog : public QDialog {
    Q_OBJECT
public:
    explicit UserSelectDialog(const std::vector<UserListEntry>& users,
                               QWidget* parent = nullptr);

    std::string selectedUser() const { return selectedUser_; }
    std::string password() const { return password_->text().toStdString(); }

private slots:
    void onSelectionChanged();
    void onConfirm();

private:
    void createUI();

    QListWidget* userList_;
    QLineEdit* password_;
    QLabel* lblStatus_;
    QPushButton* btnConfirm_;
    std::vector<UserListEntry> users_;
    std::string selectedUser_;
};

#endif