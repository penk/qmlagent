# Copyright (C) 2026 Penk Chen <penkia@gmail.com>
# SPDX-License-Identifier: Apache-2.0

qt_add_plugin(QQmlAgentServiceFactoryPlugin
    OUTPUT_NAME qmldbg_agent
    CLASS_NAME QQmlAgentServiceFactory
)

target_sources(QQmlAgentServiceFactoryPlugin PRIVATE
    qqmlagentactionability.cpp qqmlagentactionability_p.h
    qqmlagentdiagnostics.cpp qqmlagentdiagnostics_p.h
    qqmlagentinput.cpp qqmlagentinput_p.h
    qqmlagentinputdriver.cpp qqmlagentinputdriver_p.h
    qqmlagentlogbuffer.cpp qqmlagentlogbuffer_p.h
    qqmlagentlogcollector.cpp qqmlagentlogcollector_p.h
    qqmlagentprotocol.cpp qqmlagentprotocol_p.h
    qqmlagentrender.cpp qqmlagentrender_p.h
    qqmlagentruntime.cpp qqmlagentruntime_p.h
    qqmlagentservice.cpp qqmlagentservice_p.h
    qqmlagentservicefactory.cpp qqmlagentservicefactory_p.h
    qqmlagentsourceresolver.cpp qqmlagentsourceresolver_p.h
    qqmlagentuitree.cpp qqmlagentuitree_p.h
)

target_link_libraries(QQmlAgentServiceFactoryPlugin PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Qml
    Qt6::QmlPrivate
    Qt6::Quick
    Qt6::QuickPrivate
    Qt6::QuickTemplates2Private
)

if(TARGET Qt6::Quick3D)
    target_compile_definitions(QQmlAgentServiceFactoryPlugin PRIVATE QMLAGENT_HAS_QUICK3D)
    target_link_libraries(QQmlAgentServiceFactoryPlugin PRIVATE Qt6::Quick3D)
endif()

set(qmlagent_build_plugin_dir "${CMAKE_BINARY_DIR}/plugins/qmltooling")
set_target_properties(QQmlAgentServiceFactoryPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${qmlagent_build_plugin_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${qmlagent_build_plugin_dir}"
)

if(NOT DEFINED QMLAGENT_PLUGIN_INSTALL_DIR)
    get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    set(QMLAGENT_PLUGIN_INSTALL_DIR "${_qt_prefix}/plugins/qmltooling")
endif()

install(TARGETS QQmlAgentServiceFactoryPlugin
    LIBRARY DESTINATION "${QMLAGENT_PLUGIN_INSTALL_DIR}"
    RUNTIME DESTINATION "${QMLAGENT_PLUGIN_INSTALL_DIR}"
    BUNDLE DESTINATION "${QMLAGENT_PLUGIN_INSTALL_DIR}"
)
