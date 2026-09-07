// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/instance_guard.h"

#include <QLocalSocket>

namespace todobench {
namespace {

constexpr auto kServerName = "TodoBenchPrimaryInstance";

}  // namespace

InstanceGuard::InstanceGuard(QObject* parent) : QObject(parent) {
    connect(&server_, &QLocalServer::newConnection, this, [this] { handle_connection(); });
}

bool InstanceGuard::become_primary() {
    QLocalSocket socket;
    socket.connectToServer(kServerName);
    if (socket.waitForConnected(200)) {
        socket.write("activate\n");
        socket.waitForBytesWritten(200);
        socket.waitForDisconnected(200);
        return false;
    }
    QLocalServer::removeServer(kServerName);
    return server_.listen(kServerName);
}

void InstanceGuard::on_activation(std::function<void()> callback) { callback_ = std::move(callback); }

void InstanceGuard::handle_connection() {
    auto* socket = server_.nextPendingConnection();
    if (socket == nullptr) return;
    socket->waitForReadyRead(200);
    socket->deleteLater();
    if (callback_) callback_();
}

}  // namespace todobench
