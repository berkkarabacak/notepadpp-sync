cmake_minimum_required(VERSION 3.20)
project(NppSync VERSION 1.0.0 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# Notepad++ Sync plugin. Build:
#   cmake -B build -A x64
#   cmake --build build --config Release
#   ctest --test-dir build -C Release
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(NPSYNC_BUILD_TESTS "Build plugin core tests" ON)

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)

include(FetchContent)

# ---- nlohmann/json (header-only) ----
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760e6b142d42d1aa959b970
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ---- SQLite amalgamation ----
FetchContent_Declare(
    sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
)
FetchContent_MakeAvailable(sqlite_amalgamation)
add_library(sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- plugin core (portable, unit-tested) ----
add_library(npsync_core STATIC
    src/core/Crypto.cpp
    src/core/Crypto.h
    src/core/Merge.cpp
    src/core/Merge.h
    src/core/IgnoreRules.cpp
    src/core/IgnoreRules.h
    src/core/PathUtil.cpp
    src/core/PathUtil.h
    src/core/VersionVector.cpp
    src/core/VersionVector.h
)
target_include_directories(npsync_core PUBLIC src)
target_link_libraries(npsync_core PUBLIC bcrypt)
if(MSVC)
    target_compile_options(npsync_core PRIVATE /W4)
endif()

# ---- the plugin DLL ----
add_library(NppSync SHARED
    src/DllMain.cpp
    src/PluginDefinition.cpp
    src/PluginDefinition.h
    src/PluginInterface.h
    src/Notepad_plus_msgs.h
    src/ApiClient.cpp
    src/ApiClient.h
    src/LocalDb.cpp
    src/LocalDb.h
    src/FolderWatcher.cpp
    src/FolderWatcher.h
    src/SyncEngine.cpp
    src/SyncEngine.h
    src/Settings.cpp
    src/Settings.h
    src/Dialogs.cpp
    src/Dialogs.h
    src/Logger.cpp
    src/Logger.h
    src/resource.rc
)
target_include_directories(NppSync PRIVATE src)
target_link_libraries(NppSync PRIVATE
    npsync_core sqlite3 nlohmann_json::nlohmann_json
    bcrypt winhttp crypt32 comctl32 shell32 ole32
)
set_target_properties(NppSync PROPERTIES
    OUTPUT_NAME "NppSync"
    PREFIX ""
)
if(MSVC)
    target_compile_options(NppSync PRIVATE /W4 /permissive-)
    # Static CRT so the DLL has no runtime redistributable dependency.
    set_property(TARGET NppSync PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET npsync_core PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET sqlite3 PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- tests ----
if(NPSYNC_BUILD_TESTS)
    enable_testing()
    add_executable(npsync_core_tests tests/core_tests.cpp)
    target_link_libraries(npsync_core_tests PRIVATE npsync_core)
    if(MSVC)
        target_compile_options(npsync_core_tests PRIVATE /W4)
        set_property(TARGET npsync_core_tests PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME core_tests COMMAND npsync_core_tests)
endif()

# ---- install / packaging layout ----
install(TARGETS NppSync RUNTIME DESTINATION .)
