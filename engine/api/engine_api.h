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
//        --target web --textures external), then the KTX2 textures through eng_texture_* per image (request them
//        together with the geometry; a model draws as a box / is skipped until every texture landed).
//   eng_terrain_index(bytes) / eng_terrain_tile(dir, z, x, y, bytes) / eng_terrain_tile_rgba(dir, z, x, y, w, h, px) /
//        eng_terrain_tile_fail(dir, z, x, y)   the static world is built when the last DEM tile answered (a failed
//        tile stays flat); every other answer streams in (2 GPU uploads per frame)
//   eng_city_json(bytes), eng_model_begin(slot, bytes) / eng_model_fail(slot)   decor (hiasan, garis, trees) is
//        built once every decor model answered; train models can arrive any time (box until then)
//   A missing / invalid index (eng_terrain_index(0, 0) or bad JSON) is not fatal: the world is built on flat ground
//   at rail height and eng_last_error() carries the reason.
// Per frame: eng_set_state(stepJson) with the bridge's `step` object, then eng_frame(dt).
// Large strings (the world save, model.json ~600 KB): copy them into the heap (eng_alloc + stringToUTF8) and pass
// the pointer - ccall's 'string' arguments live on the Wasm STACK (1 MB), which overflows for big maps.
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
int eng_terrain_index(const uint8_t* bytes, int len);   // 0 + eng_last_error() when missing/invalid: the world is built WITHOUT terrain (flat)
const char* eng_last_error(void);                        // last failure message ("" when none); cleared by eng_load_world
int eng_terrain_tile(const char* dir, int z, int x, int y, const uint8_t* bytes, int len);   // dem: f32[n*n]; sat/<i>: EIMG record
int eng_terrain_tile_rgba(const char* dir, int z, int x, int y, int w, int h, const uint8_t* rgba);   // dem: Terrarium PNG pixels; sat/<i>: imagery
void eng_terrain_tile_fail(const char* dir, int z, int x, int y);
int eng_city_json(const uint8_t* bytes, int len);
int eng_set_state(const char* stepJson);
// Schematic control table (bridge `panel` JSON, sim-state.ts panelLayoutJson): the in-world meja boards on the station
// GLB `mejalayan` quads (engine/world/meja_board). Static per world; send once after eng_load_world. "" / "{}" clears.
int eng_set_panel(const char* panelJson);
// Corridor atlas (catalog `tekstur.rel1067`, pilot-rel-1067.jpg): RGBA8 rows bottom-up (three's flipY), clamp S / repeat T,
// sRGB, mipmapped; replaces the procedural painter (same UV layout). Any time after eng_init.
int eng_rail_atlas_rgba(int w, int h, const uint8_t* rgba);
void eng_frame(float dt);
int eng_ready(void);                 // 1 once the static world is built
void eng_set_paused(int paused);     // game paused: JPL arms / cloud drift freeze (the camera still moves)
// Visibility layers (dunia3dKonst.ts TAMPIL_BAKU): "pita" route/occupancy ribbons, "wesel" point arrows (hidden = not
// pickable either), "pohon" trees, "awan" clouds, "kota" city, "benang" iconic rail line forced at every distance (else
// only when far). "label" / "papan" / "tepi" / "pelat" are the host's DOM overlays: accepted (returns 1) but nothing
// changes in the engine. Unknown name = 0. All on by default (benang off).
int eng_set_layer(const char* name, int on);
const char* eng_stats(void);         // JSON: fps, drawCalls, buildMs, summary, pending assets, terrain {near, far, patches, resident, requested, pendingJobs, trees}

