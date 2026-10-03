add_library(ZiliuTIP SHARED
  src/dll_main.cpp
  src/language_bar_button.cpp
  src/text_service.cpp
  ZiliuTIP.def)

target_include_directories(ZiliuTIP PRIVATE include)
target_link_libraries(ZiliuTIP PRIVATE ziliu_ipc_client ziliu_ui ole32 oleaut32 shell32 uuid)
set_target_properties(ZiliuTIP PROPERTIES OUTPUT_NAME "ZiliuTIP")
option(ZILIU_CONTEXT_METADATA_PROBE "TEST ONLY: bounded synthetic-field diagnostics" OFF)
if(ZILIU_CONTEXT_METADATA_PROBE)
  target_compile_definitions(ZiliuTIP PRIVATE ZILIU_CONTEXT_METADATA_PROBE=1)
endif()
ziliu_configure_target(ZiliuTIP)
ziliu_add_version_resource(
  ZiliuTIP "Ziliu Text Services Framework input processor" "ZiliuTIP.dll" VFT_DLL)
