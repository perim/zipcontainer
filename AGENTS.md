# Repository Guidelines

## Project Structure & Module Organization
- Core library lives in `zipc.cpp`/`zipc.h`; it exposes a small C API for reading/writing uncompressed ZIP containers.
- Build configuration is in `CMakeLists.txt`.
- Tests reside in `tests/zipctest.cpp`; they currently focus on basic open/write/read flows.
- Artifacts are kept in a `build/` directory when following the commands below.
- We do not have and do not want any external dependencies.

## Build, Test, and Development Commands
- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` (use `Debug` while iterating).
- Build library and test binary: `cmake --build build`.
- Run tests: `ctest --test-dir build --output-on-failure`.
- Clean build artifacts: `cmake --build build --target clean`.
- Verify created zip files with `zip -T <filename.zip>`

## Coding Style & Naming Conventions
- Language: C++20 with `-Wall`; avoid compiler warnings.
- Indentation: Tab indentation and Allman braces (opening brace on a new line).
- Public API uses `zipc_*` and `zipcstream_*` snake_case; keep new functions consistent.
- Keep headers minimal; prefer forward declarations over extra includes to limit compile time.
- No formatter is enforced; keep diffs small and readable.

## Testing Guidelines
- Framework: plain `assert` within `tests/zipctest.cpp` executed via `ctest`.
- Name new tests descriptively and group related checks in functions for clarity.
- Add coverage for both happy paths and error handling (e.g., missing entries, invalid modes).
- When adding APIs, include at least one integration-style test that exercises `zipc_open` → operation → `zipc_close`.

## Commit & Pull Request Guidelines
- No formal commit style is established yet; use a concise imperative subject and keep bodies brief with rationale and risks.

## Security & Configuration Tips
- Library intentionally handles uncompressed ZIP files only. Make sure we fail gracefully on compressed files.
- We do not want to add any thread synchronization inside the library. Document any concurrency guarantees added or removed.
