# CMakeLists.txt Refactor Plan

## Goal
The current `CMakeLists.txt` is over 1900 lines long and acts as a monolith, handling options, dependency resolution, core library builds, element/plugin builds, testing, installation, and packaging. This plan outlines a strategy to refactor it into a modular, hierarchical structure to improve maintainability, reduce merge conflicts, and prevent variable leakage.

## Proposed Directory Structure
```
zstreamer/
├── CMakeLists.txt                # Root: Minimal setup, includes, and add_subdirectory calls
├── cmake/
│   ├── Options.cmake             # Global build options (ENABLE_*)
│   ├── Dependencies.cmake        # find_package, pkg_check_modules, and HAS_* flags
│   └── Packaging.cmake           # CPack configurations (if applicable)
├── src/
│   ├── CMakeLists.txt            # Builds `zstreamer` core library
│   └── plugins/
│       └── CMakeLists.txt        # Builds `zstreamer-elements` and all dynamic plugins
└── tests/
    └── CMakeLists.txt            # Builds unit tests, integration tests, and demos
```

## Refactoring Steps

### Phase 1: Setup and Configuration Extraction
1. Create `cmake/Options.cmake`. Move all `option(...)` declarations here.
2. Create `cmake/Dependencies.cmake`. Move all `find_package()`, `pkg_check_modules()`, and dependency validation logic here.
3. Update root `CMakeLists.txt` to `include(cmake/Options.cmake)` and `include(cmake/Dependencies.cmake)`.

### Phase 2: Core Library
1. Create `src/CMakeLists.txt`.
2. Move the `zstreamer` static/shared library definition, its source list (`CORE_SOURCES`), and target properties (include directories, link libraries) into this file.
3. Add `add_subdirectory(src)` to the root `CMakeLists.txt`.

### Phase 3: Elements and Plugins
1. Create `src/plugins/CMakeLists.txt` (or just `src/CMakeLists.txt` might be enough for both, but `src/plugins` or `src/elements` is cleaner). Let's use `src/elements/CMakeLists.txt` for consistency with `zstreamer-elements`.
2. Move the `zstreamer-elements` library definition and its source list into this file.
3. Move all individual dynamic plugin definitions (`zst_filesink`, `zst_v4l2source`, etc.) into this file.

### Phase 4: Tests and Demos
1. Create `tests/CMakeLists.txt`.
2. Move all `add_executable()` and `add_test()` calls, along with their target properties, into this file.
3. Ensure this directory is conditionally added in the root:
   ```cmake
   if(BUILD_TESTS)
       add_subdirectory(tests)
   endif()
   ```

### Phase 5: Installation and Packaging
1. Isolate the `install()` and CPack logic.
2. Place target-specific `install()` commands in their respective `CMakeLists.txt` (e.g., install headers in `src/`, install tests in `tests/`).
3. Move top-level packaging variables into `cmake/Packaging.cmake` or keep at the very bottom of the root `CMakeLists.txt`.

## Benefits
- **Scoped Variables**: CMake variables will be constrained to the directories where they are needed, reducing side-effects.
- **Improved Readability**: Contributors can find the exact file related to the module they are modifying.
- **Conflict Reduction**: Concurrent development on tests, plugins, and core will not result in conflicts in a single root file.
