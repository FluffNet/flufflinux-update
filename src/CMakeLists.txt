kcmutils_add_qml_kcm(kcm_fluffupdates DISABLE_DESKTOP_FILE_GENERATION)

# KDE derives the launcher identity from the metadata filename. Keep the
# installed module name stable while maintaining the source as kcm_flu.json.
configure_file(kcm_flu.json ${CMAKE_CURRENT_BINARY_DIR}/kcm_fluffupdates.json COPYONLY)
add_custom_target(kcm_fluffupdates-kcm-desktop-gen
    COMMAND KF6::kcmdesktopfilegenerator
        ${CMAKE_CURRENT_BINARY_DIR}/kcm_fluffupdates.json
        ${CMAKE_CURRENT_BINARY_DIR}/kcm_fluffupdates.desktop
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kcm_fluffupdates.json
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/kcm_fluffupdates.desktop
    VERBATIM)
add_dependencies(kcm_fluffupdates kcm_fluffupdates-kcm-desktop-gen)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kcm_fluffupdates.desktop
    DESTINATION ${KDE_INSTALL_APPDIR})

target_sources(kcm_fluffupdates PRIVATE
    flufflinuxupdate.cpp
    flufflinuxupdate.h
    nativeqt.cpp
    nativeqt.h
    kcm_flu.json
)

target_link_libraries(kcm_fluffupdates PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::KCMUtils
    flu_bridge
)

target_compile_definitions(kcm_fluffupdates PRIVATE
    TRANSLATION_DOMAIN=\"kcm_fluffupdates\"
    FLUFFLINUX_LOCALE_DIR=\"${KDE_INSTALL_FULL_LOCALEDIR}\"
    PROJECT_VERSION=\"${PROJECT_VERSION}\"
)

find_program(FLU_CARGO cargo REQUIRED)
set(FLU_SERVICE_TARGET ${CMAKE_BINARY_DIR}/service-target)
set(FLU_HELPER ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/flufflinux-update-helper)
set(FLU_WORKER ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/flufflinux-update-worker)
set(FLU_NOTIFIER ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/flufflinux-update-notifier)
file(GLOB_RECURSE FLU_RUST_SERVICE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/rust/flu-service/src/*.rs
    ${CMAKE_SOURCE_DIR}/rust/flu-core/src/*.rs)
add_custom_command(OUTPUT ${FLU_HELPER} ${FLU_WORKER} ${FLU_NOTIFIER}
    COMMAND ${FLU_CARGO} build --locked --release --package flu-service
        --manifest-path ${CMAKE_SOURCE_DIR}/Cargo.toml --target-dir ${FLU_SERVICE_TARGET}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy ${FLU_SERVICE_TARGET}/release/flufflinux-update-helper ${FLU_HELPER}
    COMMAND ${CMAKE_COMMAND} -E copy ${FLU_SERVICE_TARGET}/release/flufflinux-update-worker ${FLU_WORKER}
    COMMAND ${CMAKE_COMMAND} -E copy ${FLU_SERVICE_TARGET}/release/flufflinux-update-notifier ${FLU_NOTIFIER}
    DEPENDS ${FLU_RUST_SERVICE_SOURCES} ${CMAKE_SOURCE_DIR}/Cargo.toml
        ${CMAKE_SOURCE_DIR}/Cargo.lock ${CMAKE_SOURCE_DIR}/rust/flu-service/Cargo.toml
        ${CMAKE_SOURCE_DIR}/rust/flu-core/Cargo.toml
    COMMENT "Building the Rust update helper, worker and session notifier" VERBATIM)
add_custom_target(flu-services ALL DEPENDS ${FLU_HELPER} ${FLU_WORKER} ${FLU_NOTIFIER})
install(PROGRAMS ${FLU_HELPER} ${FLU_WORKER} ${FLU_NOTIFIER} DESTINATION lib/flufflinux-update)
install(FILES org.flufflinux.Update.Notifications.service DESTINATION share/dbus-1/services)
# These same SVG files are embedded in the QML panel by kcmutils_add_qml_kcm.
# Register them with the desktop icon theme for the native notification body.
install(FILES
    ui/icons/flufflinux-update-success.svg
    ui/icons/flufflinux-update-error.svg
    DESTINATION ${KDE_INSTALL_DATADIR}/icons/hicolor/scalable/status)
install(FILES flufflinux-update.service
    DESTINATION lib/systemd/system
)

install(FILES org.flufflinux.update.policy
    DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions
)

# Package-removal policy consumed by the privileged helper. With DESTDIR this
# is staged as fakeroot/etc/pacman.d/flufflinux-update-package-protection.json.
install(FILES flufflinux-update-package-protection.json
    DESTINATION ${KDE_INSTALL_SYSCONFDIR}/pacman.d
)

# Also expose the panel as a normal application-search result.
install(PROGRAMS org.flufflinux.update.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

# Desktop identity for native inhibitor/job name/icon lookup. Keep the existing
# translated launcher unchanged and do not add another visible menu entry.
install(FILES org.flufflinux.update.worker.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)
