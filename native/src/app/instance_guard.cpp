// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/instance_guard.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>

namespace todobench {
InstanceGuard::InstanceGuard(QObject* parent) : QObject(parent) {
    connect(&server_, &QLocalServer::newConnection, this, [this] { handle_connection(); });
}
InstanceOutcome InstanceGuard::start(const QString& state_directory, const QString& workspace) {
    if (!QDir().mkpath(state_directory)) { error_ = "Cannot create instance state directory"; return InstanceOutcome::Failed; }
    const auto canonical = QFileInfo(state_directory).canonicalFilePath();
    const auto name = "TodoBench-" + QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    lock_ = std::make_unique<QLockFile>(state_directory + "/instance.lock");
    lock_->setStaleLockTime(0);
    if (lock_->tryLock()) {
        QLocalServer::removeServer(name);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(name)) { error_ = server_.errorString(); return InstanceOutcome::Failed; }
        primary_pid_ = QCoreApplication::applicationPid();
        return InstanceOutcome::Primary;
    }
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(1000)) { error_ = "Existing instance did not accept activation: " + socket.errorString(); return InstanceOutcome::Failed; }
    socket.write(QJsonDocument(QJsonObject{{"command", "activate"}, {"workspace", workspace}}).toJson(QJsonDocument::Compact) + "\n");
    if (!socket.waitForBytesWritten(1000) || !socket.waitForReadyRead(2000)) {
        error_ = "Existing instance did not acknowledge activation";
        return InstanceOutcome::Failed;
    }
    const auto response = socket.readLine().trimmed();
    if (!response.startsWith("activated:")) { error_ = "Invalid activation response"; return InstanceOutcome::Failed; }
    primary_pid_ = response.mid(10).toLongLong();
    if (primary_pid_ <= 0) { error_ = "Invalid primary process ID"; return InstanceOutcome::Failed; }
    return InstanceOutcome::Activated;
}
void InstanceGuard::on_activation(std::function<void(const QString&)> callback) { callback_ = std::move(callback); }
void InstanceGuard::handle_connection() {
    auto* socket = server_.nextPendingConnection();
    if (!socket) return;
    auto respond = [this, socket] {
        if (!socket->canReadLine()) return;
        const auto request = QJsonDocument::fromJson(socket->readLine()).object();
        if (request.value("command") == "activate" && callback_) {
            callback_(request.value("workspace").toString());
            // Queued acknowledgement proves the primary event loop remains responsive.
            QTimer::singleShot(0, socket, [socket] {
                socket->write("activated:" + QByteArray::number(QCoreApplication::applicationPid()) + "\n");
                socket->disconnectFromServer();
            });
        } else socket->disconnectFromServer();
    };
    connect(socket, &QLocalSocket::readyRead, socket, respond);
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    QTimer::singleShot(3000, socket, [socket] { socket->disconnectFromServer(); socket->deleteLater(); });
    respond();
}
} // namespace todobench
