# SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
# SPDX-License-Identifier: BSD-2-Clause

add_definitions(-DTRANSLATION_DOMAIN=\"krdpserver\")

add_executable(krdpserver)

target_sources(krdpserver PRIVATE main.cpp SessionController.cpp VirtualDisplayManager.cpp LocalPresenceMonitor.cpp)

kconfig_target_kcfg_file(krdpserver FILE krdpserversettings.kcfg CLASS_NAME ServerConfig SINGLETON)

target_include_directories(krdpserver PRIVATE ${CMAKE_BINARY_DIR})

target_link_libraries(krdpserver PRIVATE Qt6::Gui KF6::CoreAddons KF6::ConfigGui KF6::DBusAddons KF6::Crash KRdp qt6keychain KF6::StatusNotifierItem KF6::I18n KF6::IdleTime KF6::Screen Qt6::DBus PkgConfig::Libxcvt)

if (BUILD_PLASMA_SESSION)
    target_compile_definitions(krdpserver PRIVATE -DWITH_PLASMA_SESSION)
endif()

configure_file(org.kde.krdpserver.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpserver.desktop @ONLY)
configure_file(app-org.kde.krdpserver.service.in ${CMAKE_CURRENT_BINARY_DIR}/app-org.kde.krdpserver.service @ONLY)
configure_file(org.kde.krdpserver.returndisplays.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpserver.returndisplays.desktop @ONLY)

install(TARGETS krdpserver DESTINATION ${KDE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpserver.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/app-org.kde.krdpserver.service DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
# Global shortcut to turn the physical monitors back on, runs as a separate
# process so it works even if krdpserver does not respond.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpserver.returndisplays.desktop DESTINATION ${KDE_INSTALL_DATADIR}/kglobalaccel)
install(FILES 00-krdp.preset DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/user-preset)
