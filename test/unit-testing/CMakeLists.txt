# target platform, not host
if(WIN32)
#  if(CMAKE_CROSSCOMPILING)
#    set(CMAKE_CROSSCOMPILING_EMULATOR "wine")
#  endif(CMAKE_CROSSCOMPILING)
  add_custom_command(OUTPUT ${redwg}.dll
    COMMAND file(COPY ${top_builddir}/${redwg}.dll
                 DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    DEPENDS ${redwg}
  )
endif(WIN32)

add_executable(bits_test
  bits_test.c
  ../../src/bits.c
  ../../src/logging.c
  ../../src/codepages.c
  ../../src/classes.c
  ../../src/objects.c
  tests_common.h)
#set_property(TARGET bits_test PROPERTY LINK_LIBRARIES -lm ${CMAKE_THREAD_LIBS_INIT})
add_test(bits_test ${TARGET_SYSTEM_EMULATOR} bits_test${CMAKE_EXECUTABLE_SUFFIX})
if(WIN32)
  set_tests_properties(bits_test PROPERTIES FIXTURES_SETUP ${redwg}.dll)
endif()

# hash_test includes hash.c, to fail its allocations
add_executable(hash_test
  hash_test.c
  ../../src/bits.c
  ../../src/logging.c
  ../../src/codepages.c
  ../../src/classes.c
  tests_common.h)
#set_property(TARGET hash_test PROPERTY LINK_LIBRARIES -lm ${CMAKE_THREAD_LIBS_INIT})
add_test(hash_test ${TARGET_SYSTEM_EMULATOR} hash_test${CMAKE_EXECUTABLE_SUFFIX})

add_executable(common_test
  common_test.c
  ../../src/logging.c
  ../../src/classes.c
  ../../src/codepages.c
  tests_common.h)
add_test(common_test ${TARGET_SYSTEM_EMULATOR} common_test${CMAKE_EXECUTABLE_SUFFIX})
if(WIN32)
  set_tests_properties(common_test PROPERTIES FIXTURES_SETUP ${redwg}.dll)
endif()

add_executable(decode_test
  decode_test.c
  ../../src/dwg.c
  ../../src/bits.c
  ../../src/logging.c
  ../../src/codepages.c
  ../../src/hash.c
  ../../src/decode_r2007.c
  ../../src/objects.c
  ../../src/classes.c
  ../../src/print.c
  ../../src/decode2.c
  tests_common.h)
#set_property(TARGET decode_test
#  PROPERTY LINK_LIBRARIES -lm ${CMAKE_THREAD_LIBS_INIT})
target_include_directories(decode_test PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR})
add_test(decode_test ${TARGET_SYSTEM_EMULATOR} decode_test${CMAKE_EXECUTABLE_SUFFIX})

add_executable(dxf_test
  dxf_test.c
  ../../src/dynapi.c
  ../../src/logging.c
  ../../src/objects.c
  ../../src/classes.c
  tests_common.h)
#set_property(TARGET dxf_test
#  PROPERTY LINK_LIBRARIES -lm ${CMAKE_THREAD_LIBS_INIT})
add_test(dxf_test ${TARGET_SYSTEM_EMULATOR} dxf_test${CMAKE_EXECUTABLE_SUFFIX})

add_executable(dynapi_test
  dynapi_test.c
  ../../src/bits.c
  ../../src/logging.c
  ../../src/codepages.c
  ../../src/objects.c
  ../../src/classes.c
  tests_common.h)
#set_property(TARGET dynapi_test
#  PROPERTY LINK_LIBRARIES -lm ${CMAKE_THREAD_LIBS_INIT})
add_test(dynapi_test ${TARGET_SYSTEM_EMULATOR} dynapi_test${CMAKE_EXECUTABLE_SUFFIX})

