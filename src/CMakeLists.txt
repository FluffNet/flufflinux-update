kcmutils_add_qml_kcm(kcm_fluffupdates)

target_sources(kcm_fluffupdates PRIVATE
    fluffupdates.cpp
    fluffupdates.h
    kcm_fluffupdates.json
)

target_link_libraries(kcm_fluffupdates PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::KCMUtils
)

target_compile_definitions(kcm_fluffupdates PRIVATE
    TRANSLATION_DOMAIN=\"kcm_fluffupdates\"
    FLUFFLINUX_LOCALE_DIR=\"${KDE_INSTALL_FULL_LOCALEDIR}\"
)

add_executable(flufflinux-update-helper updatehelper.cpp)
target_link_libraries(flufflinux-update-helper PRIVATE Qt6::Core)
install(TARGETS flufflinux-update-helper
    RUNTIME DESTINATION ${KDE_INSTALL_LIBDIR}/flufflinux-update
)

add_executable(flufflinux-update-worker updateworker.cpp)
target_link_libraries(flufflinux-update-worker PRIVATE Qt6::Core)
install(TARGETS flufflinux-update-worker
    RUNTIME DESTINATION ${KDE_INSTALL_LIBDIR}/flufflinux-update
)
install(FILES flufflinux-update.service
    DESTINATION ${KDE_INSTALL_LIBDIR}/systemd/system
)

install(FILES org.flufflinux.update.policy
    DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions
)

# Also expose the panel as a normal application-search result.
install(PROGRAMS org.flufflinux.update.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)
