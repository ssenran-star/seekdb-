cmake_minimum_required(VERSION 3.16)
project(minivectordb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(minivectordb
  src/main.cpp
  src/parser.cpp
  src/storage.cpp
  src/engine.cpp
  src/hnsw.cpp
)
target_include_directories(minivectordb PRIVATE third_party)
target_compile_options(minivectordb PRIVATE -Wall -Wextra)
target_link_options(minivectordb PRIVATE -static-libstdc++ -static-libgcc)