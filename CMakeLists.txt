cmake_minimum_required(VERSION 3.24)

project(flufflinux_update_kcm VERSION 1.5.0 LANGUAGES CXX)

option(BUILD_TESTING "Build the isolated regression tests" OFF)
include(CTest)

set(QT_MIN_VERSION "6.9.0")
set(KF6_MIN_VERSION "6.0.0")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

# Fluff Linux follows Arch's filesystem layout. Never let multilib host
# detection or Qt's absolute installation paths redirect package files into a
# non-Arch library directory or mix absolute and relative KDE paths.
set(KDE_INSTALL_USE_QT_SYS_PATHS OFF CACHE BOOL
    "Do not adopt Qt's absolute installation path scheme" FORCE)
# Keep these as literal relative directory names. Declaring them as CACHE PATH
# values makes CMake resolve command-line values such as "lib" against the
# source directory, turning only LIBDIR into an absolute path.
set(CMAKE_INSTALL_LIBDIR "lib" CACHE STRING "Library installation directory" FORCE)
set(KDE_INSTALL_BINDIR "bin" CACHE STRING "KDE executable installation directory" FORCE)
set(KDE_INSTALL_LIBDIR "lib" CACHE STRING "KDE library installation directory" FORCE)
set(KDE_INSTALL_INCLUDEDIR "include" CACHE STRING "KDE header installation directory" FORCE)
# Qt 6 plugins on Arch live below /usr/lib/qt6/plugins. Keeping lib itself
# relative prevents lib64 output, while this explicit Qt subdirectory keeps
# KCM plugins discoverable by System Settings and kcmshell6.
set(KDE_INSTALL_PLUGINDIR "lib/qt6/plugins" CACHE STRING
    "KDE Qt plugin installation directory" FORCE)

include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Network Qml Quick Widgets)
if(BUILD_TESTING)
    find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)
endif()
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS CoreAddons I18n KCMUtils)

# Use the same CXX-Qt stack as the other Rust/QML Fluff Linux applications.
# Cargo owns the Rust core; CMake retains KDE plugin and package installation.
include(FetchContent)
FetchContent_Declare(CxxQt
    GIT_REPOSITORY https://github.com/KDAB/cxx-qt-cmake.git
    GIT_TAG 0.10.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(CxxQt)
cxx_qt_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    CRATES flu_bridge
    LOCKED
    QT_MODULES Qt6::Core
)

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Install gettext catalogs into the fakeroot alongside the KCM.
ki18n_install(po)

# Include the project license in packages staged through DESTDIR.
install(FILES LICENSE
    DESTINATION ${KDE_INSTALL_DATADIR}/licenses/flufflinux-update
)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
