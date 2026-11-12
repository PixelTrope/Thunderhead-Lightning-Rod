add_executable(LightningRod WIN32
    main.cpp
    SysInfo.cpp
    Lightning.cpp
    Thermal.cpp
    Audio.cpp
    TeamViewer.cpp
    Installed.cpp
    LightningRod.rc
    LightningRod.ico
)
target_link_libraries(LightningRod PRIVATE common iphlpapi comctl32 wbemuuid ole32 oleaut32 winmm version)
# The default sound scheme, assets/sounds, is read from sounds\ beside the executable;
# Thunderhead copies the same folder to every node with the agent update.
add_custom_command(TARGET LightningRod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/sounds $<TARGET_FILE_DIR:LightningRod>/sounds)
# Windows subsystem (no console window at logon) with a plain main() entry point.
target_link_options(LightningRod PRIVATE /ENTRY:mainCRTStartup)
