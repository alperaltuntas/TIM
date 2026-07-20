# TIM C++ unit tests
#
# Run from the TIM root, with parallelio (PIO) and AMReX installs on CMAKE_PREFIX_PATH:
#   ./build.sh --tests                              # build + install + ctest
# or with raw CMake:
#   cmake -S . -B build -DTIM_ADD_TEST_TARGETS=ON \
#         -DCMAKE_PREFIX_PATH="<amrex-install>;$NCAR_ROOT_PARALLELIO"
#   cmake --build build --target test_tim_smoke
#   ctest --test-dir build -L unit --output-on-failure

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  if(TIM_FETCH_DEPS)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
  else()
    message(FATAL_ERROR
      "GTest not found. Install GoogleTest or set CMAKE_PREFIX_PATH "
      "to include its install location, or configure with "
      "-DTIM_FETCH_DEPS=ON to download it automatically.")
  endif()
endif()
enable_testing()

if(AMReX_GPU_BACKEND STREQUAL "CUDA")
  enable_language(CUDA)
endif()

include(GoogleTest)

# One TIM gtest executable: always links tim + GTest and the shared main;
# LINK adds per-target deps (e.g. PIO for the smoke test).
function(tim_add_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LINK" ${ARGN})
  add_executable(${name} test_tim_main.cpp ${T_SOURCES})
  target_link_libraries(${name} GTest::gtest tim ${T_LINK})
  if(AMReX_GPU_BACKEND STREQUAL "CUDA")
    set_source_files_properties(test_tim_main.cpp ${T_SOURCES}
      PROPERTIES LANGUAGE CUDA)
  endif()
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS "unit")
endfunction()

tim_add_test(test_tim_smoke SOURCES smoke/test_smoke.cpp LINK PIO::PIO_C)
tim_add_test(test_tim_core SOURCES test_time.cpp test_decomp.cpp)
