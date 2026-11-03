// SPDX-License-Identifier: MIT
/*
 * USBUtils — open reimplementation of USBUtil v2.2 (PS2 USBExtreme manager)
 * Copyright (c) 2025 AldySan
 * Based on the reverse-engineering of "USBUtil v2.2 rev1.0" by ISEKO (2009).
 */
#pragma once

#include <QEventLoop>
#include <QThread>

#include <functional>
#include <memory>

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

/// Run `fn` on a WorkerThread and block until it finishes, keeping the
/// GUI painting (a real QEventLoop, not a processEvents() spin, which
/// re-enters arbitrary slots and lets users re-trigger operations
/// mid-flight). For operations that are normally fast but can occasionally
/// take minutes (e.g. the DNAS patch's whole-stream IOPRP fallback scan).
///
/// Lifetime rules that keep this safe:
///  - `fn` is COPIED into the worker lambda (never capture the reference
///    parameter), and the result is written through a heap-allocated
///    shared slot — no stack references cross the thread boundary.
///  - The WorkerThread object deletes itself via its built-in
///    finished->deleteLater connection; the DeferredDelete is processed by
///    the OUTER event loop after this nested loop has unwound, i.e.
///    strictly after the worker has returned from run() — no
///    use-after-free window.
///  - Callers must disable their dialog first (setEnabled(false)) so the
///    nested loop cannot re-enter the triggering slot, and the dialog must
///    stay alive for the duration (stack-allocated modals qualify).
template <typename F>
auto runOnThread(F&& fn) -> decltype(fn()) {
    using T = decltype(fn());
    auto result = std::make_shared<T>();          // shared heap slot
    QThread* th = new WorkerThread(
        [result, fn = std::forward<F>(fn)]() { *result = fn(); });
    QEventLoop loop;
    QObject::connect(th, &QThread::finished, &loop, &QEventLoop::quit);
    th->start();
    loop.exec();                                   // paint, stay responsive
    return std::move(*result);
}

} // namespace usbutils
