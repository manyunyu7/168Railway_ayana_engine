// Engine C ABI ("ayana"): the world renderer as a library for a host that owns the simulation and the UI —
// the reference web client (ppka-wannabe-2/src/tiga-ayana) in the Wasm build. Every function is
// `extern "C"`, strings are UTF-8 / JSON, buffers live in the Wasm heap (eng_alloc / eng_free).
//
// Load sequence (the host drives every fetch; the engine never touches the network):
//   eng_init(w, h, dpr)                            GL context on <canvas id="ayana-canvas">, renderer, font
//   eng_load_world(world, summary, map, catalog)   save "world" object, bridge summary, map slug, model.json
//     -> asset requests through Module.onAssetRequest(kind, path):  kind "terrain" <map>/index.json, then the DEM
//        tiles <map>/dem/<z>_<x>_<y>.bin (eagerly, the static world needs them) and, while streaming, satellite
//        tiles <map>/sat/<layer>/<z>_<x>_<y>.bin as the camera moves (the host fetches from wherever it likes:
//        fetch_tiles output, or the tile servers decoded by the browser -> eng_terrain_tile_rgba);
//        "city" <slug>.json (baked OSM city, optional); "model" <catalog id> (geometry-only .emod, convert
//        --target web --textures external), then the KTX2 textures through eng_texture_* per image.
//   eng_terrain_index(bytes) / eng_terrain_tile(dir, z, x, y, bytes) / eng_terrain_tile_rgba(dir, z, x, y, w, h, px) /
//        eng_terrain_tile_fail(dir, z, x, y)   the static world is built when the last DEM tile answered (a failed
//        tile stays flat); every other answer streams in (2 GPU uploads per frame)
//   eng_city_json(bytes), eng_model_begin(slot, bytes) / eng_model_fail(slot)   decor (hiasan, garis, trees) is
//        built once every decor model answered; train models can arrive any time (box until then)
// Per frame: eng_set_state(stepJson) with the bridge's `step` object, then eng_frame(dt).
// Camera / input / picking: see below. HUD text is not drawn — the host owns the UI.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int eng_init(int width, int height, float dpr);
void eng_resize(int width, int height, float dpr);
void eng_shutdown(void);
int eng_load_world(const char* worldJson, const char* summaryJson, const char* mapSlug, const char* catalogJson);
int eng_terrain_index(const uint8_t* bytes, int len);
int eng_terrain_tile(const char* dir, int z, int x, int y, const uint8_t* bytes, int len);   // dem: f32[n*n]; sat/<i>: EIMG record
int eng_terrain_tile_rgba(const char* dir, int z, int x, int y, int w, int h, const uint8_t* rgba);   // dem: Terrarium PNG pixels; sat/<i>: imagery
void eng_terrain_tile_fail(const char* dir, int z, int x, int y);
int eng_city_json(const uint8_t* bytes, int len);
int eng_set_state(const char* stepJson);
void eng_frame(float dt);
int eng_ready(void);                 // 1 once the static world is built
const char* eng_stats(void);         // JSON: fps, drawCalls, buildMs, summary, pending assets, terrain {near, far, patches, resident, requested, pendingJobs, trees}

// camera (modes as engine/app/camera_rig.h CamMode: 0 bebas 1 jalan 2 kabin 3 samping 4 atas 5 ekor)
int eng_camera_mode(int mode);       // returns the mode in effect (a train mode without a train stays put)
int eng_get_camera_mode(void);
void eng_orbit(float dx, float dy);  // pixels
void eng_zoom(float steps);
void eng_set_view(float distance, float yaw, float pitch);
void eng_look_at(double wx, double wy);              // orbit target (world XY), height from the ground
void eng_compass_click(float x, float y);            // short right-click at (x, y): fly the focus there
void eng_compass_glide(float x, float y, int on);    // right button held: glide towards (x, y)
void eng_follow_train(const char* id);              // "" = nearest to the camera
void eng_cycle_subject(int dir);
void eng_telescope(int held);
void eng_key(const char* code, int down);           // walk mode: KeyW/A/S/D, ShiftLeft, Arrow*
void eng_pointer(float x, float y, int button, int phase);   // css px; phase 0 down 1 move 2 up; button 0 left 1 right
const char* eng_camera_json(void);                   // {mode, eye:[x,y,z], look:[...], distance, yaw, pitch, fov}

// picking / hover (css px). Returns "signal:<id>" | "point:<id>" | "".
const char* eng_pick(float x, float y);
void eng_hover(const char* id);                      // "" = none; ring drawn on the object
const char* eng_object_screen(const char* id);       // "x,y" css px of the object's label anchor, "" when off screen
int eng_set_preview(const char* json);               // {"path":[segId...],"dead":bool} or "" / "null" to clear
const char* eng_train_screen(void);                  // JSON [{id,no,name,state,speed,x,y,d}] label anchors on screen

// asset ingestion
void* eng_alloc(int bytes);
void eng_free(void* p);
int eng_model_begin(const char* slot, const uint8_t* bytes, int len);   // geometry-only EMOD
void eng_model_fail(const char* slot);
int eng_supports(int format);        // eng::TexFormat: 0 RGBA8 1 ETC2_RGB 2 ETC2_RGBA 3 BC1 4 BC3 5 BC7
int eng_image_count(const char* slot);
int eng_image_source(const char* slot, int i);
int eng_image_flags(const char* slot, int i);   // wrapS | wrapT << 8 | linear << 16 | placeholder << 17
int eng_texture_begin(const char* slot, int image, int width, int height, int format, int mipCount, int srgb, int wrapS, int wrapT);
int eng_texture_mip(int level, const uint8_t* data, int bytes);
int eng_texture_end(void);

#ifdef __cplusplus
}
#endif
