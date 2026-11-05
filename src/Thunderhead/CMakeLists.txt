# Thunderhead runs on the Windows 10 host only; its file dialogs need Vista or later.
if(CMAKE_VS_PLATFORM_TOOLSET MATCHES "_xp$")
    message(STATUS "Thunderhead is not built with the XP toolset")
    return()
endif()

add_executable(Thunderhead WIN32
    main.cpp
    NodeStore.cpp
    Server.cpp
    Thunderhead.rc
    Thunderhead.ico
)
target_link_libraries(Thunderhead PRIVATE common comctl32 version shell32 ole32)
