add_executable(s226-cli main.cpp)
target_link_libraries(s226-cli PRIVATE s226ble)
target_compile_definitions(s226-cli PRIVATE S226_VERSION="${S226_VERSION}")
install(TARGETS s226-cli)
