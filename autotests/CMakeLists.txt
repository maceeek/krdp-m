# SPDX-FileCopyrightText: 2026 Nicolas Fella <nicolas.fella@gmx.de>
# SPDX-License-Identifier: BSD-2-Clause

add_test(NAME kcm_smoketest COMMAND kcmshell6 --smoke-test kcm_krdpserver)
set_tests_properties(kcm_smoketest PROPERTIES
    ENVIRONMENT_MODIFICATION QT_PLUGIN_PATH=path_list_prepend:${CMAKE_BINARY_DIR}/bin
)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED Test)

add_executable(virtualdisplaymanagertest virtualdisplaymanagertest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualDisplayManager.cpp)
target_include_directories(virtualdisplaymanagertest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(virtualdisplaymanagertest PRIVATE Qt6::Test KRdp KF6::Screen KF6::ConfigCore PkgConfig::Libxcvt)
add_test(NAME virtualdisplaymanagertest COMMAND virtualdisplaymanagertest)

add_executable(monitorlayouttest monitorlayouttest.cpp)
target_link_libraries(monitorlayouttest PRIVATE Qt6::Test KRdp)
add_test(NAME monitorlayouttest COMMAND monitorlayouttest)

add_executable(nvencencodertest nvencencodertest.cpp)
target_link_libraries(nvencencodertest PRIVATE Qt6::Test KRdp PkgConfig::FFmpeg)
add_test(NAME nvencencodertest COMMAND nvencencodertest)

add_executable(av1encodertest av1encodertest.cpp)
target_link_libraries(av1encodertest PRIVATE Qt6::Test KRdp)
add_test(NAME av1encodertest COMMAND av1encodertest)

add_executable(gfxcapabilitiestest gfxcapabilitiestest.cpp)
target_link_libraries(gfxcapabilitiestest PRIVATE Qt6::Test KRdp)
add_test(NAME gfxcapabilitiestest COMMAND gfxcapabilitiestest)

add_executable(networkdetectionsequencetest networkdetectionsequencetest.cpp)
target_include_directories(networkdetectionsequencetest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(networkdetectionsequencetest PRIVATE Qt6::Test)
add_test(NAME networkdetectionsequencetest COMMAND networkdetectionsequencetest)

add_executable(videopacingtest videopacingtest.cpp)
target_include_directories(videopacingtest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(videopacingtest PRIVATE Qt6::Test)
add_test(NAME videopacingtest COMMAND videopacingtest)

add_executable(videostreamsizetest videostreamsizetest.cpp)
target_link_libraries(videostreamsizetest PRIVATE Qt6::Test KRdp)
# The test intercepts KPipeWire's size setters to verify negotiation requests
# without connecting to the user's PipeWire daemon or requiring a GPU.
set_target_properties(videostreamsizetest PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME videostreamsizetest COMMAND videostreamsizetest)
