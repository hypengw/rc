cmake_minimum_required(VERSION 3.28)
project(
  rc
  VERSION 1.0
  DESCRIPTION "rust like Rc/Arc for c++"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RC_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

add_library(rc)
add_library(rstd::rc ALIAS rc)
target_sources(
  rc
  PUBLIC FILE_SET
         all
         TYPE
         CXX_MODULES
         BASE_DIRS
         src
         FILES
         src/arc.cppm
         src/rc.cppm
         src/mod.cppm)

# Add installation rules
if(PROJECT_IS_TOP_LEVEL)
  include(GNUInstallDirs)
  install(
    TARGETS rc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# Testing configuration
if(RC_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(test)
endif()
