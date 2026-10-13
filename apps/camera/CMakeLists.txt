cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(n900_camera)

# Sensor register lists are generated from a local Linux tree at build time
set(LINUX_DIR $ENV{HOME}/linux CACHE PATH "Linux source tree with the et8ek8 driver")
set(ET8EK8_MODES ${LINUX_DIR}/drivers/media/i2c/et8ek8/et8ek8_mode.c)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
  OUTPUT ${GEN_DIR}/et8ek8_regs.h
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_regs.py
          ${ET8EK8_MODES} ${GEN_DIR}/et8ek8_regs.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_regs.py ${ET8EK8_MODES}
)
add_custom_target(et8ek8_regs DEPENDS ${GEN_DIR}/et8ek8_regs.h)
add_dependencies(app et8ek8_regs)
target_include_directories(app PRIVATE ${GEN_DIR})
target_sources(app PRIVATE src/main.c)
