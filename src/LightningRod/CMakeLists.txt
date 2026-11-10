add_executable(LightningRod WIN32
    main.cpp
    SysInfo.cpp
    Lightning.cpp
    Thermal.cpp
    Audio.cpp
    TeamViewer.cpp
    LightningRod.rc
    LightningRod.ico
)
target_link_libraries(LightningRod PRIVATE common iphlpapi comctl32 wbemuuid ole32 oleaut32 winmm)
# Windows subsystem (no console window at logon) with a plain main() entry point.
target_link_options(LightningRod PRIVATE /ENTRY:mainCRTStartup)
