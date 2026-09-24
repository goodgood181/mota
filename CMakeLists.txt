cmake_minimum_required(VERSION 3.10)
project(lunhui_tower CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_executable(mota
  src/main.cpp
  src/game.cpp
)
if(WIN32)
  target_sources(mota PRIVATE src/ui.cpp)
endif()

# 数据目录随构建输出复制
add_custom_command(TARGET mota POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/data
          $<TARGET_FILE_DIR:mota>/data
  COMMENT "Copying data/ next to the executable"
)