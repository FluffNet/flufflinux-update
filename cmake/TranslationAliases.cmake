# Gettext and desktop environments use country/modifier names in addition to
# the shared language/script codes. Compile aliases from the same authored PO
# file so a regional lookup cannot select a different script.
file(READ "${CMAKE_SOURCE_DIR}/po/languages.json" flu_language_inventory)
string(JSON flu_alias_count LENGTH "${flu_language_inventory}" aliases)
find_program(FLU_MSGFMT_EXECUTABLE msgfmt REQUIRED)
set(flu_alias_outputs)
if(flu_alias_count GREATER 0)
    math(EXPR flu_alias_last "${flu_alias_count} - 1")
    foreach(index RANGE 0 ${flu_alias_last})
        string(JSON alias MEMBER "${flu_language_inventory}" aliases ${index})
        string(JSON language GET "${flu_language_inventory}" aliases "${alias}")
        if(alias STREQUAL language)
            continue()
        endif()
        set(source "${CMAKE_SOURCE_DIR}/po/${language}/kcm_fluffupdates.po")
        set(output "${CMAKE_CURRENT_BINARY_DIR}/locale-aliases/${alias}/kcm_fluffupdates.mo")
        add_custom_command(OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/locale-aliases/${alias}"
            COMMAND "${FLU_MSGFMT_EXECUTABLE}" --check-format -o "${output}" "${source}"
            DEPENDS "${source}" "${CMAKE_SOURCE_DIR}/po/languages.json"
            VERBATIM)
        list(APPEND flu_alias_outputs "${output}")
        install(FILES "${output}"
            DESTINATION "${KDE_INSTALL_LOCALEDIR}/${alias}/LC_MESSAGES")
    endforeach()
endif()
add_custom_target(flu-locale-aliases ALL DEPENDS ${flu_alias_outputs})
