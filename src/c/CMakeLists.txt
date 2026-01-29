option(USE_COMPILER_RT "use compiler-rt runtime" OFF)
option(USE_ADDRESS_SANITIZER "use address sanitizer" OFF)
cmake_minimum_required(VERSION 3.10)

find_program(CLANG_FORMAT_EXE clang-format)
file(GLOB_RECURSE C_SOURCES LIST_DIRECTORIES false CONFIGURE_DEPENDS "*.c" "*.h")
list(FILTER C_SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
add_custom_target(${PROJECT_NAME}-format ALL
  COMMAND ${CLANG_FORMAT_EXE} -style=file -i ${C_SOURCES}
)

# stylua: autoformat for Lua files
set(STYLUA_VERSION "v2.3.1")
if(CMAKE_HOST_WIN32)
  set(STYLUA_ARCHIVE "stylua-windows-x86_64.zip")
  set(STYLUA_EXE_NAME "stylua.exe")
else()
  set(STYLUA_ARCHIVE "stylua-linux-x86_64-musl.zip")
  set(STYLUA_EXE_NAME "stylua")
endif()
set(STYLUA_URL "https://github.com/JohnnyMorganz/StyLua/releases/download/${STYLUA_VERSION}/${STYLUA_ARCHIVE}")
set(STYLUA_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/${STYLUA_ARCHIVE}")
set(STYLUA_DIR "${CMAKE_CURRENT_BINARY_DIR}/stylua")
set(STYLUA_EXE "${STYLUA_DIR}/${STYLUA_EXE_NAME}")
if(NOT EXISTS "${STYLUA_EXE}")
  if(NOT EXISTS "${STYLUA_ARCHIVE_PATH}")
    file(DOWNLOAD "${STYLUA_URL}" "${STYLUA_ARCHIVE_PATH}")
  endif()
  file(MAKE_DIRECTORY "${STYLUA_DIR}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xf "${STYLUA_ARCHIVE_PATH}"
    WORKING_DIRECTORY "${STYLUA_DIR}"
  )
  if(NOT CMAKE_HOST_WIN32)
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E chmod +x "${STYLUA_EXE}"
    )
  endif()
endif()

set(STYLUA_CONFIG_PATH "${CMAKE_SOURCE_DIR}/.stylua.toml")
set(STYLUA_CONFIG_ARGS)
if(EXISTS "${STYLUA_CONFIG_PATH}")
  set(STYLUA_CONFIG_ARGS "--config-path" "${STYLUA_CONFIG_PATH}")
endif()

set(LUA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lua")
file(GLOB_RECURSE LUA_SOURCES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${LUA_SOURCE_DIR}/*.lua")
list(FILTER LUA_SOURCES EXCLUDE REGEX "json\\.lua$")

add_custom_target(${PROJECT_NAME}-format-lua
  COMMAND "${STYLUA_EXE}" ${STYLUA_CONFIG_ARGS} ${LUA_SOURCES}
)

# Combine format targets used by other parts of the build (psdtoolkit_intf depends on this)
add_custom_target(${PROJECT_NAME}_format DEPENDS ${PROJECT_NAME}-format ${PROJECT_NAME}-format-lua)

add_subdirectory(3rd/ovbase)
add_subdirectory(3rd/ovl)
add_subdirectory(3rd/yyjson)

set(LOCAL_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/local)
file(MAKE_DIRECTORY ${LOCAL_INSTALL_PREFIX}/include)
include(ExternalProject)

set(LANGCSV "${CMAKE_CURRENT_SOURCE_DIR}/../i18n/langs.csv")
file(READ "${LANGCSV}" langs)
string(STRIP ${langs} langs)
string(REPLACE "\n" ";" langs "${langs}")
foreach(line IN LISTS langs)
  if (line MATCHES "^#.*$|^([^,]+),$")
    continue()
  endif()
  if (line MATCHES "^([^,]+),([^,]+)$")
    list(APPEND polist "${CMAKE_CURRENT_SOURCE_DIR}/../i18n/${CMAKE_MATCH_1}.po.DO_NOT_EDIT")
  else()
    message(FATAL_ERROR "invalid language difinition: ${line}")
  endif()
endforeach()
add_custom_command(
  OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc"
  COMMAND
    ${CMAKE_COMMAND}
    -Doutput_dir="${CMAKE_CURRENT_BINARY_DIR}"
    -Drctmpl="${CMAKE_CURRENT_SOURCE_DIR}/i18n.rc.tmpl"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/i18n_rc.cmake"
  WORKING_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/../i18n"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/i18n.rc.tmpl"
    ${polist}
)
add_custom_target(${PROJECT_NAME}_generate_i18n_rc DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc")

# Version generation - runs every build to check if git state changed
set(VERSION_H "${CMAKE_CURRENT_BINARY_DIR}/version.h")
set(VERSION_ENV "${CMAKE_CURRENT_BINARY_DIR}/version.env")
add_custom_target(${PROJECT_NAME}_generate_version_h
  COMMAND ${CMAKE_COMMAND}
    -Dlocal_dir="${CMAKE_CURRENT_SOURCE_DIR}"
    -Dinput_file="${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    -Doutput_file="${VERSION_H}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/version.cmake"
  BYPRODUCTS "${VERSION_H}" "${VERSION_ENV}"
  COMMENT "Checking version information..."
)

find_program(XXD xxd CMAKE_FIND_ROOT_PATH_BOTH)
if(NOT XXD)
  find_program(XXD_BUSYBOX busybox CMAKE_FIND_ROOT_PATH_BOTH)
  if(XXD_BUSYBOX)
    set(XXD "${XXD_BUSYBOX};xxd")
  else()
    message(FATAL_ERROR "xxd not found")
  endif()
endif()

set(LUAJIT_PLATFORM x86_64)
set(LUAJIT_URL "https://github.com/oov/luajit/releases/download/v2.1.20250708-a2/luajit_v2.1.20250708-a2_${LUAJIT_PLATFORM}.zip")
string(REGEX MATCH "[^/]+$" LUAJIT_ARCHIVE_NAME "${LUAJIT_URL}")
set(LUAJIT_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/${LUAJIT_ARCHIVE_NAME}")
set(LUAJIT_DIR "${CMAKE_CURRENT_BINARY_DIR}/luajit")
set(LUAJIT_DLL "${LUAJIT_DIR}/bin/luaJIT.dll")
set(LUAJIT_INCLUDE "${LUAJIT_DIR}/include")
if(NOT EXISTS "${LUAJIT_DIR}")
  if(NOT EXISTS "${LUAJIT_ARCHIVE_PATH}")
    file(DOWNLOAD "${LUAJIT_URL}" "${LUAJIT_ARCHIVE_PATH}")
  endif()
  string(REGEX REPLACE "\\.[^.]+$" "" LUAJIT_ARCHIVE_NOEXT "${LUAJIT_ARCHIVE_NAME}")
  file(MAKE_DIRECTORY "${LUAJIT_DIR}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xf ${LUAJIT_ARCHIVE_PATH}
    WORKING_DIRECTORY "${LUAJIT_DIR}"
  )
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy "${LUAJIT_DIR}/bin/luaJIT.dll" "${CMAKE_CURRENT_BINARY_DIR}/luaJIT.dll"
  )
endif()

set(is_clang "$<C_COMPILER_ID:Clang>")
set(v16_or_later "$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,16>")
set(v18_or_later "$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,18>")
set(v19_or_later "$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,19>")
set(v21_or_later "$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,21>")
add_library(psdtoolkit_intf INTERFACE)
target_include_directories(psdtoolkit_intf INTERFACE
  "${CMAKE_CURRENT_BINARY_DIR}" # for version.h
  "${LUAJIT_INCLUDE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/3rd/aviutl2_plugin_sdk_for_c/include"
)
target_compile_definitions(psdtoolkit_intf INTERFACE
  $<$<COMPILE_LANGUAGE:C>:SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}>
  $<$<BOOL:${WIN32}>:_WIN32_WINNT=0x0601>
  _WINDOWS
  $<$<CONFIG:Release>:NDEBUG>
)
target_compile_options(psdtoolkit_intf INTERFACE
  $<$<AND:$<BOOL:${WIN32}>,$<BOOL:${USE_COMPILER_RT}>>:--rtlib=compiler-rt>
  -mstackrealign
  -Wall
  -Wextra
  -Werror
  -Weverything
  -Wshadow
  -Werror=return-type
  -pedantic-errors
  -Wno-declaration-after-statement
  -Wno-padded
  $<$<AND:${is_clang},${v21_or_later}>:-Wno-c++-keyword>
  $<$<AND:${is_clang},${v16_or_later}>:-Wno-unsafe-buffer-usage>
  $<$<AND:${is_clang},${v18_or_later}>:-Wno-switch-default>
  $<$<AND:${is_clang},${v19_or_later}>:-Wno-pre-c11-compat>
  -ffunction-sections
  -fdata-sections
  $<$<CONFIG:Debug>:-O0>
  $<$<CONFIG:Release>:-O2>
  $<$<BOOL:${USE_ADDRESS_SANITIZER}>:-fsanitize=address>
  $<$<NOT:$<BOOL:${USE_ADDRESS_SANITIZER}>>:-flto>
)
target_link_options(psdtoolkit_intf INTERFACE
  -fuse-ld=lld
  -Wl,--gc-sections
  # -Wl,--print-gc-sections
  -Wl,--kill-at
  $<$<BOOL:${USE_ADDRESS_SANITIZER}>:-fsanitize=address>
  $<$<CONFIG:Release>:-s>
)
add_dependencies(psdtoolkit_intf ${PROJECT_NAME}_format)

add_library(psdtoolkit SHARED
  alias.c
  anm2.c
  anm2_edit.c
  anm2_script_mapper.c
  anm2_selection.c
  anm2editor.c
  anm2editor_convert.c
  anm2editor_detail.c
  anm2editor_import.c
  anm2editor_splitter.c
  anm2editor_toolbar.c
  anm2editor_treeview.c
  anm2_script_picker.c
  anm_to_anm2.c
  cache.c
  config.c
  config_dialog.c
  dialog.c
  dllmain.c
  error.c
  i18n.c
  i18n.rc
  ini_reader.c
  input.c
  ipc.c
  json.c
  layer.c
  logf.c
  psdtoolkit.c
  script_module.c
  win32.c
  anm2editor.rc
  anm2_script_picker.rc
  config_dialog.rc
  psdtoolkit.rc
)
set_property(SOURCE psdtoolkit.rc APPEND PROPERTY OBJECT_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/psdtoolkit.manifest
)
set(PTK_PLUGIN_DIR "${EXPORT_DIR}/Plugin/PSDToolKit")
set_target_properties(psdtoolkit PROPERTIES
  OUTPUT_NAME "PSDToolKit.aux2"
  PREFIX ""
  SUFFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${PTK_PLUGIN_DIR}"
)
add_dependencies(psdtoolkit ${PROJECT_NAME}_generate_i18n_rc ${PROJECT_NAME}_generate_version_h)
target_link_libraries(psdtoolkit PRIVATE
  psdtoolkit_intf
  "${LUAJIT_DLL}"
  -Wl,-delayload,luaJIT.dll
  ovbase
  ovl
  yyjson
  gdiplus
)

add_executable(test_cache cache_test.c cache.c)
target_link_libraries(test_cache PRIVATE
  psdtoolkit_intf
  ovbase
)
add_test(NAME test_cache COMMAND test_cache)

add_executable(test_script_module script_module_test.c script_module.c)
target_link_libraries(test_script_module PRIVATE
  psdtoolkit_intf
  ovbase
)
add_test(NAME test_script_module COMMAND test_script_module)

add_executable(test_ini_reader ini_reader_test.c ini_reader.c)
target_link_libraries(test_ini_reader PRIVATE
  psdtoolkit_intf
  ovbase
  ovl
)
add_test(NAME test_ini_reader COMMAND test_ini_reader)

add_executable(test_layer layer_test.c ini_reader.c win32.c json.c i18n.c)
target_link_libraries(test_layer PRIVATE
  psdtoolkit_intf
  ovbase
  ovl
  yyjson
)
add_test(NAME test_layer COMMAND test_layer)

add_executable(test_anm2 anm2_test.c json.c)
target_link_libraries(test_anm2 PRIVATE
  psdtoolkit_intf
  ovbase
  ovl
  yyjson
)
add_test(NAME test_anm2 COMMAND test_anm2)

add_executable(test_anm2_edit anm2_edit_test.c anm2_edit.c anm2_script_mapper.c anm2_selection.c anm2.c json.c ini_reader.c i18n.c)
target_link_libraries(test_anm2_edit PRIVATE
  psdtoolkit_intf
  ovbase
  ovl
  yyjson
)
add_test(NAME test_anm2_edit COMMAND test_anm2_edit)

add_executable(test_anm2_selection anm2_selection_test.c anm2_selection.c anm2.c json.c)
target_link_libraries(test_anm2_selection PRIVATE
  psdtoolkit_intf
  ovbase
  ovl
  yyjson
)
add_test(NAME test_anm2_selection COMMAND test_anm2_selection)

add_executable(test_alias alias_test.c alias.c ini_reader.c i18n.c)
target_link_libraries(test_alias PRIVATE
  psdtoolkit_intf
  ovbase
  ovl
)
add_test(NAME test_alias COMMAND test_alias)

add_executable(test_anm_to_anm2 anm_to_anm2_test.c anm_to_anm2.c)
target_link_libraries(test_anm_to_anm2 PRIVATE
  psdtoolkit_intf
  ovbase
)
add_test(NAME test_anm_to_anm2 COMMAND test_anm_to_anm2)

# Lua tests using LuaJIT
set(LUAJIT_EXE "${LUAJIT_DIR}/bin/luajit.exe")
set(LUA_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test_data/lua")

# Test LayerSelector.lua exclusive support
add_test(
  NAME test_lua_exclusive_support
  COMMAND "${LUAJIT_EXE}" "${LUA_TEST_DIR}/test_exclusive_support.lua"
)
set_tests_properties(test_lua_exclusive_support PROPERTIES
  ENVIRONMENT "LUA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../lua"
)

set(LUA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lua")
set(BUNDLE_LUA_CMAKE "${CMAKE_SOURCE_DIR}/src/cmake/bundle_lua.cmake")

# Bundle Lua files
# Folder names must end with .lua (e.g., PSDToolKit.lua folder -> PSDToolKit.lua file, module name: PSDToolKit)
set(BUNDLE_LUA_TARGETS PSDToolKitHandler.lua PSDToolKit.lua)
foreach(name IN LISTS BUNDLE_LUA_TARGETS)
  string(REGEX REPLACE "\\.lua$" "" module_name "${name}")
  set(${name}_LUA_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}")
  file(GLOB_RECURSE ${name}_LUA_SOURCES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${LUA_SRC_DIR}/${name}/*.lua")
  add_custom_command(
    OUTPUT "${${name}_LUA_OUTPUT}"
    COMMAND ${CMAKE_COMMAND}
      -Dinput_dir="${LUA_SRC_DIR}/${name}"
      -Doutput_file="${${name}_LUA_OUTPUT}"
      -Dmodule_name="${module_name}"
      -Dversion_env_file="${VERSION_ENV}"
      -P "${BUNDLE_LUA_CMAKE}"
    DEPENDS ${${name}_LUA_SOURCES} "${BUNDLE_LUA_CMAKE}" "${VERSION_ENV}" ${PROJECT_NAME}_generate_version_h
    COMMENT "Bundling ${name}"
  )
endforeach()

# Generate multi-script files from directories starting with @
# Each directory under LUA_SRC_DIR starting with @ (e.g., @Name.obj2, @Name.anm2)
# is processed to generate a corresponding file with the same name.
set(BUNDLE_MULTI_SCRIPT_CMAKE "${CMAKE_SOURCE_DIR}/src/cmake/bundle_multi_script.cmake")
file(GLOB MULTI_SCRIPT_INPUT_DIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS "${LUA_SRC_DIR}/@*.*2")
set(MULTI_SCRIPT_OUTPUTS "")
foreach(multi_script_dir IN LISTS MULTI_SCRIPT_INPUT_DIRS)
  if(NOT IS_DIRECTORY "${multi_script_dir}")
    continue()
  endif()
  get_filename_component(multi_script_name "${multi_script_dir}" NAME)
  set(multi_script_output "${CMAKE_CURRENT_BINARY_DIR}/${multi_script_name}")
  file(GLOB_RECURSE multi_script_lua_sources LIST_DIRECTORIES false CONFIGURE_DEPENDS "${multi_script_dir}/*.lua")
  list(SORT multi_script_lua_sources)
  add_custom_command(
    OUTPUT "${multi_script_output}"
    COMMAND ${CMAKE_COMMAND}
      -Dinput_dir="${multi_script_dir}"
      -Doutput_file="${multi_script_output}"
      -Dversion_env_file="${VERSION_ENV}"
      -P "${BUNDLE_MULTI_SCRIPT_CMAKE}"
    DEPENDS ${multi_script_lua_sources} "${BUNDLE_MULTI_SCRIPT_CMAKE}" "${VERSION_ENV}" ${PROJECT_NAME}_generate_version_h
    COMMENT "Generating ${multi_script_name}"
  )
  list(APPEND MULTI_SCRIPT_OUTPUTS "${multi_script_output}")
endforeach()

# Generate embedded Lua script header (PSDToolKitHandler.lua -> PSDToolKitHandler_lua.h)
set(EMBED_LUA_HANDLER_NAME "PSDToolKitHandler_lua")
set(EMBED_LUA_HANDLER_H "${CMAKE_CURRENT_BINARY_DIR}/${EMBED_LUA_HANDLER_NAME}.h")
add_custom_command(
  OUTPUT "${EMBED_LUA_HANDLER_H}"
  COMMAND ${XXD} -i "PSDToolKitHandler.lua" > "${EMBED_LUA_HANDLER_NAME}.h"
  DEPENDS "${PSDToolKitHandler.lua_LUA_OUTPUT}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Generating ${EMBED_LUA_HANDLER_NAME}.h"
)
add_custom_target(${PROJECT_NAME}_generate_embed_lua_headers DEPENDS "${EMBED_LUA_HANDLER_H}")
add_dependencies(psdtoolkit ${PROJECT_NAME}_generate_embed_lua_headers)

add_custom_command(TARGET psdtoolkit POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/README.md" "${PTK_PLUGIN_DIR}/PSDToolKit.txt"
)

# Copy scripts to export directory
# Default: copy to Script/PSDToolKit with same filename
# Custom: "source_file|dest_subdir|dest_filename"
set(COPY_SCRIPTS
  "${PSDToolKit.lua_LUA_OUTPUT}"
  ${MULTI_SCRIPT_OUTPUTS}
)
foreach(entry IN LISTS COPY_SCRIPTS)
  string(REPLACE "|" ";" parts "${entry}")
  list(LENGTH parts parts_len)
  list(GET parts 0 src_file)
  if(parts_len GREATER 1)
    list(GET parts 1 dest_subdir)
    list(GET parts 2 dest_file)
  else()
    set(dest_subdir "Script/PSDToolKit")
    get_filename_component(dest_file "${src_file}" NAME)
  endif()
  # If src_file is an absolute path or starts with ${, use it as-is.
  # Otherwise, treat it as relative to LUA_SRC_DIR.
  if(IS_ABSOLUTE "${src_file}" OR src_file MATCHES "^\\$")
    set(src_path "${src_file}")
  else()
    set(src_path "${LUA_SRC_DIR}/${src_file}")
  endif()
  set(dest_dir "${EXPORT_DIR}/${dest_subdir}")
  set(dest_path "${dest_dir}/${dest_file}")
  add_custom_command(
    OUTPUT "${dest_path}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${dest_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "${src_path}" "${dest_path}"
    DEPENDS "${src_path}"
  )
  list(APPEND COPY_SCRIPT_OUTPUTS "${dest_path}")
endforeach()
add_custom_target(${PROJECT_NAME}_copy_script ALL DEPENDS ${COPY_SCRIPT_OUTPUTS} ${MULTI_SCRIPT_OUTPUTS})

# Copy template files to PSDToolKit/template directory
set(TEMPLATE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../template")
set(TEMPLATE_DEST_DIR "${EXPORT_DIR}/Plugin/PSDToolKit/template")
set(TEMPLATE_FILES
  "voice.object.template"
  "psd.object.template"
)
foreach(template_file IN LISTS TEMPLATE_FILES)
  set(src_path "${TEMPLATE_SRC_DIR}/${template_file}")
  set(dest_path "${TEMPLATE_DEST_DIR}/${template_file}")
  add_custom_command(
    OUTPUT "${dest_path}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TEMPLATE_DEST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${src_path}" "${dest_path}"
    DEPENDS "${src_path}"
  )
  list(APPEND COPY_TEMPLATE_OUTPUTS "${dest_path}")
endforeach()
add_custom_target(${PROJECT_NAME}_copy_template ALL DEPENDS ${COPY_TEMPLATE_OUTPUTS})

# Copy PSDToolKit.ini to export directory
set(PSDTOOLKIT_INI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/PSDToolKit.ini")
set(PSDTOOLKIT_INI_DEST "${EXPORT_DIR}/Plugin/PSDToolKit/PSDToolKit.ini")
add_custom_command(
  OUTPUT "${PSDTOOLKIT_INI_DEST}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${EXPORT_DIR}/Plugin/PSDToolKit"
  COMMAND ${CMAKE_COMMAND} -E copy "${PSDTOOLKIT_INI_SRC}" "${PSDTOOLKIT_INI_DEST}"
  DEPENDS "${PSDTOOLKIT_INI_SRC}"
  COMMENT "Copying PSDToolKit.ini"
)
add_custom_target(${PROJECT_NAME}_copy_ini ALL DEPENDS "${PSDTOOLKIT_INI_DEST}")

# Copy alias object files to Alias directory
set(ALIAS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../alias")
set(ALIAS_DEST_DIR "${EXPORT_DIR}/Alias")
set(ALIAS_FILES
  "subtitles.object"
)
foreach(alias_file IN LISTS ALIAS_FILES)
  set(src_path "${ALIAS_SRC_DIR}/${alias_file}")
  if(alias_file STREQUAL "subtitles.object")
    set(dest_path "${ALIAS_DEST_DIR}/字幕表示.object")
  else()
    set(dest_path "${ALIAS_DEST_DIR}/${alias_file}")
  endif()
  add_custom_command(
    OUTPUT "${dest_path}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALIAS_DEST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${src_path}" "${dest_path}"
    DEPENDS "${src_path}"
  )
  list(APPEND COPY_ALIAS_OUTPUTS "${dest_path}")
endforeach()
add_custom_target(${PROJECT_NAME}_copy_alias ALL DEPENDS ${COPY_ALIAS_OUTPUTS})

# Copy *.aul2 files to Language directory
set(AUL2_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aul2")
set(AUL2_DEST_DIR "${EXPORT_DIR}/Language")
set(AUL2_FILES
  "English.PSDToolKit.aul2"
  "简体中文.PSDToolKit.aul2"
)
foreach(aul2_file IN LISTS AUL2_FILES)
  set(src_path "${AUL2_SRC_DIR}/${aul2_file}")
  set(dest_path "${AUL2_DEST_DIR}/${aul2_file}")
  add_custom_command(
    OUTPUT "${dest_path}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AUL2_DEST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${src_path}" "${dest_path}"
    DEPENDS "${src_path}"
  )
  list(APPEND COPY_AUL2_OUTPUTS "${dest_path}")
endforeach()
add_custom_target(${PROJECT_NAME}_copy_aul2 ALL DEPENDS ${COPY_AUL2_OUTPUTS})
