#include "main_window.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("LW Signal Capture");
    QApplication::setOrganizationName("LuoWave");
    QApplication::setApplicationVersion("1.1");

    // Fusion + an explicit dark palette also styles Linux popup windows that
    // are not always covered by a parent widget's stylesheet.
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(17, 23, 34));
    palette.setColor(QPalette::WindowText, QColor(224, 232, 242));
    palette.setColor(QPalette::Base, QColor(31, 41, 55));
    palette.setColor(QPalette::AlternateBase, QColor(37, 48, 64));
    palette.setColor(QPalette::Text, QColor(237, 243, 251));
    palette.setColor(QPalette::Button, QColor(38, 50, 69));
    palette.setColor(QPalette::ButtonText, QColor(237, 243, 251));
    palette.setColor(QPalette::Highlight, QColor(37, 103, 154));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::ToolTipBase, QColor(28, 37, 50));
    palette.setColor(QPalette::ToolTipText, QColor(237, 243, 251));
    palette.setColor(QPalette::PlaceholderText, QColor(117, 132, 151));
    app.setPalette(palette);

    MainWindow window;
    window.showMaximized();
    return app.exec();
}
