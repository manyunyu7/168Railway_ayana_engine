# `ayana` — the engine as an ES module for the reference web client (ppka-wannabe-2/src/tiga-ayana).
# Emscripten only (the root CMakeLists adds this directory in the wasm preset). Output: build/wasm/ayana.js
# + ayana.wasm + ayana.data (the font); web/deploy-to-ppka.sh copies them to ppka-wannabe-2/public/ayana/.
add_executable(ayana engine_api.cpp engine_api_hud.cpp main_stub.cpp)
target_link_libraries(ayana PRIVATE engine)
set(AYANA_EXPORTS
  _eng_init _eng_resize _eng_shutdown _eng_load_world _eng_terrain_index _eng_terrain_tile _eng_terrain_tile_rgba _eng_terrain_tile_fail _eng_city_json _eng_set_state
  _eng_frame _eng_ready _eng_stats _eng_camera_mode _eng_get_camera_mode _eng_orbit _eng_zoom _eng_set_view _eng_look_at
  _eng_compass_click _eng_compass_glide _eng_follow_train _eng_cycle_subject _eng_telescope _eng_key _eng_pointer
  _eng_camera_json _eng_pick _eng_hover _eng_object_screen _eng_set_preview _eng_train_screen _eng_alloc _eng_free
  _eng_model_begin _eng_model_fail _eng_supports _eng_image_count _eng_image_source _eng_image_flags
  _eng_texture_begin _eng_texture_mip _eng_texture_end
  _eng_project _eng_signal_screen _eng_station_screen)
string(REPLACE ";" "," AYANA_EXPORTS_CSV "${AYANA_EXPORTS}")
target_link_options(ayana PRIVATE
  "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/font.efnt@/assets/font.efnt"
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createAyana -sENVIRONMENT=web
  -sALLOW_MEMORY_GROWTH=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sEXIT_RUNTIME=0 -sINVOKE_RUN=0
  -sEXPORTED_FUNCTIONS=${AYANA_EXPORTS_CSV}
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8)
