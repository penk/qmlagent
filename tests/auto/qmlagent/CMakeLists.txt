# Copyright (C) 2026 Penk Chen <penkia@gmail.com>
# SPDX-License-Identifier: Apache-2.0

qt_add_executable(tst_qmlagentlogbuffer
    tst_qmlagentlogbuffer.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentlogbuffer.cpp
)

target_include_directories(tst_qmlagentlogbuffer PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent
)

target_link_libraries(tst_qmlagentlogbuffer PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME qmlagentlogbuffer
    COMMAND tst_qmlagentlogbuffer
)

qt_add_executable(tst_qmlagentprotocol
    tst_qmlagentprotocol.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentprotocol.cpp
    ${PROJECT_SOURCE_DIR}/tools/qmlagent/qmlagentmcpprotocol.cpp
)

target_include_directories(tst_qmlagentprotocol PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent
    ${PROJECT_SOURCE_DIR}/tools/qmlagent
)

target_compile_definitions(tst_qmlagentprotocol PRIVATE
    QMLAGENT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(tst_qmlagentprotocol PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME qmlagentprotocol
    COMMAND tst_qmlagentprotocol
)

qt_add_executable(tst_qmlagentinput
    tst_qmlagentinput.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentactionability.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentinput.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentinputdriver.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentjsonutils.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentdiagnostics.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentlogbuffer.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentlogcollector.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentruntime.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentuitree.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent/qqmlagentsourceresolver.cpp
)

target_include_directories(tst_qmlagentinput PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugins/qmltooling/qmldbg_agent
)

target_link_libraries(tst_qmlagentinput PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::QmlPrivate
    Qt6::QmlDebugPrivate
    Qt6::Quick
    Qt6::QuickPrivate
    Qt6::QuickTemplates2Private
    Qt6::Test
)

if(TARGET Qt6::Quick3D)
    target_compile_definitions(tst_qmlagentinput PRIVATE QMLAGENT_HAS_QUICK3D)
    target_link_libraries(tst_qmlagentinput PRIVATE Qt6::Quick3D)
endif()

add_test(NAME qmlagentinput
    COMMAND tst_qmlagentinput
)
