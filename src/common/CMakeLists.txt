add_library(common STATIC
    Protocol.cpp
    Net.cpp
    Log.cpp
    StrUtil.cpp
)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(common PUBLIC ws2_32)
