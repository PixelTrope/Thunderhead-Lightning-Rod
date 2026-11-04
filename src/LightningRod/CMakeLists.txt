add_executable(LightningRod WIN32
    main.cpp
    SysInfo.cpp
    Lightning.cpp
)
target_link_libraries(LightningRod PRIVATE common iphlpapi comctl32)
# Windows subsystem (no console window at logon) with a plain main() entry point.
target_link_options(LightningRod PRIVATE /ENTRY:mainCRTStartup)