// ---- quality / world look (dunia3d.ts laci KAMERA; the host persists the choices) ----
// Quality tier (dunia3dKonst.ts TINGKAT_MUTU): 0 penuh .. 4 minimum -> tree draw radius 3200/2600/2000/1400/900 m and
// clouds off from tier 2 (the dpr cap is the host's: eng_resize). Returns the tier in effect.
int eng_set_quality(int tier);
void eng_set_tree_radius(float metres);   // overrides the tier's radius (touch screens: JARAK_SENTUH rVeg)
void eng_set_tree_density(float k);       // `Kerapatan pohon` 0..16 (RAPAT_BAKU 2): every cell re-scattered
// Sky time: seconds since 00:00 to pin the sun (the `Siang tetap` choice = 12 h at 58 deg elevation is what the host
// sends), < 0 = follow the sim clock again (eng_set_state).
void eng_set_sky_time(double sec);
void eng_set_theme(int dark);             // UI theme (TEMA): fog / dome-ground tint, backdrop plane colour
// The host changed its ground imagery source (SUMBER_TANAH): every resident AND in-flight imagery tile is dropped
// and requested again through onAssetRequest — the host must discard answers to requests issued before this call
// (they carry the old source). `flat` = the plain "polos" mode: the host fails every request and the tiles are
// painted in the theme's ground colour (hampar) instead of the loading colour.
void eng_reset_imagery(int flat);

// ---- compass / rig settings ----
void eng_compass_settings(int rotation, float speed, int show);   // kompas3d.ts SetelanKompas (rotation vs panning, 0.5..2x, reticle)
void eng_side_flip(void);                                         // `Pindah sisi`: mirror the samping camera
// Per-mode framing parameter (dunia3d.ts profilKam `atur`): jalan fov 45..95, kabin fov 40..95, samping distance 8..160,
// atas height 40..2000, ekor distance 10..200. The value is clamped; bebas has none (get returns 0, set is ignored).
void eng_set_rig_param(float value);
float eng_get_rig_param(void);

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
void eng_key(const char* code, int down);           // walk mode: KeyW/A/S/D, ShiftLeft, Space (jump), Arrow*; kabin: Q/E/R
void eng_pointer(float x, float y, int button, int phase);   // css px; phase 0 down 1 move 2 up; button 0 left 1 right
const char* eng_camera_json(void);                   // {mode, eye:[x,y,z], look:[...], distance, yaw, pitch, fov, azimuth, subject}
                                                     // subject = followed train id ("" in bebas without an explicit follow)

// picking / hover (css px). Returns "signal:<id>" | "point:<id>" | "".
const char* eng_pick(float x, float y);
void eng_hover(const char* id);                      // "" = none; ring drawn on the object
const char* eng_object_screen(const char* id);       // "x,y" css px of the object's label anchor, "" when off screen
int eng_set_preview(const char* json);               // {"path":[segId...],"dead":bool} or "" / "null" to clear
const char* eng_train_screen(void);                  // JSON [{id,no,name,state,speed,x,y,d,front,visible,ax,ay}] every train: roof anchor
                                                     // (mirrored when behind the camera), ax/ay = 60 m ahead of the nose

// asset ingestion
void* eng_alloc(int bytes);
void eng_free(void* p);
int eng_model_begin(const char* slot, const uint8_t* bytes, int len);   // geometry-only EMOD
int eng_model_begin_glb(const char* slot, const uint8_t* bytes, int len);   // plain GLB (no Draco/meshopt) when no .emod was
                                                                        // prebuilt: images dropped, textures stream as usual
void eng_model_fail(const char* slot);
int eng_supports(int format);        // eng::TexFormat: 0 RGBA8 1 ETC2_RGB 2 ETC2_RGBA 3 BC1 4 BC3 5 BC7
int eng_image_count(const char* slot);
int eng_image_source(const char* slot, int i);
int eng_image_flags(const char* slot, int i);   // wrapS | wrapT << 8 | linear << 16 | placeholder << 17
int eng_texture_begin(const char* slot, int image, int width, int height, int format, int mipCount, int srgb, int wrapS, int wrapT);
int eng_texture_mip(int level, const uint8_t* data, int bytes);
int eng_texture_end(void);
// The host has no KTX2 twin for the slot: every placeholder still waiting becomes neutral grey (0x9aa3ac) and the
// model counts as textured-complete. Until a streamed model IS textured-complete (every placeholder answered by
// eng_texture_end, or this call) trains draw the sarana-coloured box and hiasan / garis / tree models are skipped.
void eng_model_textures_unavailable(const char* slot);

