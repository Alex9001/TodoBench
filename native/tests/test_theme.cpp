// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/theme.h"
#include "app/theme_editor.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QStyle>

using namespace todobench;

class ThemeTest final : public QObject {
    Q_OBJECT
private slots:
    void presetContrast_data();
    void presetContrast();
    void lightIsIndependentOfDesktop();
    void systemRestoresOriginalAppearance();
    void overridesPersistPerPreset();
    void invalidOverridesAreIgnored();
    void customizerPreviewsAndResets();
    void customizerKeepsPresetsSeparate();
};

void ThemeTest::presetContrast_data() {
    QTest::addColumn<QString>("theme");
    for (const auto& preset : theme_presets()) {
        if (preset.id != "system") QTest::newRow(preset.id.c_str()) << QString::fromStdString(preset.id);
    }
}

void ThemeTest::presetContrast() {
    QFETCH(QString, theme);
    const auto palette = theme_palette(theme.toStdString(), {}, system_theme_palette());
    QVERIFY(color_contrast(palette.color(QPalette::Text), palette.color(QPalette::Base)) >= 4.5);
    QVERIFY(color_contrast(palette.color(QPalette::WindowText), palette.color(QPalette::Window)) >= 4.5);
    QVERIFY(color_contrast(palette.color(QPalette::ButtonText), palette.color(QPalette::Button)) >= 4.5);
    QVERIFY(color_contrast(palette.color(QPalette::HighlightedText), palette.color(QPalette::Highlight)) >= 4.5);
    QVERIFY(color_contrast(palette.color(QPalette::ToolTipText), palette.color(QPalette::ToolTipBase)) >= 4.5);
}

void ThemeTest::lightIsIndependentOfDesktop() {
    QPalette dark_desktop;
    dark_desktop.setColor(QPalette::Window, Qt::black);
    dark_desktop.setColor(QPalette::Base, Qt::black);
    const auto light = theme_palette("light", {}, dark_desktop);
    QCOMPARE(light.color(QPalette::Base), QColor("#ffffff"));
    QVERIFY(light.color(QPalette::Window).lightness() > 230);
    Settings settings;
    settings.theme = "midnight";
    apply_theme(settings);
    settings.theme = "light";
    apply_theme(settings);
    QCOMPARE(QApplication::palette().color(QPalette::Base), light.color(QPalette::Base));
}

void ThemeTest::systemRestoresOriginalAppearance() {
    const auto original = system_theme_palette();
    Settings settings;
    settings.theme = "blue";
    apply_theme(settings);
    settings.theme = "system";
    apply_theme(settings);
    QCOMPARE(QApplication::palette().color(QPalette::Base), original.color(QPalette::Base));
    QCOMPARE(QApplication::palette().color(QPalette::Window), original.color(QPalette::Window));
}

void ThemeTest::overridesPersistPerPreset() {
    QTemporaryDir temporary;
    const auto path = std::filesystem::path(temporary.path().toStdString()) / "settings.json";
    Settings settings;
    settings.theme = "green";
    settings.theme_overrides = {{"green", {{"surface", "#102030"}, {"text", "#abcdef"}}},
                                {"light", {{"accent", "#aabbcc"}}}};
    std::string error;
    QVERIFY(save_settings(path, settings, error));
    const auto loaded = load_settings(path);
    QVERIFY(std::holds_alternative<Settings>(loaded));
    const auto restored = std::get<Settings>(loaded);
    QCOMPARE(restored.theme, settings.theme);
    QVERIFY(restored.theme_overrides == settings.theme_overrides);
    QCOMPARE(theme_palette(restored.theme, theme_overrides(restored), system_theme_palette()).color(QPalette::Text), QColor("#abcdef"));
}

void ThemeTest::invalidOverridesAreIgnored() {
    const auto base = theme_palette("light", {}, system_theme_palette());
    const auto invalid = theme_palette("light", {{"surface", "garbage"}, {"text", "#00000000"}}, system_theme_palette());
    QCOMPARE(invalid.color(QPalette::Base), base.color(QPalette::Base));
    QCOMPARE(invalid.color(QPalette::Text), base.color(QPalette::Text));
}

void ThemeTest::customizerPreviewsAndResets() {
    Settings working;
    working.theme = "light";
    const auto application_palette = QApplication::palette();
    ThemeEditor editor(working);
    auto* field = editor.findChild<QLineEdit*>("themeColor_surface");
    field->setText("#112233");
    QCOMPARE(editor.findChild<QWidget*>("themePreview")->palette().color(QPalette::Base), QColor("#112233"));
    QCOMPARE(QApplication::palette(), application_palette);
    field->setText("invalid");
    QVERIFY(!editor.valid());
    editor.findChild<QPushButton*>("resetThemeColors")->click();
    QVERIFY(editor.valid());
    QVERIFY(theme_overrides(working).empty());
}

void ThemeTest::customizerKeepsPresetsSeparate() {
    Settings working;
    working.theme = "light";
    ThemeEditor editor(working);
    auto* presets = editor.findChild<QComboBox*>("themePreset");
    auto* field = editor.findChild<QLineEdit*>("themeColor_accent");
    field->setText("#123456");
    presets->setCurrentIndex(presets->findData("green"));
    QVERIFY(field->text().isEmpty());
    field->setText("#abcdef");
    presets->setCurrentIndex(presets->findData("light"));
    QCOMPARE(field->text(), QString("#123456"));
    QCOMPARE(working.theme_overrides.at("green").at("accent"), std::string("#abcdef"));
}

QTEST_MAIN(ThemeTest)
#include "test_theme.moc"
