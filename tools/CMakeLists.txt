if(ALKA_BUILD_ENGINE)
  add_library(AlkaInspectorBridge STATIC InspectorBridge/InspectorBridge.cpp)
  add_library(Alka::InspectorBridge ALIAS AlkaInspectorBridge)
  target_include_directories(AlkaInspectorBridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(AlkaInspectorBridge
    PUBLIC Alka::Alka
    PRIVATE Alka::CompileOptions)
  if(WIN32)
    target_link_libraries(AlkaInspectorBridge PUBLIC ws2_32)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(AlkaInspectorBridge PUBLIC Threads::Threads)
  endif()
endif()
