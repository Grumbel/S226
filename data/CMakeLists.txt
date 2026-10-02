# Desktop/AppStream ID: the binary name, like most desktop apps, rather
# than a reverse-DNS name.
set(S226_APP_ID s226-hr)

install(FILES ${S226_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${S226_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES icons/${S226_APP_ID}.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${PROJECT_SOURCE_DIR}/man/s226-hr.1 ${PROJECT_SOURCE_DIR}/man/s226-cli.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# Fixed-size PNGs for desktops and launchers that do not use the SVG.
find_program(RSVG_CONVERT rsvg-convert)
if(RSVG_CONVERT)
  set(icon_pngs)
  foreach(size 16 24 32 48 64 128 256 512)
    set(png ${CMAKE_CURRENT_BINARY_DIR}/icons/${size}/${S226_APP_ID}.png)
    add_custom_command(
      OUTPUT ${png}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/icons/${size}
      COMMAND ${RSVG_CONVERT} -w ${size} -h ${size} -o ${png}
              ${CMAKE_CURRENT_SOURCE_DIR}/icons/${S226_APP_ID}.svg
      DEPENDS icons/${S226_APP_ID}.svg
      VERBATIM)
    list(APPEND icon_pngs ${png})
    install(FILES ${png} DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
  endforeach()
  add_custom_target(icons ALL DEPENDS ${icon_pngs})
else()
  message(STATUS "rsvg-convert not found: installing the scalable icon only")
endif()

if(BUILD_TESTING)
  find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
  if(DESKTOP_FILE_VALIDATE)
    add_test(NAME desktop-file
             COMMAND ${DESKTOP_FILE_VALIDATE} ${CMAKE_CURRENT_SOURCE_DIR}/${S226_APP_ID}.desktop)
  endif()
  find_program(APPSTREAMCLI appstreamcli)
  if(APPSTREAMCLI)
    add_test(NAME metainfo
             COMMAND ${APPSTREAMCLI} validate --no-net --pedantic --override=cid-desktopapp-is-not-rdns=info
                     ${CMAKE_CURRENT_SOURCE_DIR}/${S226_APP_ID}.metainfo.xml)
  endif()
  find_program(MANDOC mandoc)
  if(MANDOC)
    add_test(NAME manpages
             COMMAND ${MANDOC} -Tlint -Wwarning ${PROJECT_SOURCE_DIR}/man/s226-hr.1
                     ${PROJECT_SOURCE_DIR}/man/s226-cli.1)
  endif()
endif()
