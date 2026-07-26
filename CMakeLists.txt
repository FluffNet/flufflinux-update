cmake_minimum_required(VERSION 3.20)

project(flufflinux_update_kcm VERSION 1.0.0 LANGUAGES CXX)

set(QT_MIN_VERSION "6.5.0")
set(KF6_MIN_VERSION "6.0.0")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core DBus Gui Network Qml Quick Widgets)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS CoreAddons I18n KCMUtils)

add_subdirectory(src)

# Install gettext catalogs into the fakeroot alongside the KCM.
ki18n_install(po)

# Include the project license in packages staged through DESTDIR.
install(FILES LICENSE
    DESTINATION ${KDE_INSTALL_DATADIR}/licenses/flufflinux-update
)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
