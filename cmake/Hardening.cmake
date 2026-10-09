# Sanitizer and fuzzing support for this project's own targets.
#
# Sanitizers are applied only to aspera_* targets, not to the fetched
# dependencies: the point is a clean signal about this codebase, and Jolt in
# particular trips UBSan on intentional type punning.

option(ASPERA_SANITIZE "Build project targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ASPERA_FUZZ "Build the untrusted-input fuzz targets" OFF)

set(ASPERA_SANITIZER_FLAGS "")
if(ASPERA_SANITIZE)
  if(MSVC)
    set(ASPERA_SANITIZER_FLAGS /fsanitize=address)
  else()
    # Trap rather than log-and-continue, so a sanitizer finding fails the test
    # run instead of scrolling past in CI output.
    set(ASPERA_SANITIZER_FLAGS
        -fsanitize=address,undefined -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer -g)
  endif()
endif()

# Applies the project's chosen warning and sanitizer settings to one target.
function(aspera_harden target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
  if(ASPERA_SANITIZER_FLAGS)
    target_compile_options(${target} PRIVATE ${ASPERA_SANITIZER_FLAGS})
    target_link_options(${target} PRIVATE ${ASPERA_SANITIZER_FLAGS})
  endif()
endfunction()

if(ASPERA_SANITIZE AND NOT MSVC)
  if(APPLE)
    set(ASPERA_SANITIZER_ENV "ASAN_OPTIONS=detect_leaks=0")
  else()
    set(ASPERA_SANITIZER_ENV "ASAN_OPTIONS=detect_leaks=1")
  endif()
  list(APPEND ASPERA_SANITIZER_ENV "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endif()

# Applies the sanitizer runtime options a test needs, if any.
function(aspera_test_environment test)
  if(ASPERA_SANITIZER_ENV)
    set_tests_properties(${test} PROPERTIES ENVIRONMENT "${ASPERA_SANITIZER_ENV}")
  endif()
endfunction()

# libFuzzer ships with LLVM but not with Apple clang. Detect it rather than
# assume, and fall back to tests/fuzz_main.cpp, which replays the same harnesses
# deterministically so the targets stay useful on every toolchain.
set(ASPERA_HAVE_LIBFUZZER OFF)
if(ASPERA_FUZZ AND NOT MSVC)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
  check_cxx_source_compiles(
    "#include <cstddef>
     #include <cstdint>
     extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t *, size_t) { return 0; }"
    ASPERA_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

# Adds one fuzz target. With libFuzzer it is a coverage-guided fuzzer; without
# it, a deterministic corpus replay registered with ctest so CI still runs it.
function(aspera_fuzz_target name source)
  if(NOT ASPERA_FUZZ)
    return()
  endif()
  if(ASPERA_HAVE_LIBFUZZER)
    add_executable(${name} ${source})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(${name} ${source} ${CMAKE_SOURCE_DIR}/tests/fuzz_main.cpp)
  endif()
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(${name} PRIVATE ${ARGN})
  aspera_harden(${name})
  # Bounded so it can run in CI on every push; raise -runs= for a longer session.
  add_test(NAME ${name} COMMAND ${name} -runs=20000)
  set_tests_properties(${name} PROPERTIES TIMEOUT 300 LABELS fuzz)
  aspera_test_environment(${name})
endfunction()
