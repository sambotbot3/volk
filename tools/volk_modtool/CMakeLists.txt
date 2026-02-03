add_executable(volk_modtool volk_modtool_generate.cpp)
target_compile_features(volk_modtool PRIVATE cxx_std_17)

install(TARGETS volk_modtool RUNTIME DESTINATION ${VOLK_RUNTIME_DIR} COMPONENT "volk")
