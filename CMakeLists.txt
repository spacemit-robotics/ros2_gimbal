cmake_minimum_required(VERSION 3.8)
project(control_gimbal_node)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_msgs REQUIRED)
find_package(Threads REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/GimbalEuler.msg"
  "msg/GimbalState.msg"
  "srv/SetGimbalLimits.srv"
  "srv/SetGimbalMode.srv"
  "srv/SetGimbalTarget.srv"
  "srv/SetGimbalZoom.srv"
  DEPENDENCIES std_msgs
)

rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")

set(GIMBAL_COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../components/control/gimbal)
set(GIMBAL_INCLUDE_DIR "")
set(GIMBAL_LIB "")
set(GIMBAL_TARGET "")
set(GIMBAL_OBJECTS "")

find_path(GIMBAL_INCLUDE_DIR gimbal.h PATH_SUFFIXES include)
find_library(GIMBAL_LIB gimbal)

if((NOT GIMBAL_INCLUDE_DIR OR NOT GIMBAL_LIB) AND DEFINED ENV{CMAKE_PREFIX_PATH})
  string(REPLACE ":" ";" GIMBAL_PREFIX_PATHS "$ENV{CMAKE_PREFIX_PATH}")
  find_path(GIMBAL_INCLUDE_DIR gimbal.h PATHS ${GIMBAL_PREFIX_PATHS} PATH_SUFFIXES include)
  find_library(GIMBAL_LIB gimbal PATHS ${GIMBAL_PREFIX_PATHS} PATH_SUFFIXES lib)
endif()

if(GIMBAL_INCLUDE_DIR AND GIMBAL_LIB)
  set(GIMBAL_TARGET ${GIMBAL_LIB})
elseif(EXISTS ${GIMBAL_COMPONENT_DIR}/include/gimbal.h)
  add_library(gimbal_component_fallback OBJECT
    ${GIMBAL_COMPONENT_DIR}/src/gimbal.c
    ${GIMBAL_COMPONENT_DIR}/src/drivers/drv_udp_tz0xxx.c
    ${GIMBAL_COMPONENT_DIR}/src/drivers/drv_udp_c12.c
  )
  target_include_directories(gimbal_component_fallback PUBLIC
    ${GIMBAL_COMPONENT_DIR}/include
    ${GIMBAL_COMPONENT_DIR}/src
  )
  set(GIMBAL_OBJECTS $<TARGET_OBJECTS:gimbal_component_fallback>)
  set(GIMBAL_INCLUDE_DIR ${GIMBAL_COMPONENT_DIR}/include)
else()
  message(FATAL_ERROR "gimbal sources not found under components/control/gimbal.")
endif()

add_executable(gimbal_server_node
  src/gimbal_server_node.cpp
  ${GIMBAL_OBJECTS}
)
ament_target_dependencies(gimbal_server_node rclcpp std_msgs)
target_include_directories(gimbal_server_node PRIVATE ${GIMBAL_INCLUDE_DIR})
target_link_libraries(gimbal_server_node Threads::Threads m ${GIMBAL_TARGET} "${cpp_typesupport_target}")

install(TARGETS gimbal_server_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_export_dependencies(rosidl_default_runtime std_msgs)
ament_package()
