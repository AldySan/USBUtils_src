# Crash analysis: GUI crash ~3 s after startup (Windows, v0.0.3)

User-visible symptom: `usbutils-gui.exe` starts, the window appears, and
roughly **three seconds later the process dies** with no dialog. A
minidump (`usbutils-gui.exe.16584.dmp`, access violation) was provided
and analyzed with the exact v0.0.3 symbols (module TimeDateStamp
`0x6ae2e4b5` matched the released binary byte-for-byte).

## What the dump showed

* Exception `0xC0000005` (read access violation) at
  `ntdll.dll+0x62491`, faulting address `0xFFFFFFFF00000008`, on a
  **worker thread** (not the GUI thread).
* The crashing instruction is the `RTL_CRITICAL_SECTION_DEBUG`
  `ProcessLocksList` unlink validation inside `RtlDeleteCriticalSection`:

  ```asm
  mov  rcx, [rax]        ; ProcessLocksList.Flink  = 0xFFFFFFFF00000000
  test rcx, rcx
  je   done
  mov  rdx, [rax+8]      ; Blink                    = 0x800012021E8725F1
  cmp  [rcx+8], rax      ; <-- FAULT: Flink is garbage
  ```

* The stack contained exactly one exe return address,
  `usbutils-gui.exe+0x26ce`, which resolves (DWARF) to
  `_Sp_counted_ptr_inplace<std::__future_base::_Deferred_state<...>>::_M_dispose()`
  — i.e. the destruction of a `std::async` deferred future's shared
  state, immediately after `__gthr_win32_mutex_destroy` →
  `DeleteCriticalSection`.
* The destroyed state was instantiated for
  `QThread::create<MainWindow::handleUpdateResult(bool)::{lambda()#1}>`.

## Why that code was running at all

The GUI does a silent update check shortly after startup:

```cpp
QTimer::singleShot(2500, this, &MainWindow::autoUpdateCheck);   // 2.5 s
```

`QTimer::singleShot(2500)` + DNS + TLS + HTTP round-trip = the ~3 s
delay the user measured. `autoUpdateCheck()` spawns the worker through
`QThread::create(...)`. In Qt ≥ 6.9 that convenience function is
implemented on top of `std::async(std::launch::deferred, ...)`
(`qthread.h`, `#include <future> // for std::async`):

```cpp
QThread *QThread::create(Function &&f, Args &&... args)
{
    auto threadFunction = [f](auto &&... largs) mutable -> void { ... };
    return createThreadImpl(std::async(std::launch::deferred,
                                       std::move(threadFunction), ...));
}
```

The deferred-state object embeds a `std::mutex` (a winpthreads
`CRITICAL_SECTION`) and a `std::condition_variable`. In our MinGW-w64
builds (libstdc++ + winpthreads DLLs) that machinery corrupted heap
state: by the time the worker thread finished and destroyed the state,
its critical-section debug block already contained garbage
(`Flink = 0xFFFFFFFF00000000`, `Blink = 0x800012021E8725F1`). A stale
`pthread_mutex_lock` frame on the same stack additionally showed the
mutex's "valid" word being read as `-1` — the object had been smashed
while the update check was still running. The crashing
`DeleteCriticalSection` was merely the first operation to *validate*
the corrupted list and die on it.

## The fix (v0.0.4)

* All `QThread::create(...)` call sites (10 of them, across
  mainwindow / finddialog / gamefilesdialog / videoripdialog /
  ps2loaddialog) now use `usbutils::WorkerThread`
  (`src/gui/workerthread.h`): a plain `QThread` subclass whose
  `run()` override invokes a `std::function`. No `std::async`, no
  deferred state, no embedded mutex/cv — the classic and
  battle-tested Qt worker-thread form.
* `MainWindow::~MainWindow()` now waits for an in-flight update check
  (previously the worker captured `this` with no shutdown guard).
* `httpGet()` (WinHTTP path) got a defensive URL-parse guard.

The same replacement was applied to the *other* background jobs
(copy/move/extract/inject/video-rip/drive report/find/PS2Load) even
though only the update check crashed in practice — they all rode the
same fragile `std::async` machinery, and the fix is free.
