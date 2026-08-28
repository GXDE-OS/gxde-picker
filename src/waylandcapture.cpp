/*
 * Copyright (C) 2026 CharOfString <root@charofstring.cc>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "waylandcapture.h"

#include "protocols/kywc-capture-v1-client-protocol.h"
#include "protocols/kywc-output-v1-client-protocol.h"

#include <QGuiApplication>
#include <QElapsedTimer>
#include <QList>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScreen>
#include <qpa/qplatformnativeinterface.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace {

// DRM fourcc values used by wlcom. Keeping these local avoids a libdrm
// dependency just for decoding the shared-memory capture buffer.
constexpr uint32_t DrmFormatXrgb8888 = 0x34325258;
constexpr uint32_t DrmFormatArgb8888 = 0x34325241;
constexpr uint32_t DrmFormatXbgr8888 = 0x34324258;
constexpr uint32_t DrmFormatAbgr8888 = 0x34324241;
constexpr uint64_t DrmModifierLinear = 0;
constexpr uint64_t DrmModifierInvalid = (1ULL << 56) - 1;

struct CaptureFrame {
    struct Plane {
        int fd = -1;
        uint32_t offset = 0;
        uint32_t stride = 0;
    };

    bool done = false;
    bool hasBuffer = false;
    bool ok = false;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t flags = 0;
    uint32_t planeCount = 0;
    uint64_t modifier = 0;
    Plane planes[4];
    QImage image;
};

void closeFrameFds(CaptureFrame *frame)
{
    for (CaptureFrame::Plane &plane : frame->planes) {
        if (plane.fd >= 0) {
            close(plane.fd);
            plane.fd = -1;
        }
    }
}

QImage::Format imageFormatForDrm(uint32_t format)
{
    switch (format) {
    case DrmFormatArgb8888:
        return QImage::Format_ARGB32;
    case DrmFormatXrgb8888:
        return QImage::Format_RGB32;
    case DrmFormatAbgr8888:
        return QImage::Format_RGBA8888;
    case DrmFormatXbgr8888:
        return QImage::Format_RGBX8888;
    default:
        return QImage::Format_Invalid;
    }
}

bool readLinearBuffer(CaptureFrame *frame)
{
    const CaptureFrame::Plane &plane = frame->planes[0];
    const QImage::Format format = imageFormatForDrm(frame->format);
    if (format == QImage::Format_Invalid || plane.fd < 0) {
        return false;
    }

    const size_t mapSize = size_t(plane.offset) + size_t(plane.stride) * frame->height;
    void *mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, plane.fd, 0);
    if (mapped == MAP_FAILED) {
        return false;
    }

    const auto *pixels = static_cast<const uchar *>(mapped) + plane.offset;
    frame->image = QImage(pixels, int(frame->width), int(frame->height), int(plane.stride), format).copy();
    munmap(mapped, mapSize);
    return !frame->image.isNull();
}

void addPlaneAttributes(std::vector<EGLint> *attributes, uint32_t index,
                        const CaptureFrame::Plane &plane, uint64_t modifier)
{
    static constexpr EGLint FdAttributes[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static constexpr EGLint OffsetAttributes[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static constexpr EGLint PitchAttributes[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static constexpr EGLint ModifierLowAttributes[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static constexpr EGLint ModifierHighAttributes[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    attributes->insert(attributes->end(), {
        FdAttributes[index], plane.fd,
        OffsetAttributes[index], EGLint(plane.offset),
        PitchAttributes[index], EGLint(plane.stride),
    });
    if (modifier != DrmModifierInvalid) {
        attributes->insert(attributes->end(), {
            ModifierLowAttributes[index], EGLint(modifier & 0xffffffffu),
            ModifierHighAttributes[index], EGLint(modifier >> 32),
        });
    }
}

bool readDmabuf(CaptureFrame *frame)
{
    if (frame->planeCount == 0 || frame->planeCount > 4) {
        return false;
    }
    for (uint32_t index = 0; index < frame->planeCount; ++index) {
        if (frame->planes[index].fd < 0) {
            return false;
        }
    }

    QOpenGLContext context;
    if (!context.create()) {
        return false;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface)) {
        return false;
    }

    const EGLDisplay display = eglGetCurrentDisplay();
    const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    using ImageTargetTexture = void (*)(unsigned int, void *);
    const auto imageTargetTexture = reinterpret_cast<ImageTargetTexture>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (display == EGL_NO_DISPLAY || !createImage || !destroyImage || !imageTargetTexture) {
        context.doneCurrent();
        return false;
    }

    std::vector<EGLint> attributes = {
        EGL_WIDTH, EGLint(frame->width),
        EGL_HEIGHT, EGLint(frame->height),
        EGL_LINUX_DRM_FOURCC_EXT, EGLint(frame->format),
    };
    for (uint32_t index = 0; index < frame->planeCount; ++index) {
        addPlaneAttributes(&attributes, index, frame->planes[index], frame->modifier);
    }
    attributes.push_back(EGL_NONE);

    const EGLImageKHR eglImage = createImage(display, EGL_NO_CONTEXT,
                                              EGL_LINUX_DMA_BUF_EXT, nullptr,
                                              attributes.data());
    if (eglImage == EGL_NO_IMAGE_KHR) {
        qWarning("Unable to import Wayland DMA-BUF (EGL error 0x%x)", eglGetError());
        context.doneCurrent();
        return false;
    }

    QOpenGLFunctions *gl = context.functions();
    gl->initializeOpenGLFunctions();
    GLuint texture = 0;
    GLuint framebuffer = 0;
    gl->glGenTextures(1, &texture);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    imageTargetTexture(GL_TEXTURE_2D, eglImage);

    gl->glGenFramebuffers(1, &framebuffer);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture, 0);

    bool ok = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    QImage image(int(frame->width), int(frame->height), QImage::Format_RGBA8888);
    if (ok && !image.isNull()) {
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl->glReadPixels(0, 0, int(frame->width), int(frame->height),
                         GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
        ok = gl->glGetError() == GL_NO_ERROR;
    } else {
        ok = false;
    }

    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glDeleteFramebuffers(1, &framebuffer);
    gl->glDeleteTextures(1, &texture);
    destroyImage(display, eglImage);
    context.doneCurrent();

    if (ok) {
        // wlcom's capture buffer is already in top-left screen orientation.
        // QImage also treats its first row as the top row, so no extra flip is
        // needed after reading the attached EGLImage framebuffer.
        frame->image = image;
    }
    return ok;
}

void finishFrame(CaptureFrame *frame)
{
    frame->done = true;
    if (frame->planes[0].fd < 0 || frame->width == 0 || frame->height == 0) {
        closeFrameFds(frame);
        return;
    }

    const bool linear = frame->modifier == DrmModifierLinear ||
                        frame->modifier == DrmModifierInvalid;
    frame->ok = (linear && readLinearBuffer(frame)) || readDmabuf(frame);
    closeFrameFds(frame);
    if (!frame->ok) {
        qWarning("Unable to decode the Wayland capture buffer");
    }
}

void frameFailed(void *data, kywc_capture_frame_v1 *)
{
    auto *frame = static_cast<CaptureFrame *>(data);
    frame->done = true;
    closeFrameFds(frame);
}

void frameCancelled(void *data, kywc_capture_frame_v1 *)
{
    auto *frame = static_cast<CaptureFrame *>(data);
    frame->done = true;
    closeFrameFds(frame);
}

void frameBuffer(void *data, kywc_capture_frame_v1 *frame, int32_t fd, uint32_t format,
                 uint32_t width, uint32_t height, uint32_t offset, uint32_t stride,
                 uint32_t modifierHi, uint32_t modifierLo, uint32_t flags)
{
    auto *capture = static_cast<CaptureFrame *>(data);
    capture->planes[0] = {fd, offset, stride};
    capture->format = format;
    capture->width = width;
    capture->height = height;
    capture->flags = flags;
    capture->planeCount = 1;
    capture->modifier = (uint64_t(modifierHi) << 32) | uint64_t(modifierLo);
    capture->hasBuffer = true;

    // Version 1 has no buffer_done event, so the buffer event terminates it.
    if (kywc_capture_frame_v1_get_version(frame) < 2) {
        finishFrame(capture);
    }
}

void frameBufferWithPlane(void *data, kywc_capture_frame_v1 *, uint32_t index, int32_t fd,
                          uint32_t offset, uint32_t stride)
{
    auto *capture = static_cast<CaptureFrame *>(data);
    if (index >= 4) {
        close(fd);
        return;
    }
    capture->planes[index] = {fd, offset, stride};
    capture->planeCount = std::max(capture->planeCount, index + 1);
}

void frameBufferDone(void *data, kywc_capture_frame_v1 *)
{
    finishFrame(static_cast<CaptureFrame *>(data));
}

const kywc_capture_frame_v1_listener frameListener = {
    frameFailed,
    frameCancelled,
    frameBuffer,
    frameBufferWithPlane,
    frameBufferDone,
};

void outputName(void *data, kywc_output_v1 *, const char *name)
{
    static_cast<WaylandCapture::Output *>(data)->name = QString::fromUtf8(name);
}
void outputMake(void *, kywc_output_v1 *, const char *) {}
void outputModel(void *, kywc_output_v1 *, const char *) {}
void outputSerial(void *, kywc_output_v1 *, const char *) {}
void outputDescription(void *, kywc_output_v1 *, const char *) {}
void outputPhysicalSize(void *, kywc_output_v1 *, int32_t, int32_t) {}
void outputCapabilities(void *, kywc_output_v1 *, uint32_t) {}
void outputEnabled(void *, kywc_output_v1 *, int32_t) {}
void outputCurrentMode(void *, kywc_output_v1 *, kywc_output_mode_v1 *) {}
void outputPosition(void *data, kywc_output_v1 *, int32_t x, int32_t y)
{
    static_cast<WaylandCapture::Output *>(data)->position = QPoint(x, y);
}
void outputTransform(void *, kywc_output_v1 *, int32_t) {}
void outputScale(void *, kywc_output_v1 *, wl_fixed_t) {}
void outputPower(void *, kywc_output_v1 *, uint32_t) {}
void outputBrightness(void *, kywc_output_v1 *, uint32_t) {}
void outputColorTemp(void *, kywc_output_v1 *, uint32_t) {}
void outputFinished(void *, kywc_output_v1 *) {}

void modeSize(void *, kywc_output_mode_v1 *, int32_t, int32_t) {}
void modeRefresh(void *, kywc_output_mode_v1 *, int32_t) {}
void modePreferred(void *, kywc_output_mode_v1 *) {}
void modeFinished(void *, kywc_output_mode_v1 *mode)
{
    kywc_output_mode_v1_destroy(mode);
}

const kywc_output_mode_v1_listener outputModeListener = {
    modeSize,
    modeRefresh,
    modePreferred,
    modeFinished,
};

void outputMode(void *, kywc_output_v1 *, kywc_output_mode_v1 *mode)
{
    kywc_output_mode_v1_add_listener(mode, &outputModeListener, nullptr);
}

const kywc_output_v1_listener outputListener = {
    outputName,
    outputMake,
    outputModel,
    outputSerial,
    outputDescription,
    outputPhysicalSize,
    outputMode,
    outputCapabilities,
    outputEnabled,
    outputCurrentMode,
    outputPosition,
    outputTransform,
    outputScale,
    outputPower,
    outputBrightness,
    outputColorTemp,
    outputFinished,
};

const wl_registry_listener registryListener = {
    WaylandCapture::registryGlobal,
    WaylandCapture::registryGlobalRemove,
};

const kywc_output_manager_v1_listener outputManagerListener = {
    WaylandCapture::outputManagerOutput,
    WaylandCapture::outputManagerPrimary,
    WaylandCapture::outputManagerDone,
    WaylandCapture::outputManagerFinished,
};

} // namespace

WaylandCapture::WaylandCapture() = default;

WaylandCapture::~WaylandCapture()
{
    destroyObjects();
}

void WaylandCapture::initialize()
{
    if (m_initialized) {
        return;
    }
    m_initialized = true;

    if (!QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        return;
    }

    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    if (!native) {
        return;
    }

    m_display = static_cast<wl_display *>(native->nativeResourceForIntegration("display"));
    if (!m_display) {
        m_display = static_cast<wl_display *>(native->nativeResourceForWindow("display", nullptr));
    }
    if (!m_display) {
        qWarning("Unable to access the Qt Wayland display");
        return;
    }

    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &registryListener, this);
    if (wl_display_roundtrip(m_display) < 0) {
        qWarning("Unable to enumerate Wayland globals");
        return;
    }
    // Manager bindings are requests produced while handling the first
    // roundtrip. A second one receives the initial output list.
    if (m_outputManager && wl_display_roundtrip(m_display) < 0) {
        qWarning("Unable to enumerate Wayland outputs");
    }
}

bool WaylandCapture::available()
{
    initialize();
    return m_display && m_captureManager && !m_outputs.isEmpty();
}

QString WaylandCapture::outputUuid(QScreen *screen) const
{
    if (screen) {
        for (const Output *output : m_outputs) {
            if (output->name == screen->name()) {
                return output->uuid;
            }
        }
        for (const Output *output : m_outputs) {
            if (output->position == screen->geometry().topLeft()) {
                return output->uuid;
            }
        }
    }
    if (m_primaryOutput) {
        return m_primaryOutput->uuid;
    }
    if (!m_outputs.isEmpty()) {
        return m_outputs.constFirst()->uuid;
    }
    return QString();
}

bool WaylandCapture::captureOutput(QScreen *screen, QImage *image)
{
    if (!image || !available()) {
        return false;
    }

    const QByteArray uuid = outputUuid(screen).toUtf8();
    kywc_capture_frame_v1 *frame = kywc_capture_manager_v1_capture_output(
        m_captureManager, 0, uuid.constData());
    if (!frame) {
        return false;
    }

    CaptureFrame capture;
    kywc_capture_frame_v1_add_listener(frame, &frameListener, &capture);
    wl_display_flush(m_display);

    QElapsedTimer timer;
    timer.start();
    while (!capture.done && timer.elapsed() < 1000) {
        wl_display_flush(m_display);
        if (wl_display_prepare_read(m_display) != 0) {
            if (wl_display_dispatch_pending(m_display) < 0) {
                break;
            }
            continue;
        }

        pollfd pollFd = {wl_display_get_fd(m_display), POLLIN, 0};
        const int result = poll(&pollFd, 1, 50);
        if (result > 0 && (pollFd.revents & POLLIN)) {
            if (wl_display_read_events(m_display) < 0) {
                break;
            }
        } else {
            wl_display_cancel_read(m_display);
        }
        if (result > 0 && (pollFd.revents & POLLIN)) {
            wl_display_dispatch_pending(m_display);
        }
    }

    if (!capture.done) {
        closeFrameFds(&capture);
    }
    if (capture.hasBuffer) {
        kywc_capture_frame_v1_release_buffer(frame, 0);
    }
    kywc_capture_frame_v1_destroy(frame);
    wl_display_flush(m_display);

    if (!capture.ok) {
        return false;
    }
    *image = std::move(capture.image);
    return true;
}

void WaylandCapture::destroyObjects()
{
    for (Output *output : std::as_const(m_outputs)) {
        if (output->proxy) {
            kywc_output_v1_destroy(output->proxy);
        }
        delete output;
    }
    m_outputs.clear();
    m_primaryOutput = nullptr;

    if (m_captureManager) {
        kywc_capture_manager_v1_destroy(m_captureManager);
        m_captureManager = nullptr;
    }
    if (m_outputManager) {
        kywc_output_manager_v1_stop(m_outputManager);
        kywc_output_manager_v1_destroy(m_outputManager);
        m_outputManager = nullptr;
    }
    if (m_registry) {
        wl_registry_destroy(m_registry);
        m_registry = nullptr;
    }
    if (m_display) {
        wl_display_flush(m_display);
    }
}

void WaylandCapture::registryGlobal(void *data, wl_registry *registry, unsigned int name,
                                    const char *interface, unsigned int version)
{
    auto *capture = static_cast<WaylandCapture *>(data);
    if (std::strcmp(interface, kywc_capture_manager_v1_interface.name) == 0 &&
        !capture->m_captureManager) {
        const uint32_t bindVersion = std::min(version, 2u);
        capture->m_captureManager = static_cast<kywc_capture_manager_v1 *>(
            wl_registry_bind(registry, name, &kywc_capture_manager_v1_interface, bindVersion));
    } else if (std::strcmp(interface, kywc_output_manager_v1_interface.name) == 0 &&
               !capture->m_outputManager) {
        capture->m_outputManager = static_cast<kywc_output_manager_v1 *>(
            wl_registry_bind(registry, name, &kywc_output_manager_v1_interface, 1));
        kywc_output_manager_v1_add_listener(capture->m_outputManager, &outputManagerListener,
                                            capture);
    }
}

void WaylandCapture::registryGlobalRemove(void *, wl_registry *, unsigned int)
{
}

void WaylandCapture::outputManagerOutput(void *data, kywc_output_manager_v1 *,
                                          kywc_output_v1 *output, const char *uuid)
{
    auto *capture = static_cast<WaylandCapture *>(data);
    auto *entry = new Output;
    entry->uuid = QString::fromUtf8(uuid);
    entry->proxy = output;
    capture->m_outputs.append(entry);
    kywc_output_v1_set_user_data(output, entry);
    kywc_output_v1_add_listener(output, &outputListener, entry);
}

void WaylandCapture::outputManagerPrimary(void *data, kywc_output_manager_v1 *, kywc_output_v1 *output)
{
    auto *entry = static_cast<Output *>(kywc_output_v1_get_user_data(output));
    if (entry) {
        static_cast<WaylandCapture *>(data)->m_primaryOutput = entry;
    }
}

void WaylandCapture::outputManagerDone(void *data, kywc_output_manager_v1 *)
{
    static_cast<WaylandCapture *>(data)->m_outputsReady = true;
}

void WaylandCapture::outputManagerFinished(void *data, kywc_output_manager_v1 *)
{
    auto *capture = static_cast<WaylandCapture *>(data);
    if (capture->m_outputManager) {
        kywc_output_manager_v1_destroy(capture->m_outputManager);
        capture->m_outputManager = nullptr;
    }
}
