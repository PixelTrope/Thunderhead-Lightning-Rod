add_executable(Thunderhead WIN32
    main.cpp
    NodeStore.cpp
    Server.cpp
    Thunderhead.rc
)
target_link_libraries(Thunderhead PRIVATE common comctl32)
