cmake_minimum_required(VERSION 3.16)
project(crosskobo CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CROSSKOBO_SIM "Build the host simulator front-end as well" ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -fno-omit-frame-pointer")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------- third party
add_library(ck_thirdparty STATIC
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
  third_party/stb_impl.c
)
target_include_directories(ck_thirdparty PUBLIC third_party third_party/miniz third_party/stb)
target_compile_definitions(ck_thirdparty PUBLIC
  MINIZ_NO_TIME
  STBI_ONLY_PNG STBI_ONLY_JPEG STBI_ONLY_GIF STBI_ONLY_BMP
  STBI_NO_STDIO STBI_NO_FAILURE_STRINGS)
target_compile_options(ck_thirdparty PRIVATE -w)

# ---------------------------------------------------------------------- core
file(GLOB CK_SOURCES
  src/core/*.cpp
  src/gfx/*.cpp
  src/platform/*.cpp
  src/ui/*.cpp
  src/epub/*.cpp
  src/net/*.cpp
  src/reader/*.cpp
  src/notes/*.cpp
  src/library/*.cpp
  src/app/*.cpp
)
add_library(ck_core STATIC ${CK_SOURCES})
target_include_directories(ck_core PUBLIC src third_party third_party/stb third_party/miniz)
target_link_libraries(ck_core PUBLIC ck_thirdparty)

add_executable(crosskobo src/main.cpp)
target_link_libraries(crosskobo PRIVATE ck_core)

# The device build embeds nothing; assets are installed alongside the binary.
if(CROSSKOBO_SIM)
  add_executable(crosskobo-sim src/sim/sim_main.cpp)
  target_link_libraries(crosskobo-sim PRIVATE ck_core)
endif()

enable_testing()
file(GLOB CK_TESTS tests/test_*.cpp)
foreach(t ${CK_TESTS})
  get_filename_component(name ${t} NAME_WE)
  add_executable(${name} ${t})
  target_link_libraries(${name} PRIVATE ck_core)
  # Tests need the bundled fonts; pass their location explicitly so
  # they can run from any working directory.
  add_test(NAME ${name} COMMAND ${name} ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts)
  set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
