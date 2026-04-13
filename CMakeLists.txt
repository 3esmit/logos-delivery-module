cmake_minimum_required(VERSION 3.14)
project(LogosDeliveryModulePlugin LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include the Logos Module CMake helper
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Define the module
logos_module(
    NAME delivery_module
    SOURCES
        src/delivery_module_interface.h
        src/delivery_module_plugin.h
        src/delivery_module_plugin.cpp
        src/api_call_handler.h
        src/QExpected.h
    EXTERNAL_LIBS
        logosdelivery
    INCLUDE_DIRS
        lib
)

include(CTest)

if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

    add_executable(delivery_module_plugin_integration_test
        test/test_create_node_integration.cpp
        src/delivery_module_plugin.cpp
        src/delivery_module_plugin.h
        src/delivery_module_interface.h
        src/api_call_handler.h
    )

    target_include_directories(delivery_module_plugin_integration_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    if(EXISTS "${LOGOS_DELIVERY_ROOT}/include/liblogosdelivery.h")
        target_include_directories(delivery_module_plugin_integration_test PRIVATE ${LOGOS_DELIVERY_ROOT}/include)
    elseif(EXISTS "${LOGOS_DELIVERY_ROOT}/liblogosdelivery/liblogosdelivery.h")
        target_include_directories(delivery_module_plugin_integration_test PRIVATE ${LOGOS_DELIVERY_ROOT}/liblogosdelivery)
    endif()

    if(_liblogos_is_source)
        target_include_directories(delivery_module_plugin_integration_test PRIVATE ${LOGOS_LIBLOGOS_ROOT})
    else()
        target_include_directories(delivery_module_plugin_integration_test PRIVATE ${LOGOS_LIBLOGOS_ROOT}/include)
    endif()

    if(_cpp_sdk_is_source)
        target_include_directories(delivery_module_plugin_integration_test PRIVATE 
            ${LOGOS_CPP_SDK_ROOT}/cpp
            ${LOGOS_CPP_SDK_ROOT}/cpp/generated
        )
    else()
        target_include_directories(delivery_module_plugin_integration_test PRIVATE 
            ${LOGOS_CPP_SDK_ROOT}/include
            ${LOGOS_CPP_SDK_ROOT}/include/cpp
            ${LOGOS_CPP_SDK_ROOT}/include/core
        )
    endif()

    target_link_libraries(delivery_module_plugin_integration_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::RemoteObjects
        Qt${QT_VERSION_MAJOR}::Test
    )

    if(NOT _cpp_sdk_is_source)
        target_link_libraries(delivery_module_plugin_integration_test PRIVATE ${LOGOS_SDK_LIB})
    else()
        target_sources(delivery_module_plugin_integration_test PRIVATE
            ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.cpp
            ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_client.cpp
            ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_consumer.cpp
            ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_provider.cpp
            ${LOGOS_CPP_SDK_ROOT}/cpp/token_manager.cpp
            ${LOGOS_CPP_SDK_ROOT}/cpp/module_proxy.cpp
        )
    endif()

    if(LIBLOGOSDELIVERY_PATH)
        target_link_libraries(delivery_module_plugin_integration_test PRIVATE ${LIBLOGOSDELIVERY_PATH})

        if(APPLE)
            add_custom_command(TARGET delivery_module_plugin_integration_test POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    -DTEST_BIN=$<TARGET_FILE:delivery_module_plugin_integration_test>
                    -DREAL_LIB=${LIBLOGOSDELIVERY_PATH}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fix_liblogosdelivery_id.cmake
                COMMENT "Rewriting liblogosdelivery install name for integration test runtime"
            )
        endif()
    endif()

    add_test(NAME delivery_module_plugin_integration_test COMMAND delivery_module_plugin_integration_test)
endif()
