// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/icons.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QSvgRenderer>
#include <QVariant>

static void initialize_icons() { Q_INIT_RESOURCE(icons); }

namespace todobench {
namespace {
class PaletteIcon final : public QIconEngine {
public:
    explicit PaletteIcon(QString name) : name_(std::move(name)) {}
    QIconEngine* clone() const override { return new PaletteIcon(name_); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const auto palette = QApplication::palette();
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        const auto role = mode == QIcon::Selected || state == QIcon::On ? QPalette::HighlightedText : QPalette::WindowText;
        QFile file(":/icons/lucide/" + name_ + ".svg");
        if (!file.open(QIODevice::ReadOnly)) return;
        auto svg = file.readAll();
        svg.replace("currentColor", palette.color(group, role).name().toUtf8());
        QSvgRenderer renderer(svg);
        renderer.render(painter, QRectF(rect));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
private:
    QString name_;
};
}
QIcon lucide_icon(const QString& name) {
    initialize_icons();
    return QIcon(new PaletteIcon(name));
}
QIcon launcher_icon() {
    initialize_icons();
    return QIcon(":/icons/todobench.svg");
}
void set_action_icon(QAction* action, const QString& name) {
    action->setProperty("lucideName", name);
    action->setIcon(lucide_icon(name));
}
void refresh_action_icons(QObject* root) {
    for (auto* action : root->findChildren<QAction*>()) {
        const auto name = action->property("lucideName").toString();
        if (!name.isEmpty()) action->setIcon(lucide_icon(name));
    }
}
}