# reads drawings on several threads at once
find_package(Threads)
if(Threads_FOUND)
  add_executable(threads_test
    threads_test.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  target_link_libraries(threads_test ${CMAKE_THREAD_LIBS_INIT})
  if(CMAKE_USE_PTHREADS_INIT)
    target_compile_definitions(threads_test PRIVATE HAVE_PTHREAD)
  endif()
  if(NOT MSVC)
    add_test(threads_test ${TARGET_SYSTEM_EMULATOR} threads_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds test/test-data
    set_tests_properties(threads_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../.."
      SKIP_RETURN_CODE 77)
  endif()
endif()

if(NOT LIBREDWG_DISABLE_WRITE)
  add_executable(color_test color_test.c ../../src/bits.c ../../src/codepages.c
    ../../src/logging.c ../../src/classes.c tests_common.h)
  if(NOT MSVC)
    add_test(color_test ${TARGET_SYSTEM_EMULATOR} color_test${CMAKE_EXECUTABLE_SUFFIX})
  endif()
  add_executable(svg_api_test svg_api_test.c ../../src/logging.c
    ../../src/classes.c tests_common.h)
  if(NOT MSVC)
    add_test(svg_api_test ${TARGET_SYSTEM_EMULATOR} svg_api_test${CMAKE_EXECUTABLE_SUFFIX})
  endif()
  add_executable(add_test
    add_test.c
    ../../src/bits.c
    ../../src/codepages.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(add_test ${TARGET_SYSTEM_EMULATOR} add_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds the drawings in test/test-data
    set_tests_properties(add_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../..")
  endif()
  add_executable(dynblocks_test
    dynblocks_test.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(dynblocks_test ${TARGET_SYSTEM_EMULATOR} dynblocks_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds test/test-data/2004/Dynblocks.dwg
    set_tests_properties(dynblocks_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../..")
  endif()
  add_executable(dynblock_eval_test
    dynblock_eval_test.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(dynblock_eval_test ${TARGET_SYSTEM_EMULATOR} dynblock_eval_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds test/test-data/2004/Dynblocks.dwg
    set_tests_properties(dynblock_eval_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../..")
  endif()
  add_executable(dynblock_insert_test
    dynblock_insert_test.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(dynblock_insert_test ${TARGET_SYSTEM_EMULATOR} dynblock_insert_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds test/test-data/2004/Dynblocks.dwg
    set_tests_properties(dynblock_insert_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../..")
  endif()
  add_executable(fields_test
    fields_test.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(fields_test ${TARGET_SYSTEM_EMULATOR} fields_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds test/test-data
    set_tests_properties(fields_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../..")
  endif()
  add_executable(summaryinfo_test
    summaryinfo_test.c
    ../../src/bits.c
    ../../src/codepages.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(summaryinfo_test ${TARGET_SYSTEM_EMULATOR} summaryinfo_test${CMAKE_EXECUTABLE_SUFFIX})
  endif()
  # copy_test includes copy.c, which calls internal functions of these
  add_executable(copy_test
    copy_test.c
    ../../src/bits.c
    ../../src/codepages.c
    ../../src/common.c
    ../../src/decode.c
    ../../src/decode2.c
    ../../src/decode_r2007.c
    ../../src/dwg.c
    ../../src/encode.c
    ../../src/encode2.c
    ../../src/hash.c
    ../../src/logging.c
    ../../src/objects.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(copy_test ${TARGET_SYSTEM_EMULATOR} copy_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds test/test-data
    set_tests_properties(copy_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../..")
  endif()
  add_executable(eed_test
    eed_test.c
    ../../src/logging.c
    ../../src/classes.c
    tests_common.h)
  if(NOT MSVC)
    add_test(eed_test ${TARGET_SYSTEM_EMULATOR} eed_test${CMAKE_EXECUTABLE_SUFFIX})
    # where the test finds test/test-data
    set_tests_properties(eed_test PROPERTIES
      ENVIRONMENT "top_srcdir=${CMAKE_CURRENT_SOURCE_DIR}/../..")
  endif()
endif(NOT LIBREDWG_DISABLE_WRITE)
