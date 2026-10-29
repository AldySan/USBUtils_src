// SPDX-License-Identifier: MIT
/*
 * USBUtils — open reimplementation of USBUtil v2.2 (PS2 USBExtreme manager)
 * Copyright (c) 2025 AldySan
 * Based on the reverse-engineering of "USBUtil v2.2 rev1.0" by ISEKO (2009).
 */
#pragma once

#include <QThread>

#include <functional>

namespace usbutils {

/// A QThread that runs a plain std::function in its run() override.
///
/// Why not QThread::create()? Qt >= 6.8 implements QThread::create() on top
/// of std::async(std::launch::deferred, ...). In our MinGW-w64 builds that
/// machinery (libstdc++ _Deferred_state + winpthreads CRITICAL_SECTION)
/// corrupted heap state and crashed the GUI ~3 s after startup, right when
/// the automatic update-check thread finished — DeleteCriticalSection hit a
/// smashed RTL_CRITICAL_SECTION_DEBUG list and faulted (full analysis in
/// docs/crash-v0.0.3-startup.md). A virtual run() override on a QThread
/// subclass uses none of the std::async machinery and is the classic,
/// battle-tested Qt worker-thread form.
///
/// Lifetime contract: the object deletes ITSELF via the built-in
/// finished -> deleteLater connection below (the canonical Qt pattern; the
/// deleteLater event is delivered on the thread that CREATED the object,
/// after the worker has unwound run()). Call sites must never call
/// deleteLater() or delete on a WorkerThread themselves, and must never
/// keep a long-lived owning pointer to it.
class WorkerThread final : public QThread {
public:
    explicit WorkerThread(std::function<void()> fn, QObject* parent = nullptr)
        : QThread(parent), fn_(std::move(fn))
    {
        connect(this, &QThread::finished, this, &QObject::deleteLater);
    }

    void run() override { fn_(); }

private:
    std::function<void()> fn_;
};

} // namespace usbutils
