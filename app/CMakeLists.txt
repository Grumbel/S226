find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia)
qt_standard_project_setup()

qt_add_executable(s226-hr
  main.cpp
  main_window.cpp
  bpm_view.cpp
  metronome.cpp
  watch_bridge.cpp
  measurement_log.cpp
  trend_graph.cpp
)
# Window icon when the icon theme does not provide it (uninstalled runs).
qt_add_resources(s226-hr icons
  PREFIX /
  BASE ../data/icons
  FILES ../data/icons/s226-hr.svg
)
target_link_libraries(s226-hr PRIVATE s226ble Qt6::Widgets Qt6::Multimedia)
target_compile_definitions(s226-hr PRIVATE S226_VERSION="${S226_VERSION}")
target_compile_options(s226-hr PRIVATE -Wall -Wextra)

install(TARGETS s226-hr)
