# This file is part of GammaRay, the Qt application inspection and manipulation tool.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(gammaray_mcp_srcs
    main.cpp
    mcpserver.cpp
    mcpserver.h
    ../plugins/widgetinspector/widgetinspectorinterface.cpp
    ../plugins/widgetinspector/widgetinspectorinterface.h
)

add_executable(gammaray-mcp ${gammaray_mcp_srcs})
target_compile_features(gammaray-mcp PRIVATE ${GAMMARAY_REQUIRED_CXX_FEATURES})
target_compile_definitions(gammaray-mcp PRIVATE GAMMARAY_MCP_VERSION="0.2.0")
target_include_directories(gammaray-mcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/widgetinspector)
target_link_libraries(
    gammaray-mcp
    PRIVATE gammaray_client
            gammaray_launcher
            gammaray_common
            gammaray_common_internal
            gammaray_quickinspector_shared
            gammaray_ui
            Qt::Core
            Qt::Gui
            Qt::Network
            Qt::Quick
            Qt::Widgets
)
set_target_properties(
    gammaray-mcp
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}"
)
gammaray_set_rpath(gammaray-mcp ${BIN_INSTALL_DIR})

install(TARGETS gammaray-mcp DESTINATION ${BIN_INSTALL_DIR})

# A deterministic Qt process used by the MCP end-to-end smoke test. It is not
# part of the default build or installation.
add_executable(gammaray-mcp-test-target EXCLUDE_FROM_ALL testtarget.cpp)
target_link_libraries(gammaray-mcp-test-target PRIVATE Qt::Core)
set_target_properties(
    gammaray-mcp-test-target
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}"
)

# A visible QWidget target used by the screenshot integration smoke test.  It
# deliberately has stable object names and visually distinct child widgets so
# the test can prove both widget and containing-window capture paths.
add_executable(gammaray-mcp-gui-test-target EXCLUDE_FROM_ALL guitesttarget.cpp)
target_link_libraries(gammaray-mcp-gui-test-target PRIVATE Qt::Core Qt::Gui Qt::Widgets)
set_target_properties(
    gammaray-mcp-gui-test-target
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}"
)