// ---- HUD projection (engine_api_hud.cpp) — screen anchors for the host's DOM overlays -------------------
// Signal name plates, station bubbles and their tethers are DOM; these answer "where on the canvas" for the
// last drawn frame. css px, y down. `visible` = inside the viewport and in front of the camera.
int eng_project(double wx, double wy, int heightMode, float* out);   // world XY -> out[4] {x, y, visible, dist m};
                                                                     // heightMode 0 carved ground, 1 rail head at the
                                                                     // nearest track point; returns 1 when in front of the camera
const char* eng_signal_screen(void);    // JSON [{id,name,type,x,y,visible,aspect,dist}] every signal, top lens / LOD dot
                                        // (SignalVisuals::screenPositions); aspect "red"|"yellow"|"green"
const char* eng_station_screen(void);   // JSON [{id,code,name,x,y,visible,dist}] station scenery, anchor 55 m
                                        // (TINGGI_PAPAN) above the carved ground; off-camera stations are omitted

// ---- editing (engine_api_edit.cpp; docs/SURVEYOR.md) — the surveyor tools of the host (Tata objek, Gambar garis,
// Kuas tanah, Editor rel, Objek rel, Ukur) keep their DOM / keyboard / save logic in TypeScript and call these for
// what needed three.js: raycasts, live re-placement, partial rebuilds and overlays. Coordinates: x/y css px,
// wx/wy world (Mercator metres, y south-positive), heights in scene metres (rail head at ~0 near the origin).
// Every call answers "" / 0 before eng_ready().
// Picking:
const char* eng_pick_ground(float x, float y);      // "wx,wy,h" carved ground under the pixel (heightfield march), "" = sky
float eng_ground(double wx, double wy);             // carved ground height (scene m) at a world point (tanahTerukir)
// "hiasan:<index>" (world.hiasan.objek index; triangle-precise, then a 16 px screen tolerance for thin targets like
// uji3dTata.ts objDiLayar) | "garis:<index>" (hiasan.garis polyline within 16 px) | "signal:<id>" | "point:<id>"
// | "node:<id>" (track node within 14 px, editorRel3d.ts nodeDiLayar) | "segment:<id>:<s>" (rail centreline within
// 12 px, s in metres) | "". Earlier kinds win.
const char* eng_pick_object(float x, float y);
const char* eng_pick_node(float x, float y, float maxPx);    // "<nodeId>" of the node handle within maxPx (0 = 14 px) regardless of what covers it, ""
const char* eng_pick_track(float x, float y, float maxPx);   // "segId,s,side" nearest centreline within maxPx (side +1 = cursor left of the tangent), ""
// Live edits (no full rebuild; the world save object inside the engine is updated too, so a later eng_track_edit /
// reload sees the same data; the host mirrors every edit into its own World):
int eng_hiasan_set(int index, double wx, double wy, float naik, float rotDeg, float skala);   // re-places one model
int eng_hiasan_add(const char* json);       // {"model","x","y","naik","rot","skala",...} -> new index (-1 = bad json); a model
                                            // not resident yet is requested and shows when it lands
int eng_hiasan_remove(int index);
const char* eng_hiasan_info(int index);     // JSON {model,x,y,naik,rot,skala,resident,size:[x,y,z]} (size = normalised model), "" = bad index
const char* eng_model_size(const char* id); // "x,y,z" normalised size of a RESIDENT catalog model (palette cards; never requests one), "" otherwise
int eng_garis_set(const char* json);        // {"index":i, kelas, naik, titik:[{x,y}], kunci} replaces (i < 0 appends); {"index":i,"remove":true}
int eng_node_height(const char* nodeId, double h, int hasHeight);   // hand-written `tinggi` (raw DEM m); hasHeight 0 = follow the DEM. Takes effect at eng_rails_rebuild
double eng_rails_rebuild(void);             // profile + rails + terrain chords + signals / points / boards / JPL re-placed; returns ms (< 1 s on Mojokerto)
int eng_terrain_delta(const char* json);    // the whole world.tanah ({kisi:8, delta:{"gx,gz":m}}) or "null" -> only the tiles whose cells changed are re-cut; hiasan / garis re-placed
const char* eng_node_info(const char* nodeId);   // JSON {h: raw DEM m at the rail head (the pin when hand-written), tulis, grad:[permille per neighbour]}
                                                 // (editorRel3d.ts infoTinggiNode), "" = unknown node / no profile yet
