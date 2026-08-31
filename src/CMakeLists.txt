kcmutils_add_qml_kcm(kcm_fluffupdates)

target_sources(kcm_fluffupdates PRIVATE
    fluffupdates.cpp
    fluffupdates.h
    securitydiagnostics.cpp
    securitydiagnostics.h
    signingkeyrecovery.cpp
    signingkeyrecovery.h
    kcm_fluffupdates.json
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
)

target_compile_definitions(kcm_fluffupdates PRIVATE
    TRANSLATION_DOMAIN=\"kcm_fluffupdates\"
    FLUFFLINUX_LOCALE_DIR=\"${KDE_INSTALL_FULL_LOCALEDIR}\"
    PROJECT_VERSION=\"${PROJECT_VERSION}\"
)

add_executable(flufflinux-update-helper updatehelper.cpp)
target_sources(flufflinux-update-helper PRIVATE
    signingkeyrecovery.cpp
    signingkeyrecovery.h
)
target_link_libraries(flufflinux-update-helper PRIVATE Qt6::Core Qt6::Network)
install(TARGETS flufflinux-update-helper
    RUNTIME DESTINATION lib/flufflinux-update
)

add_executable(flufflinux-update-worker updateworker.cpp)
target_sources(flufflinux-update-worker PRIVATE
    signingkeyrecovery.cpp
    signingkeyrecovery.h
)
target_link_libraries(flufflinux-update-worker PRIVATE Qt6::Core Qt6::Network)
install(TARGETS flufflinux-update-worker
    RUNTIME DESTINATION lib/flufflinux-update
)
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
