# The pinned bgfx revision timestamps render views but omits compute encoders.
# Keep this narrow extension reproducible and fail if a dependency update changes its anchors.
function(aspera_replace_exact VARIABLE BEFORE AFTER)
  string(FIND "${${VARIABLE}}" "${BEFORE}" FOUND)
  if(FOUND EQUAL -1)
    message(FATAL_ERROR "Metal compute profiling patch no longer matches pinned bgfx")
  endif()
  string(REPLACE "${BEFORE}" "${AFTER}" PATCHED "${${VARIABLE}}")
  set(${VARIABLE} "${PATCHED}" PARENT_SCOPE)
endfunction()

set(METAL_HEADER "${bgfx_cmake_SOURCE_DIR}/bgfx/src/renderer_mtl.h")
set(METAL_SOURCE "${bgfx_cmake_SOURCE_DIR}/bgfx/src/renderer_mtl.cpp")
file(READ "${METAL_HEADER}" HEADER)
if(NOT HEADER MATCHES "attach\\(MTL::ComputePassDescriptor")
  aspera_replace_exact(HEADER
    "void attach(MTL::RenderPassDescriptor* _renderPassDescriptor);"
    "void attach(MTL::RenderPassDescriptor* _renderPassDescriptor);\n\t\tvoid attach(MTL::ComputePassDescriptor* _computePassDescriptor);")
  file(WRITE "${METAL_HEADER}" "${HEADER}")
endif()
file(READ "${METAL_SOURCE}" SOURCE)
if(NOT SOURCE MATCHES "Aspera compute view profiling")
  set(ATTACH [=[
	// Aspera compute view profiling: stage-boundary counters include the whole encoder.
	void TimerQueryMtl::attach(MTL::ComputePassDescriptor* _computePassDescriptor)
	{
		if (!isViewTimingSupported() )
		{
			return;
		}
		Sampling& sampling = m_sampling[m_samplingIdx];
		if (UINT32_MAX == sampling.m_activeQuery
		|| sampling.m_numSamples + 2 > kMaxTimestampSamples)
		{
			return;
		}
		MTL::ComputePassSampleBufferAttachmentDescriptor* desc = _computePassDescriptor->sampleBufferAttachments()->object(0);
		desc->setSampleBuffer(sampling.m_sampleBuffer);
		desc->setStartOfEncoderSampleIndex(sampling.m_numSamples);
		desc->setEndOfEncoderSampleIndex(sampling.m_numSamples + 1);
		sampling.m_query[sampling.m_activeQuery].m_num += 2;
		sampling.m_numSamples += 2;
	}

	void TimerQueryMtl::resolve(uint32_t _samplingIdx)
]=])
  aspera_replace_exact(SOURCE "\tvoid TimerQueryMtl::resolve(uint32_t _samplingIdx)\n" "${ATTACH}")
  aspera_replace_exact(SOURCE
    "if (!wasCompute)\n\t\t\t\t\t{"
    "if (!wasCompute || (viewChanged && profileViews) )\n\t\t\t\t\t{")
  aspera_replace_exact(SOURCE
    "m_computeCommandEncoder = m_commandBuffer->computeCommandEncoder();"
    "MTL::ComputePassDescriptor* computePassDescriptor = MTL::ComputePassDescriptor::alloc()->init();\n\t\t\t\t\t\tif (profileViews)\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\tm_gpuTimer.attach(computePassDescriptor);\n\t\t\t\t\t\t}\n\t\t\t\t\t\tm_computeCommandEncoder = m_commandBuffer->computeCommandEncoder(computePassDescriptor);\n\t\t\t\t\t\tcomputePassDescriptor->release();")
  file(WRITE "${METAL_SOURCE}" "${SOURCE}")
endif()
