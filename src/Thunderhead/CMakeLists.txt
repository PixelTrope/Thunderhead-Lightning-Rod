# Thunderhead runs on the Windows 10 host only; its file dialogs need Vista or later.
if(CMAKE_VS_PLATFORM_TOOLSET MATCHES "_xp$")
    message(STATUS "Thunderhead is not built with the XP toolset")
    return()
endif()

add_executable(Thunderhead WIN32
    main.cpp
    NodeStore.cpp
    Server.cpp
    Theme.cpp
    Thunderhead.rc
    Thunderhead.ico
)
target_link_libraries(Thunderhead PRIVATE common comctl32 uxtheme version shell32 ole32 mpr iphlpapi ws2_32)
# The default sound scheme, assets/sounds, is read from sounds\ beside the executable.
add_custom_command(TARGET Thunderhead POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/sounds $<TARGET_FILE_DIR:Thunderhead>/sounds)
