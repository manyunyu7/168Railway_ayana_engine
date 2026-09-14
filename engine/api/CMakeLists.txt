# `ayana` — the engine as an ES module for the reference web client (ppka-wannabe-2/src/tiga-ayana).
# Emscripten only (the root CMakeLists adds this directory in the wasm preset). Output: build/wasm/ayana.js
# + ayana.wasm + ayana.data (the font); web/deploy-to-ppka.sh copies them to ppka-wannabe-2/public/ayana/.
add_executable(ayana engine_api.cpp engine_api_hud.cpp engine_api_edit.cpp main_stub.cpp)
target_link_libraries(ayana PRIVATE engine)
set(AYANA_EXPORTS
  _eng_init _eng_resize _eng_shutdown _eng_load_world _eng_terrain_index _eng_terrain_tile _eng_terrain_tile_rgba _eng_terrain_tile_fail _eng_city_json _eng_set_state
  _eng_frame _eng_ready _eng_set_paused _eng_set_layer _eng_stats _eng_last_error _eng_camera_mode _eng_get_camera_mode _eng_orbit _eng_zoom _eng_set_view _eng_look_at
  _eng_compass_click _eng_compass_glide _eng_follow_train _eng_cycle_subject _eng_telescope _eng_key _eng_pointer
  _eng_camera_json _eng_pick _eng_hover _eng_object_screen _eng_set_preview _eng_train_screen _eng_alloc _eng_free
  _eng_model_begin _eng_model_fail _eng_supports _eng_image_count _eng_image_source _eng_image_flags
  _eng_texture_begin _eng_texture_mip _eng_texture_end _eng_model_textures_unavailable
  _eng_project _eng_signal_screen _eng_station_screen
  _eng_set_quality _eng_set_tree_radius _eng_set_tree_density _eng_set_sky_time _eng_set_theme _eng_reset_imagery
  _eng_compass_settings _eng_side_flip _eng_set_rig_param _eng_get_rig_param _eng_model_begin_glb
  _eng_set_panel _eng_rail_atlas_rgba
  _eng_pick_ground _eng_ground _eng_pick_object _eng_pick_track _eng_hiasan_set _eng_hiasan_add _eng_hiasan_remove _eng_hiasan_info _eng_model_size
  _eng_garis_set _eng_node_height _eng_rails_rebuild _eng_terrain_delta _eng_track_edit
  _eng_highlight _eng_gizmo _eng_gizmo_hit _eng_gizmo_angle _eng_ghost _eng_ukur_line)
string(REPLACE ";" "," AYANA_EXPORTS_CSV "${AYANA_EXPORTS}")
target_link_options(ayana PRIVATE
  "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/font.efnt@/assets/font.efnt"
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createAyana -sENVIRONMENT=web
  -sALLOW_MEMORY_GROWTH=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sEXIT_RUNTIME=0 -sINVOKE_RUN=0
  -sEXPORTED_FUNCTIONS=${AYANA_EXPORTS_CSV}
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8)
