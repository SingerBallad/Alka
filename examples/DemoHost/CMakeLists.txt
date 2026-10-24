add_executable(DemoHost
        Main.cpp)

target_link_libraries(DemoHost PRIVATE
        Alka::Alka
        Alka::InspectorBridge
        Alka::CompileOptions)

alka_copy_v8_runtime(DemoHost)
