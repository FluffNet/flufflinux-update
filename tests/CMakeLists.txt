cxx_qt_import_crate(
    MANIFEST_PATH ${CMAKE_SOURCE_DIR}/Cargo.toml
    # Share the production export directory so dependency build scripts do not
    # rebuild Qt bindings whenever Cargo switches between the two crates.
    CXX_QT_EXPORT_DIR ${CMAKE_BINARY_DIR}/cxxqt/
    CRATES flu_test_bridge
    LOCKED
    QT_MODULES Qt6::Core
)

add_executable(signingkeyrecoverytest
    signingkeyrecoverytest.cpp
    support/signingkeyrecovery.cpp
    support/signingkeyrecovery.h
    support/securitydiagnostics.cpp
    support/securitydiagnostics.h
)
target_include_directories(signingkeyrecoverytest PRIVATE support)
target_link_libraries(signingkeyrecoverytest PRIVATE Qt6::Core Qt6::Test flu_test_bridge)
add_test(NAME signingkeyrecoverytest COMMAND signingkeyrecoverytest)

add_executable(uistatetest uistatetest.cpp ../src/nativeqt.cpp)
target_include_directories(uistatetest PRIVATE ../src)
target_compile_definitions(uistatetest PRIVATE FLUFFLINUX_LOCALE_DIR=\"${KDE_INSTALL_FULL_LOCALEDIR}\")
target_link_libraries(uistatetest PRIVATE Qt6::Core Qt6::Test Qt6::Widgets Qt6::Network KF6::I18n flu_bridge)
add_test(NAME uistatetest COMMAND uistatetest)

add_executable(updateswindowkeystest updateswindowkeystest.cpp)
target_link_libraries(updateswindowkeystest PRIVATE Qt6::Quick Qt6::Qml Qt6::Test)
target_compile_definitions(updateswindowkeystest PRIVATE
    FLU_QML_PATH=\"${CMAKE_SOURCE_DIR}/src/ui/main.qml\")
add_test(NAME updateswindowkeystest COMMAND updateswindowkeystest)

add_executable(panelvisibilitytest panelvisibilitytest.cpp)
target_link_libraries(panelvisibilitytest PRIVATE Qt6::Quick Qt6::Qml Qt6::Test)
target_compile_definitions(panelvisibilitytest PRIVATE
    FLU_QML_PATH=\"${CMAKE_SOURCE_DIR}/src/ui/main.qml\")
add_test(NAME panelvisibilitytest COMMAND panelvisibilitytest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(FLU_MSGFMT msgfmt REQUIRED)
find_program(FLU_MAKE NAMES gmake make REQUIRED)
add_test(NAME fakerootpackagingtest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fakeroot_packaging.py
        --source ${CMAKE_SOURCE_DIR} --cmake ${CMAKE_COMMAND} --make ${FLU_MAKE})
add_test(NAME batterytranslationstest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/battery_translations.py
        --source ${CMAKE_SOURCE_DIR} --msgfmt ${FLU_MSGFMT})
