# Unit tests for HEVC reference picture management (H.265 clause 8.3 / 8.5.3).
#
# These are white-box tests: they exercise the internal decoder_context /
# decoded_picture_buffer / ref_pic_set machinery directly, so they compile the
# library sources into a test-only static library instead of linking the
# installed shared object (whose non-API symbols are hidden in optimized
# builds).

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  message(STATUS "GTest not found - skipping reference picture management tests")
  return()
endif()

# Library sources needed by the reference-management tests. SIMD kernels are
# intentionally excluded: the scalar fallback function table fully initializes
# decoder_context and keeps the test build independent of the target arch.
set(refpic_test_libde265_sources
  ${PROJECT_SOURCE_DIR}/libde265/alloc_pool.cc
  ${PROJECT_SOURCE_DIR}/libde265/bitstream.cc
  ${PROJECT_SOURCE_DIR}/libde265/cabac.cc
  ${PROJECT_SOURCE_DIR}/libde265/contextmodel.cc
  ${PROJECT_SOURCE_DIR}/libde265/de265.cc
  ${PROJECT_SOURCE_DIR}/libde265/deblock.cc
  ${PROJECT_SOURCE_DIR}/libde265/decctx.cc
  ${PROJECT_SOURCE_DIR}/libde265/dpb.cc
  ${PROJECT_SOURCE_DIR}/libde265/fallback.cc
  ${PROJECT_SOURCE_DIR}/libde265/fallback-dct.cc
  ${PROJECT_SOURCE_DIR}/libde265/fallback-deblk.cc
  ${PROJECT_SOURCE_DIR}/libde265/fallback-intrapred.cc
  ${PROJECT_SOURCE_DIR}/libde265/fallback-motion.cc
  ${PROJECT_SOURCE_DIR}/libde265/image-io.cc
  ${PROJECT_SOURCE_DIR}/libde265/image.cc
  ${PROJECT_SOURCE_DIR}/libde265/intrapred.cc
  ${PROJECT_SOURCE_DIR}/libde265/md5.cc
  ${PROJECT_SOURCE_DIR}/libde265/motion.cc
  ${PROJECT_SOURCE_DIR}/libde265/nal-parser.cc
  ${PROJECT_SOURCE_DIR}/libde265/nal.cc
  ${PROJECT_SOURCE_DIR}/libde265/pps.cc
  ${PROJECT_SOURCE_DIR}/libde265/quality.cc
  ${PROJECT_SOURCE_DIR}/libde265/refpic.cc
  ${PROJECT_SOURCE_DIR}/libde265/sao.cc
  ${PROJECT_SOURCE_DIR}/libde265/scan.cc
  ${PROJECT_SOURCE_DIR}/libde265/sei.cc
  ${PROJECT_SOURCE_DIR}/libde265/slice.cc
  ${PROJECT_SOURCE_DIR}/libde265/sps.cc
  ${PROJECT_SOURCE_DIR}/libde265/threads.cc
  ${PROJECT_SOURCE_DIR}/libde265/transform.cc
  ${PROJECT_SOURCE_DIR}/libde265/util.cc
  ${PROJECT_SOURCE_DIR}/libde265/visualize.cc
  ${PROJECT_SOURCE_DIR}/libde265/vps.cc
  ${PROJECT_SOURCE_DIR}/libde265/vui.cc
)

# The tests directory is prepended (BEFORE) so the test-only config.h below is
# picked up instead of the generated one in the top-level build directory;
# that keeps the SIMD feature macros undefined and avoids unresolved arch
# symbols, since the SIMD translation units are not part of this library.
add_library(de265-refpic-test-support STATIC ${refpic_test_libde265_sources})
target_include_directories(de265-refpic-test-support BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/libde265
)
target_compile_definitions(de265-refpic-test-support PRIVATE HAVE_CONFIG_H)
target_link_libraries(de265-refpic-test-support PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(de265-refpic-test-support PUBLIC m)
endif()

add_executable(refpic-management-test
  refpic_management_test.cc
  refpic_rps_semantics_test.cc
)
target_include_directories(refpic-management-test BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/libde265
)
target_compile_definitions(refpic-management-test PRIVATE HAVE_CONFIG_H)
target_link_libraries(refpic-management-test
  PRIVATE
    de265-refpic-test-support
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(refpic-management-test)
