file(GLOB HEADERS "*.h")
file(GLOB SRC "*.cpp")
find_package (Threads REQUIRED)

add_library(wzrpc STATIC ${HEADERS} ${SRC})
set_property(TARGET wzrpc PROPERTY FOLDER "lib")

target_include_directories(wzrpc PRIVATE "${CMAKE_SOURCE_DIR}/lib/netplay")
target_include_directories(wzrpc PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/json")
target_link_libraries(wzrpc PRIVATE netplay framework)
target_link_libraries(wzrpc PRIVATE Threads::Threads )
