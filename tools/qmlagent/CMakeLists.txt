# Copyright (C) 2026 Penk Chen <penkia@gmail.com>
# SPDX-License-Identifier: Apache-2.0

set(qmlagent_client_sources
    main.cpp
    qmlagentclioutput.cpp
    qmlagentclioutput.h
    qmlagentmcpoutput.cpp
    qmlagentmcpoutput.h
    qmlagentmcpprotocol.cpp
    qmlagentmcpprotocol.h
    qmlagentpaths.cpp
    qmlagentpaths.h
    qmlagentlauncher.cpp
    qmlagentlauncher.h
)

# The Qt lib dir is kept in the install rpath so the tools also run when
# installed outside the Qt prefix (the no-sudo install path).
get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)

function(add_qmlagent_client target)
    qt_add_executable(${target}
        ${qmlagent_client_sources}
    )

    target_link_libraries(${target} PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::QmlDebugPrivate
    )

    target_compile_definitions(${target} PRIVATE
        QMLAGENT_VERSION_STR="${PROJECT_VERSION}"
    )

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "@executable_path/../lib;${_qt_prefix}/lib"
        )
    elseif(UNIX)
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib;${_qt_prefix}/lib"
        )
    endif()
endfunction()

add_qmlagent_client(qmlagentctl)
add_qmlagent_client(qmlagent-mcp)

qt_add_executable(qmlagent-launcher
    launcher_main.cpp
    qmlagentpaths.cpp
    qmlagentpaths.h
)

target_link_libraries(qmlagent-launcher PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::QmlDebugPrivate
    Qt6::Quick
)

target_compile_definitions(qmlagent-launcher PRIVATE
    QT_QML_DEBUG
    QMLAGENT_VERSION_STR="${PROJECT_VERSION}"
)

if(APPLE)
    set_target_properties(qmlagent-launcher PROPERTIES
        INSTALL_RPATH "@executable_path/../lib;${_qt_prefix}/lib"
    )
elseif(UNIX)
    set_target_properties(qmlagent-launcher PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib;${_qt_prefix}/lib"
    )
endif()

if(NOT DEFINED QMLAGENT_TOOL_INSTALL_DIR)
    set(QMLAGENT_TOOL_INSTALL_DIR "${_qt_prefix}/bin")
endif()

install(TARGETS qmlagentctl qmlagent-mcp qmlagent-launcher
    RUNTIME DESTINATION "${QMLAGENT_TOOL_INSTALL_DIR}"
    BUNDLE DESTINATION "${QMLAGENT_TOOL_INSTALL_DIR}"
)
