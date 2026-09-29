cmake_minimum_required(VERSION 3.20)
project(CppReviewer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(CPPREVIEW_BUILD_GUI "Build the Qt GUI" ON)

# ---------------------------------------------------------------- libclang
set(LLVM_ROOT_HINTS)
foreach(ver 22 21 20 19 18)
    list(APPEND LLVM_ROOT_HINTS /usr/lib/llvm-${ver})
endforeach()

find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
    HINTS ${LIBCLANG_ROOT} ENV LIBCLANG_ROOT ${LLVM_ROOT_HINTS} /usr/local/opt/llvm /opt/homebrew/opt/llvm
    PATH_SUFFIXES include)
find_library(LIBCLANG_LIBRARY NAMES clang libclang
    HINTS ${LIBCLANG_ROOT} ENV LIBCLANG_ROOT ${LLVM_ROOT_HINTS} /usr/local/opt/llvm /opt/homebrew/opt/llvm
    PATH_SUFFIXES lib)
if(NOT LIBCLANG_INCLUDE_DIR OR NOT LIBCLANG_LIBRARY)
    message(FATAL_ERROR "libclang not found. Install libclang-dev (e.g. libclang-22-dev) or set LIBCLANG_ROOT.")
endif()
message(STATUS "libclang: ${LIBCLANG_LIBRARY} (${LIBCLANG_INCLUDE_DIR})")

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- core (no Qt)
add_library(cppreview_core STATIC
    src/core/ClangProject.cpp
    src/core/CompileDb.cpp
    src/core/Diff.cpp
    src/core/Git.cpp
    src/core/Json.cpp
    src/core/Model.cpp
    src/core/Process.cpp
    src/core/Review.cpp
    src/core/SemanticDiff.cpp
    src/core/Session.cpp
    src/core/Snapshot.cpp
)
target_include_directories(cppreview_core PUBLIC src)
target_include_directories(cppreview_core SYSTEM PUBLIC ${LIBCLANG_INCLUDE_DIR})
target_link_libraries(cppreview_core PUBLIC ${LIBCLANG_LIBRARY} Threads::Threads)
target_compile_options(cppreview_core PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------- CLI
add_executable(cppreview-cli src/cli/main.cpp)
target_link_libraries(cppreview-cli PRIVATE cppreview_core)

# ---------------------------------------------------------------- GUI
if(CPPREVIEW_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent)
    set(CMAKE_AUTOMOC ON)
    add_executable(cppreviewer
        src/gui/main.cpp
        src/gui/MainWindow.cpp
        src/gui/CodeView.cpp
        src/gui/DiffView.cpp
        src/gui/CppHighlighter.cpp
        src/gui/RevisionDialog.cpp
        src/gui/GitHubClient.cpp
    )
    target_link_libraries(cppreviewer PRIVATE cppreview_core Qt6::Widgets Qt6::Network Qt6::Concurrent)
    target_compile_options(cppreviewer PRIVATE -Wall -Wextra)
    install(TARGETS cppreviewer cppreview-cli RUNTIME DESTINATION bin)
else()
    install(TARGETS cppreview-cli RUNTIME DESTINATION bin)
endif()
