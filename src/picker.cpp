/* -*- Mode: C++; indent-tabs-mode: nil; tab-width: 4 -*-
 * -*- coding: utf-8 -*-
 *
 * Copyright (C) 2011 ~ 2018 Deepin, Inc.
 *               2011 ~ 2018 Wang Yong
 *
 * Author:     Wang Yong <wangyong@deepin.com>
 * Maintainer: Wang Yong <wangyong@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "picker.h"
#include "animation.h"
#include "settings.h"
#include "utils.h"
#include "colormenu.h"
#include <QPainter>
#include <QBitmap>
#include <QPixmap>
#include <QMenu>
#include <QAction>
#include <QClipboard>
#include <QStyleFactory>
#include <QScreen>
#include <QApplication>
// #include <QDesktopWidget>
#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QWindow>

Picker::Picker(bool launchByDBus)
{
    // Init app id.
    isLaunchByDBus = launchByDBus;
    isWayland = QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
    
    // Init window flags.
    Qt::WindowFlags flags = Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint | Qt::Tool;
    if (!isWayland) {
        flags |= Qt::X11BypassWindowManagerHint;
    }
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Init attributes.
    blockHeight = 20;
    blockWidth = 20;
    displayCursorDot = false;
    height = 220;
    screenshotSize = 11;
    width = 220;
    windowHeight = 236;
    windowWidth = 236;

    shadowPixmap = QPixmap(Utils::getQrcPath("shadow.png"));

    // Init update screenshot timer.
    updateScreenshotTimer = new QTimer(this);
    updateScreenshotTimer->setSingleShot(true);
    connect(updateScreenshotTimer, SIGNAL(timeout()), this, SLOT(updateScreenshot()));

    QScreen *screen = QApplication::primaryScreen();
    if (screen) {
        resize(screen->geometry().size());
        move(screen->geometry().topLeft());
        cursorX = screen->geometry().center().x();
        cursorY = screen->geometry().center().y();
    }
}

Picker::~Picker()
{
    restoreSystemCursor();
    delete animation;
    delete menu;
}

void Picker::paintEvent(QPaintEvent *)
{
    if (!isWayland || magnifierPixmap.isNull()) {
        return;
    }

    const QPointF localPosition = mapFromGlobal(QPoint(cursorX, cursorY));
    const QSizeF magnifierSize = magnifierPixmap.deviceIndependentSize();
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawPixmap(localPosition - QPointF(magnifierSize.width() / 2,
                                                magnifierSize.height() / 2),
                       magnifierPixmap);
}

void Picker::handleMouseMove(int x, int y)
{
    cursorX = x;
    cursorY = y;

    if (updateScreenshotTimer->isActive()) {
        updateScreenshotTimer->stop();
    }
    updateScreenshotTimer->start(5);
}

void Picker::updateScreenshot()
{
    if (!displayCursorDot && isVisible()) {
        QScreen *screen = QApplication::primaryScreen();
        if (!screen || screenPixmap.isNull()) {
            return;
        }

        // Need add offset to make drop shadow's position correctly.
        int offsetX = (windowWidth - width) / 2;
        int offsetY = (windowHeight - height) / 2;

        // Get image under cursor.
        const qreal devicePixelRatio = screen->devicePixelRatio();
        const QRect screenGeometry = screen->geometry();
        const qreal scaleX = qreal(screenPixmap.width()) / screenGeometry.width();
        const qreal scaleY = qreal(screenPixmap.height()) / screenGeometry.height();
        const int pixelX = qRound((cursorX - screenGeometry.left()) * scaleX);
        const int pixelY = qRound((cursorY - screenGeometry.top()) * scaleY);
        
        const int sourceWidth = screenshotSize;
        const int sourceHeight = screenshotSize;
        const QRect source(pixelX - sourceWidth / 2, pixelY - sourceHeight / 2,
                           sourceWidth, sourceHeight);
        screenshotPixmap = QPixmap::fromImage(screenPixmap.toImage().copy(source));
        screenshotPixmap = screenshotPixmap.scaled(qRound(width * devicePixelRatio),
                                                   qRound(height * devicePixelRatio),
                                                   Qt::KeepAspectRatio,
                                                   Qt::FastTransformation);
        screenshotPixmap.setDevicePixelRatio(devicePixelRatio);

        QPixmap cursorPixmap = shadowPixmap.scaled(
            qRound(windowWidth * devicePixelRatio),
            qRound(windowHeight * devicePixelRatio),
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);
        cursorPixmap.setDevicePixelRatio(devicePixelRatio);
        QPainter painter(&cursorPixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.save();
        QPainterPath circlePath;
        circlePath.addEllipse(2 + offsetX, 2 + offsetY, width - 4, height - 4);
        painter.setClipPath(circlePath);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawPixmap(1 + offsetX, 1 + offsetY, screenshotPixmap);
        painter.restore();

        // Draw circle bound.
        int outsidePenWidth = 1;
        QPen outsidePen("#000000");
        outsidePen.setWidth(outsidePenWidth);
        painter.setOpacity(0.05);
        painter.setPen(outsidePen);
        painter.drawEllipse(1 + offsetX, 1 + offsetY, width - 2, height - 2);

        int insidePenWidth = 4;
        QPen insidePen("#ffffff");
        insidePen.setWidth(insidePenWidth);
        painter.setOpacity(0.5);
        painter.setPen(insidePen);
        painter.drawEllipse(3 + offsetX, 3 + offsetY, width - 6, height - 6);

        // Draw focus block.
        painter.setOpacity(1);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setOpacity(0.2);
        painter.setPen("#000000");
        painter.drawRect(QRect(width / 2 - blockWidth / 2 + offsetX, height / 2 - blockHeight / 2 + offsetY, blockWidth, blockHeight));
        painter.setOpacity(1);
        painter.setPen("#ffffff");
        painter.drawRect(QRect(width / 2 - blockWidth / 2 + 1 + offsetX, height / 2 - blockHeight / 2 + 1 + offsetY, blockWidth - 2, blockHeight - 2));

        if (isWayland) {
            magnifierPixmap = cursorPixmap;
            hideSystemCursor();
            update();
        } else {
            // A widget-local cursor avoids leaking one application override
            // cursor per motion event.
            setCursor(QCursor(cursorPixmap));
        }
    }
}

void Picker::handleLeftButtonPress(int x, int y)
{
    if (!displayCursorDot && isVisible()) {
        // Rest cursor and hide window.
        // NOTE: Don't call hide() at here, let process die,
        // Otherwise mouse event will pass to application window under picker.
        restoreSystemCursor();
        unsetCursor();

        // Rest color type to hex if config file not exist.
        Settings settings;

        // Emit copyColor signal to copy color to system clipboard.
        cursorColor = getColorAtCursor(x, y);
        copyColor(cursorColor, settings.getOption("color_type", "HEX").toString());
        
        // Send colorPicked signal when call by DBus and no empty appid.
        if (isLaunchByDBus && appid != "") {
            colorPicked(appid, Utils::colorToHex(cursorColor));
        }
    }
}

void Picker::handleRightButtonRelease(int x, int y)
{
    if (!displayCursorDot && isVisible()) {
        // Set displayCursorDot flag when click right button.
        displayCursorDot = true;

        cursorColor = getColorAtCursor(x, y);

        // Popup color menu window.
        menu = new ColorMenu(
            x - blockWidth / 2,
            y - blockHeight / 2,
            blockWidth,
            cursorColor,
            this);
        connect(menu, &ColorMenu::copyColor, this, &Picker::copyColor, Qt::QueuedConnection);
        connect(menu, &ColorMenu::exit, this, &Picker::exit, Qt::QueuedConnection);
        menu->show();
        menu->setFocus();       // set focus to monitor 'aboutToHide' signal of color menu

        if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
            // Wayland does not allow arbitrary positioning of normal toplevel
            // windows. ColorMenu is a transient popup here, and the X11-only
            // free-floating animation is skipped.
            restoreSystemCursor();
            unsetCursor();
            menu->showMenu();
            return;
        }

        // Display animation before poup color menu.
        animation = new Animation(x, y, screenshotPixmap, cursorColor);
        connect(animation, &Animation::finish, this, &Picker::popupColorMenu, Qt::QueuedConnection);

        // Rest cursor to default cursor.
        restoreSystemCursor();
        unsetCursor();

        // Show animation after rest cursor to avoid flash screen.
        animation->show();
    }
}

QColor Picker::getColorAtCursor(int x, int y)
{
    QScreen *screen = QApplication::primaryScreen();
    if (!screen || screenPixmap.isNull()) {
        return QColor();
    }

    const QRect geometry = screen->geometry();
    const qreal scaleX = qreal(screenPixmap.width()) / geometry.width();
    const qreal scaleY = qreal(screenPixmap.height()) / geometry.height();
    const int pixelX = qBound(0, qRound((x - geometry.left()) * scaleX),
                              screenPixmap.width() - 1);
    const int pixelY = qBound(0, qRound((y - geometry.top()) * scaleY),
                              screenPixmap.height() - 1);
    return QColor(screenPixmap.toImage().pixel(pixelX, pixelY));
}

void Picker::popupColorMenu()
{
    // Hide picker main window and popup color menu.
    // NOTE: Don't call hide() at here, let process die,
    // Otherwise mouse event will pass to application window under picker.
    menu->showMenu();
}

void Picker::StartPick(QString id)
{
    // Update app id.
    appid = id;
    displayCursorDot = false;
    
    QScreen *screen = QApplication::primaryScreen();
    if (!screen) {
        emit exit();
        return;
    }

    // Pin the future Wayland surface to the same output that is captured.
    winId();
    if (windowHandle()) {
        windowHandle()->setScreen(screen);
    }

    // Capture before showing the picker so the picker surface is excluded.
    QImage image;
    if (isWayland && waylandCapture.captureOutput(screen, &image)) {
        screenPixmap = QPixmap::fromImage(image);
        screenPixmap.setDevicePixelRatio(screen->devicePixelRatio());
    } else {
        screenPixmap = screen->grabWindow(0);
    }
    if (screenPixmap.isNull()) {
        qWarning() << "Unable to capture the screen";
        emit exit();
        return;
    }

    resize(screen->geometry().size());
    move(screen->geometry().topLeft());
    if (isWayland) {
        showFullScreen();
    } else {
        show();
    }
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    if (isWayland) {
        hideSystemCursor();
        qDebug() << "Picker surface geometry" << geometry() << frameGeometry()
                 << "origin" << mapToGlobal(QPoint(0, 0))
                 << "screen" << screen->geometry()
                 << "available" << screen->availableGeometry();
    }

    // Update screenshot when start.
    updateScreenshot();
}

void Picker::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint point = mapToGlobal(event->position().toPoint());
    handleMouseMove(point.x(), point.y());
    event->accept();
}

void Picker::mousePressEvent(QMouseEvent *event)
{
    const QPoint point = mapToGlobal(event->position().toPoint());
    if (event->button() == Qt::LeftButton) {
        handleLeftButtonPress(point.x(), point.y());
    }
    event->accept();
}

void Picker::mouseReleaseEvent(QMouseEvent *event)
{
    const QPoint point = mapToGlobal(event->position().toPoint());
    if (event->button() == Qt::RightButton) {
        handleRightButtonRelease(point.x(), point.y());
    }
    event->accept();
}

void Picker::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        emit exit();
    }
    event->accept();
}

void Picker::enterEvent(QEnterEvent *event)
{
    if (isWayland) {
        hideSystemCursor();
        const QPoint point = mapToGlobal(event->position().toPoint());
        handleMouseMove(point.x(), point.y());
    }
    QWidget::enterEvent(event);
}

void Picker::hideSystemCursor()
{
    if (isWayland && !hasCursorOverride) {
        QApplication::setOverrideCursor(Qt::BlankCursor);
        hasCursorOverride = true;
    }
}

void Picker::restoreSystemCursor()
{
    if (hasCursorOverride) {
        QApplication::restoreOverrideCursor();
        hasCursorOverride = false;
    }
}
