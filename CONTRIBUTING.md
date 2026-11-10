# Contributing to USBUtils

Thanks for wanting to help. This guide is written for first-time
contributors: if you have never sent a pull request before, you are exactly
who it is for. If anything is unclear, open an issue and ask. Asking counts
as contributing too.

## Ways to help

- **Report bugs** with a good description. When you open an issue, GitHub
  offers a bug report form that asks for the right things.
- **Suggest features** you would actually use.
- **Improve the docs.** Typos and clearer wording are real contributions.
- **Write code**: bug fixes, features, tests.
- **Test releases** on hardware and systems the maintainer does not own,
  and report how it went.

## Getting the code

You need Git, a C++17 compiler, and CMake 3.16 or newer. Qt 5 or Qt 6
Widgets is needed only for the GUI.

```bash
# 1. Fork the repository on GitHub (top right), then:
git clone https://github.com/<your-name>/USBUtils_src.git
cd USBUtils_src

# 2. Build and run the tests
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build

# 3. No Qt on your machine? Build the CLI and core only:
cmake -B build -DUSBUTILS_BUILD_GUI=OFF
```

All tests must pass before you send anything upstream.

## Finding your way around

```
src/core/    the engine: formats, install/export, all tools.
             Portable C++17, no external libraries.
src/cli/     the usbutils command line tool.
src/gui/     the usbutils-gui app. Qt lives here, and only here.
tests/       plain-assert tests, no framework.
docs/        reverse-engineering notes and format specifications.
```

Rule of thumb: if a feature should work without a screen (CLI, tests), it
belongs in core. The GUI only wires the engine to windows and buttons.

## Making changes

1. Create a branch with a short, descriptive name:
   `fix/video-rip-backup`, `feat/iso-compare`, `docs/usage-guide`.
2. Make the change, and keep it focused: one branch, one topic.
3. Add or extend a test in `tests/test_core.cpp` when it makes sense.
4. Build and run the tests again.
5. Commit, push, and open a pull request.

### Code conventions

- C++17. The project builds warning-free with `-Wall -Wextra`; keep it that
  way.
- No new dependencies in the core library.
- Qt calls stay in `src/gui`.
- Comment only what the code cannot say for itself: a tricky format quirk,
  a non-obvious invariant, a workaround with a story. Do not narrate the
  obvious.
- Keep diffs focused. If you spot unrelated code that looks wrong or
  unused, leave it alone and open an issue instead.

### Commit messages

Conventional Commits style: `type: short lowercase summary`, no trailing
period. Common types:

```
fix:      bug fix
feat:     new feature
docs:     documentation only
chore:    tooling, version bumps, packaging
perf:     performance improvement
core:     engine work (project-specific type)
```

Real examples from this repository:

```
fix: address antislop audit 001 findings
docs: correct GUI cancelability claim in README
core: cache GameStream chunk handles; trim I/O overhead
```

### Pull requests

- One pull request per topic. Small pull requests get reviewed faster.
- Fill in the pull request template: summary, related issue, type of
  change, how you tested it, and the checklist.
- Link the issue you are fixing with `Fixes #123` so it closes on merge.
- You may be asked for tweaks. That is normal review flow, not rejection.

## Reporting bugs

Open an [issue](https://github.com/AldySan/USBUtils_src/issues); GitHub
will offer the Bug report and Feature request forms. A good bug report
mentions the app (GUI or CLI), the version, your operating system, the
steps to reproduce, and what you expected instead.

If the app crashed and wrote a `.dmp` file, please attach it. The project
ships its own tooling to read those dumps; see
[tools/README.md](tools/README.md).

## Code of conduct

Be kind. Please read the [Code of Conduct](CODE_OF_CONDUCT.md); it is short.

## License

By contributing, you agree that your contributions are licensed under the
MIT License, like the rest of the project.
