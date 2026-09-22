// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <functional>
#include <memory>
namespace todobench {
enum class InstanceOutcome { Primary, Activated, Failed };
class InstanceGuard final : public QObject {
    Q_OBJECT
public:
    explicit InstanceGuard(QObject* parent = nullptr);
    InstanceOutcome start(const QString& state_directory, const QString& workspace = {});
    const QString& error() const { return error_; }
    qint64 primary_pid() const { return primary_pid_; }
    void on_activation(std::function<void(const QString&)> callback);
private:
    void handle_connection();
    QLocalServer server_;
    std::unique_ptr<QLockFile> lock_;
    std::function<void(const QString&)> callback_;
    QString error_;
    qint64 primary_pid_{0};
};
} // namespace todobench
