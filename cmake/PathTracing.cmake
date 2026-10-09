# Native path tracing deliberately bypasses bgfx GPU objects.
file(GLOB PT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/pt/*.metal")
set(PT_OUTPUTS)
foreach(SOURCE IN LISTS PT_SOURCES)
  get_filename_component(NAME "${SOURCE}" NAME)
  set(OUTPUT "${CMAKE_BINARY_DIR}/shaders/pt/${NAME}")
  add_custom_command(OUTPUT "${OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders/pt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SOURCE}" "${OUTPUT}"
    DEPENDS "${SOURCE}")
  list(APPEND PT_OUTPUTS "${OUTPUT}")
endforeach()
if(APPLE)
  enable_language(OBJCXX)
  target_sources(aspera_app PRIVATE src/render_pt_metal.mm)
  set_source_files_properties(src/render_pt_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(aspera_app PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
  execute_process(COMMAND xcrun --find metal OUTPUT_VARIABLE PT_METAL_COMPILER
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE PT_METAL_FOUND)
  if(PT_METAL_FOUND EQUAL 0)
    foreach(NAME pt_trace pt_temporal pt_filter pt_post pt_present pt_imgui)
      set(AIR "${CMAKE_BINARY_DIR}/shaders/pt/${NAME}.air")
      set(LIB "${CMAKE_BINARY_DIR}/shaders/pt/${NAME}.metallib")
      add_custom_command(OUTPUT "${LIB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders/pt"
        COMMAND xcrun -sdk macosx metal -std=metal2.4 -c "${CMAKE_SOURCE_DIR}/assets/shaders/pt/${NAME}.metal" -o "${AIR}"
        COMMAND xcrun -sdk macosx metallib "${AIR}" -o "${LIB}"
        DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/pt/${NAME}.metal" "${CMAKE_SOURCE_DIR}/assets/shaders/pt/pt_common.metal")
      list(APPEND PT_OUTPUTS "${LIB}")
    endforeach()
  else()
    message(STATUS "Metal offline compiler unavailable: package native sources for runtime compilation")
  endif()
endif()
if(APPLE)
  option(ASPERA_VULKAN_PT "Build Vulkan native path tracing (normally Windows/Linux)" OFF)
else()
  option(ASPERA_VULKAN_PT "Build Vulkan native path tracing when SDK is available" ON)
endif()
if(ASPERA_VULKAN_PT)
  find_package(Vulkan QUIET)
  find_program(PT_GLSLANG glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin")
  if(Vulkan_FOUND AND PT_GLSLANG)
    target_sources(aspera_app PRIVATE src/render_pt_vulkan.cpp)
    target_link_libraries(aspera_app PRIVATE Vulkan::Vulkan)
    if(WIN32 AND MSVC)
      # Keep the default D3D11 renderer usable without a Vulkan runtime.
      # SDL loads/checks Vulkan before the backend calls any delayed imports.
      # https://learn.microsoft.com/en-us/cpp/build/reference/linker-support-for-delay-loaded-dlls
      target_link_libraries(aspera_app PRIVATE delayimp)
      target_link_options(aspera_app INTERFACE "/DELAYLOAD:vulkan-1.dll")
    endif()
    target_compile_definitions(aspera_app PRIVATE ASPERA_HAS_VULKAN_PT=1)
    file(GLOB PT_VULKAN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/pt/*.comp" "${CMAKE_SOURCE_DIR}/assets/shaders/pt/*.vert" "${CMAKE_SOURCE_DIR}/assets/shaders/pt/*.frag")
    foreach(SOURCE IN LISTS PT_VULKAN_SOURCES)
      get_filename_component(NAME "${SOURCE}" NAME)
      set(OUTPUT "${CMAKE_BINARY_DIR}/shaders/pt/${NAME}.spv")
      add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders/pt"
        COMMAND "${PT_GLSLANG}" -V --target-env vulkan1.2 "${SOURCE}" -o "${OUTPUT}"
        DEPENDS "${SOURCE}" "${CMAKE_SOURCE_DIR}/assets/shaders/pt/pt_common.glsl")
      list(APPEND PT_OUTPUTS "${OUTPUT}")
    endforeach()
  else()
    message(STATUS "Vulkan SDK/glslang unavailable: retain raster fallback on this build")
  endif()
endif()
add_custom_target(aspera_pt_shaders DEPENDS ${PT_OUTPUTS})
add_dependencies(aspera_app aspera_pt_shaders)
