# Copyright (C) 2026 Penk Chen <penkia@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)

project(QmlAgent
    VERSION 0.1.0
    LANGUAGES CXX
)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Test QmlPrivate QuickPrivate QmlDebugPrivate QuickTemplates2Private)
find_package(Qt6 6.11 QUIET COMPONENTS Quick3D)

qt_standard_project_setup()
if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
endif()

enable_testing()

add_subdirectory(src/plugins/qmltooling/qmldbg_agent)
add_subdirectory(tests/auto/qmlagent)
add_subdirectory(tests/integration/qmlagent)
add_subdirectory(tools/qmlagent)
