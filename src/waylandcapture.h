/*
 * Copyright (C) 2026 CharOfString <root@charofstring.cc>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef WAYLANDCAPTURE_H
#define WAYLANDCAPTURE_H

#include <QImage>
#include <QList>
#include <QPoint>
#include <QString>

class QScreen;
struct kywc_capture_manager_v1;
struct kywc_output_manager_v1;
struct kywc_output_v1;
struct wl_display;
struct wl_registry;

class WaylandCapture
{
public:
    WaylandCapture();
    ~WaylandCapture();

    bool available();
    bool captureOutput(QScreen *screen, QImage *image);

    struct Output {
        QString uuid;
        QString name;
        QPoint position;
        kywc_output_v1 *proxy = nullptr;
    };

    // Wayland protocol callbacks. They are public because the generated
    // listener tables are defined in the implementation translation unit.
    static void registryGlobal(void *data, wl_registry *registry, unsigned int name,
                               const char *interface, unsigned int version);
    static void registryGlobalRemove(void *data, wl_registry *registry, unsigned int name);
    static void outputManagerOutput(void *data, kywc_output_manager_v1 *manager,
                                    kywc_output_v1 *output, const char *uuid);
    static void outputManagerPrimary(void *data, kywc_output_manager_v1 *manager,
                                     kywc_output_v1 *output);
    static void outputManagerDone(void *data, kywc_output_manager_v1 *manager);
    static void outputManagerFinished(void *data, kywc_output_manager_v1 *manager);

private:
    void initialize();
    QString outputUuid(QScreen *screen) const;
    void destroyObjects();

    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    kywc_capture_manager_v1 *m_captureManager = nullptr;
    kywc_output_manager_v1 *m_outputManager = nullptr;
    QList<Output *> m_outputs;
    Output *m_primaryOutput = nullptr;
    bool m_initialized = false;
    bool m_outputsReady = false;
};

#endif