int eng_veg_mask(const char* json);         // the whole world.vegMask ([{x,y,r,a}] world m, last stamp wins; a -1 clear / +1 plant) or "null";
                                            // only the tree cells under stamps that differ from the previous list are re-scattered
int eng_track_edit(const char* worldJson);  // full graph rebuild for the rail editor (new save "world" object; terrain data kept; decor rebuilt from resident models)
// Overlays (drawn after the world; gizmo / ukur depth-test-off like the compass):
void eng_highlight(const char* id, int mode);   // "hiasan:<i>" | "garis:<i>" (box: 1 blue, 2 amber locked) | "node:<id>" (green sphere on the
                                                // handle) | "segment:<id>[:s]" (amber ribbon along the centreline); mode 0 off
void eng_node_handles(int on, int tier);        // editorRel3d.ts node dots (rail head + 0.6): points orange, hand-written magenta, ends white,
                                                // plain blue; tier 1 = the important ones only (points / hand-written / ends)
int eng_ghost_lines(const char* json);          // [[{x,y},...],...] editor preview polylines (drag ghost / chain draw) 0.9 m over the rail
                                                // head near each point, thin blue; "[]" clears
// kind "rotate" (ring + needle + knob: uji3dTata cincin), "move" (ring + 4 arrows), "" hides. wx/wy world, h scene height
// of the ring plane, yaw radians (three convention), scale = ring radius m, axisHover 0 none 1 ring 2 knob 3..6 arrows.
void eng_gizmo(const char* kind, double wx, double wy, float h, float yaw, float scale, int axisHover);
int eng_gizmo_hit(float x, float y);            // 0 none, 1 ring (thick 0.7..1.3 hit band), 2 knob
float eng_gizmo_angle(float x, float y);        // cursor angle in the ring plane (rad, three convention), 1e9 = miss
int eng_ghost(const char* modelId, double wx, double wy, float rotDeg, float skala);   // translucent blue model at the cursor (uji3dTata hantu); "" hides
int eng_ukur_line(const char* json);            // [{x,y},...] world polyline drawn 0.4 m over the ground; "[]" / "" clears. Labels: the host projects the points (eng_project)

// ---- markers (engine_api_markers.cpp; engine/app/markers.h) — a batch of world-space editor markers (spline handles,
// trackside / scenery markers, measurement lines) replaced as a whole from JSON and drawn after the world overlays:
// [{"id","kind":"sphere|disc|cube|diamond|cone|polyline","x","y","naik","hm":0|1,"h","color":"#rrggbb","alpha","size",
//   "px","depth":false,"yaw","label":false,"pts":[{x,y,naik,hm,h}]}]. x/y world; height = ground (hm 0) / rail head
// (hm 1) at the point + naik, or `h` absolute (scene m). size in metres (radius / edge / cone length / ribbon width);
// px > 0 = a point marker keeps that css screen radius; depth false = overlay (drawn over the world); yaw radians
// (three convention; cone tip along +X); label = listed by eng_markers_screen. "[]" / "" clears.
int eng_markers(const char* json);
int eng_markers_count(void);
const char* eng_markers_pick(float x, float y, float maxPx);   // id of the nearest marker within maxPx css px (0 = 16), "" = none
const char* eng_markers_screen(void);                          // JSON [{id, x, y (css px, top of the marker), d (m), v (on screen)}] of the `label` markers

#ifdef __cplusplus
}
#endif
