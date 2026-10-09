find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
qt_standard_project_setup()

qt_add_executable(s226-hr
  main.cpp
  main_window.cpp
  bpm_view.cpp
  metronome.cpp
  watch_bridge.cpp
  measurement_log.cpp
  trend_graph.cpp
  settings_tab.cpp
  alarms_tab.cpp
  notify_tab.cpp
  history_tab.cpp
  workouts_tab.cpp
  sleep_tab.cpp
  mpris_controller.cpp
  notification_forwarder.cpp
)
qt_add_resources(s226-hr icons
  PREFIX /
  BASE ../data/icons
  FILES
    ../data/icons/s226-hr.svg
    ../data/icons/menu-heart-rate.svg
    ../data/icons/menu-blood-pressure.svg
    ../data/icons/menu-metronome.svg
    ../data/icons/menu-connect.svg
    ../data/icons/menu-disconnect.svg
)
target_link_libraries(s226-hr PRIVATE s226ble Qt6::Widgets Qt6::Multimedia Qt6::DBus PkgConfig::DBUS)
target_compile_definitions(s226-hr PRIVATE S226_VERSION="${S226_VERSION}")
target_compile_options(s226-hr PRIVATE -Wall -Wextra)

install(TARGETS s226-hr)
