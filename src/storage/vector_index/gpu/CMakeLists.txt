# Standalone build for the hnsw_gpu module and its benchmark tool.
#
# The module is deliberately dependency free: a plain C++17 toolchain is enough to build
# and run the CPU engines, which is what makes the build path testable on any machine.
# The hipVS backend is opt-in and only this option needs ROCm:
#
#   cmake -S src/storage/vector_index/gpu -B build/gpu -DSEEKDB_ENABLE_HIPVS=ON \
#         -DHIPVS_ROOT=/opt/rocm
#
# Inside the seekdb build the same sources are compiled by src/storage/CMakeLists.txt,
# this file exists so the module can be validated in isolation.
cmake_minimum_required(VERSION 3.16)
project(seekdb_hnsw_gpu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SEEKDB_ENABLE_HIPVS "Build the hipVS (ROCm/cuVS API) k-NN backend" OFF)
set(HIPVS_ROOT "" CACHE PATH "ROCm / hipVS installation prefix")
option(SEEKDB_BUILD_HNSW_GPU_BENCH "Build the standalone build benchmark" ON)
option(SEEKDB_BUILD_HNSW_GPU_TESTS "Build the unit tests" ON)

add_library(seekdb_hnsw_gpu STATIC
  ob_hnsw_knn_graph.cpp
  ob_hnsw_knn_graph_cpu.cpp
  ob_hnsw_knn_graph_hipvs.cpp
  ob_hnsw_layered_graph.cpp
  ob_hnsw_graph_file.cpp
  ob_hnsw_gpu_builder.cpp)

target_include_directories(seekdb_hnsw_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(seekdb_hnsw_gpu PUBLIC Threads::Threads)

if(SEEKDB_ENABLE_HIPVS)
  if(NOT HIPVS_ROOT)
    message(FATAL_ERROR "SEEKDB_ENABLE_HIPVS requires -DHIPVS_ROOT=<prefix>")
  endif()
  find_package(hip REQUIRED)
  # hipVS keeps the cuVS CMake package name; both are consumed through cuvs::cuvs.
  find_package(cuvs REQUIRED CONFIG HINTS ${HIPVS_ROOT}/lib/cmake ${HIPVS_ROOT}/lib64/cmake)
  target_compile_definitions(seekdb_hnsw_gpu PUBLIC SEEKDB_ENABLE_HIPVS=1)
  target_link_libraries(seekdb_hnsw_gpu PUBLIC cuvs::cuvs hip::host)
  message(STATUS "hnsw_gpu: hipVS backend enabled (${HIPVS_ROOT})")
else()
  message(STATUS "hnsw_gpu: hipVS backend disabled, only CPU engines are available")
endif()

if(SEEKDB_BUILD_HNSW_GPU_BENCH)
  add_executable(hnsw_gpu_bench ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/hipvs_hnsw_bench/main.cpp)
  target_link_libraries(hnsw_gpu_bench PRIVATE seekdb_hnsw_gpu)
endif()

if(SEEKDB_BUILD_HNSW_GPU_TESTS)
  enable_testing()
  add_executable(test_hnsw_gpu ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/hipvs_hnsw_bench/test_hnsw_gpu.cpp)
  target_link_libraries(test_hnsw_gpu PRIVATE seekdb_hnsw_gpu)
  add_test(NAME hnsw_gpu_unit COMMAND test_hnsw_gpu)
endif()
