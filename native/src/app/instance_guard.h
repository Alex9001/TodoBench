// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLocalServer>
#include <QObject>

#include <functional>

namespace todobench {

class InstanceGuard final : public QObject {
    Q_OBJECT
public:
    explicit InstanceGuard(QObject* parent = nullptr);
    bool become_primary();
    void on_activation(std::function<void()> callback);

private:
    void handle_connection();

    QLocalServer server_;
    std::function<void()> callback_;
};

}  // namespace todobench
