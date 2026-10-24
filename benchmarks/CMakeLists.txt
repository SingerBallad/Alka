add_executable(AlkaBench AlkaBench.cpp)
target_link_libraries(AlkaBench PRIVATE Alka::Alka Alka::CompileOptions)
alka_copy_v8_runtime(AlkaBench)
