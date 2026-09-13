// vec/mat4/quat/geometry checks against hand-computed values.
#include "engine/math/geometry.h"
#include "engine/math/math.h"
#include "tests/check.h"

using namespace eng;

static bool isIdentity(const mat4& m, float eps) {
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) if (!test::near(m.m[i][j], i == j ? 1 : 0, eps)) return false;
  return true;
}

TEST_MAIN({
  // ---- vec3
  vec3 a{1, 2, 3}, b{4, 5, 6};
  CHECK_NEAR(dot(a, b), 32, 0);
  vec3 c = cross({1, 0, 0}, {0, 1, 0}); CHECK(c.x == 0 && c.y == 0 && c.z == 1);
  CHECK_NEAR(length({3, 4, 0}), 5, 0);
  CHECK_NEAR(length(normalize({3, 4, 12})), 1, 1e-6);
  CHECK(length(normalize({0, 0, 0})) == 0);   // no NaN on zero
  vec3 l = lerp(a, b, 0.5f); CHECK_NEAR(l.x, 2.5, 0); CHECK_NEAR(l.z, 4.5, 0);
  CHECK_NEAR(radians(180), PI, 1e-6); CHECK_NEAR(degrees(PI / 2), 90, 1e-4);

  // ---- mat4: column-major storage, translation in m[3]
  mat4 t = mat4::translation({1, 2, 3});
  CHECK(t.data()[12] == 1 && t.data()[13] == 2 && t.data()[14] == 3);
  vec3 p = t.transformPoint({0, 0, 0}); CHECK(p.x == 1 && p.y == 2 && p.z == 3);
  vec3 d = t.transformDir({1, 0, 0}); CHECK(d.x == 1 && d.y == 0 && d.z == 0);   // w=0 ignores translation

  // inverse: M^-1 * M = I for a generic TRS, and a singular matrix yields identity (documented fallback)
  mat4 m = trs({3, -2, 5}, quat::axisAngle({1, 2, 3}, 0.7f), {2, 3, 4});
  CHECK(isIdentity(m.inverse() * m, 1e-5f));
  CHECK(isIdentity(m * m.inverse(), 1e-5f));
  CHECK(isIdentity(mat4::scale({0, 1, 1}).inverse(), 0));
  CHECK(isIdentity(m.transpose().transpose() * m.inverse(), 1e-5f));

  // rotationY(90°): +X -> -Z (right-handed, Y up)
  vec3 rx = mat4::rotationY(PI / 2).transformDir({1, 0, 0});
  CHECK_NEAR(rx.x, 0, 1e-6); CHECK_NEAR(rx.z, -1, 1e-6);
  // rotationX(90°): +Y -> +Z
  vec3 ry = mat4::rotationX(PI / 2).transformDir({0, 1, 0});
  CHECK_NEAR(ry.y, 0, 1e-6); CHECK_NEAR(ry.z, 1, 1e-6);

  // trs order: scale, then rotate, then translate. Point (1,0,0) with S=2, R=90° about Y, T=(0,0,10) -> (0,0,8).
  mat4 x = trs({0, 0, 10}, quat::axisAngle({0, 1, 0}, PI / 2), {2, 2, 2});
  vec3 q = x.transformPoint({1, 0, 0});
  CHECK_NEAR(q.x, 0, 1e-5); CHECK_NEAR(q.y, 0, 1e-5); CHECK_NEAR(q.z, 8, 1e-5);
  // the wrong order (T applied before S) would give z = 20 + ... ; check explicitly it is not that
  CHECK(!test::near(q.z, 20, 1e-3));

  // quat: axis-angle to matrix matches rotationY; composition order (a*b applies b first)
  quat qy = quat::axisAngle({0, 1, 0}, 0.9f);
  mat4 qm = qy.toMat4(), rm = mat4::rotationY(0.9f);
  for (int i = 0; i < 16; ++i) CHECK_NEAR(qm.data()[i], rm.data()[i], 1e-6);
  quat qa = quat::axisAngle({0, 1, 0}, PI / 2), qb = quat::axisAngle({1, 0, 0}, PI / 2);
  vec3 v1 = (qa * qb).toMat4().transformDir({0, 1, 0});            // X first: (0,1,0)->(0,0,1); then Y: (0,0,1)->(1,0,0)
  CHECK_NEAR(v1.x, 1, 1e-6); CHECK_NEAR(v1.y, 0, 1e-6); CHECK_NEAR(v1.z, 0, 1e-6);
  vec3 v2 = (qa * qb).toMat4().transformDir({1, 0, 0});
  vec3 v2b = (qa.toMat4() * qb.toMat4()).transformDir({1, 0, 0});
  CHECK_NEAR(v2.x, v2b.x, 1e-6); CHECK_NEAR(v2.z, v2b.z, 1e-6);

  // ---- lookAt: eye (0,0,5) looking at origin -> view maps origin to (0,0,-5), +X stays +X
  mat4 v = mat4::lookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
  vec3 o = v.transformPoint({0, 0, 0}); CHECK_NEAR(o.x, 0, 1e-6); CHECK_NEAR(o.y, 0, 1e-6); CHECK_NEAR(o.z, -5, 1e-6);
  vec3 e = v.transformPoint({0, 0, 5}); CHECK_NEAR(length(e), 0, 1e-6);
  vec3 xr = v.transformDir({1, 0, 0}); CHECK_NEAR(xr.x, 1, 1e-6);
  // eye on +X looking at origin: world -X is forward (view -Z), world +Y is up, world +Z is right... (s = f x up = (-1,0,0)x(0,1,0) = (0,0,-1))
  mat4 v2m = mat4::lookAt({10, 0, 0}, {0, 0, 0}, {0, 1, 0});
  vec3 fwd = v2m.transformDir({-1, 0, 0}); CHECK_NEAR(fwd.z, -1, 1e-6);
  vec3 rgt = v2m.transformDir({0, 0, -1}); CHECK_NEAR(rgt.x, 1, 1e-6);

  // ---- perspective: fov 90°, aspect 1, near 1, far 3 -> f=1; z maps near->-1, far->+1
  mat4 pr = mat4::perspective(PI / 2, 1, 1, 3);
  CHECK_NEAR(pr.m[0][0], 1, 1e-6); CHECK_NEAR(pr.m[1][1], 1, 1e-6);
  CHECK_NEAR(pr.m[2][2], (3 + 1) / (1 - 3.f), 1e-6);   // -2
  CHECK_NEAR(pr.m[3][2], 2 * 3 * 1 / (1 - 3.f), 1e-6);  // -3
  CHECK_NEAR(pr.m[2][3], -1, 0); CHECK(pr.m[3][3] == 0);
  vec3 zn = pr.transformPoint({0, 0, -1}); CHECK_NEAR(zn.z, -1, 1e-6);
  vec3 zf = pr.transformPoint({0, 0, -3}); CHECK_NEAR(zf.z, 1, 1e-6);
  vec3 edge = pr.transformPoint({2, 0, -2}); CHECK_NEAR(edge.x, 1, 1e-6);   // 45° edge of the frustum
  mat4 pr2 = mat4::perspective(radians(60), 16.f / 9, 0.1f, 100);
  CHECK_NEAR(pr2.m[1][1], 1 / std::tan(radians(30)), 1e-5); CHECK_NEAR(pr2.m[0][0], pr2.m[1][1] * 9 / 16, 1e-5);

  // ---- Frustum::contains
  mat4 vp = pr2 * mat4::lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
  Frustum fr(vp);
  AABB in; in.expand({-1, -1, -10}); in.expand({1, 1, -8});
  AABB behind; behind.expand({-1, -1, 5}); behind.expand({1, 1, 8});
  AABB tooFar; tooFar.expand({-1, -1, -200}); tooFar.expand({1, 1, -150});
  AABB left; left.expand({-100, -1, -10}); left.expand({-50, 1, -8});
  AABB straddle; straddle.expand({-50, -1, -10}); straddle.expand({50, 1, -8});   // partly inside
  CHECK(fr.contains(in)); CHECK(!fr.contains(behind)); CHECK(!fr.contains(tooFar)); CHECK(!fr.contains(left)); CHECK(fr.contains(straddle));
  CHECK(!fr.contains(AABB{{-1, -1, -0.05f}, {1, 1, -0.01f}}));   // in front of the near plane
  AABB huge; huge.expand({-1e4f, -1e4f, -1e4f}); huge.expand({1e4f, 1e4f, 1e4f}); CHECK(fr.contains(huge));
  CHECK(in.valid()); CHECK(!AABB{}.valid());
  vec3 ctr = in.center(); CHECK_NEAR(ctr.z, -9, 1e-6); CHECK_NEAR(in.extent().x, 1, 1e-6);
  AABB tr = in.transformed(mat4::rotationY(PI / 2) * mat4::translation({0, 0, 1}));
  CHECK_NEAR(tr.min.x, -9, 1e-5); CHECK_NEAR(tr.max.x, -7, 1e-5);   // z in [-9,-7] rotates to x in [-9,-7]

  // ---- ray/AABB
  AABB box; box.expand({-1, -1, -1}); box.expand({1, 1, 1});
  float tt = 0;
  CHECK(intersect(Ray{{0, 0, 5}, {0, 0, -1}}, box, tt)); CHECK_NEAR(tt, 4, 1e-6);
  CHECK(!intersect(Ray{{0, 0, 5}, {0, 0, 1}}, box, tt));            // pointing away
  CHECK(!intersect(Ray{{0, 3, 5}, {0, 0, -1}}, box, tt));           // misses above
  CHECK(intersect(Ray{{0, 0, 0}, {0, 0, -1}}, box, tt)); CHECK_NEAR(tt, 0, 1e-6);   // origin inside -> t=0
  CHECK(intersect(Ray{{5, 5, 5}, normalize({-1, -1, -1})}, box, tt)); CHECK_NEAR(tt, std::sqrt(3.f) * 4, 1e-4);
  CHECK(intersect(Ray{{-5, 0.5f, 0.5f}, {1, 0, 0}}, box, tt)); CHECK_NEAR(tt, 4, 1e-6);   // axis-aligned dir with zero components

  // ---- ray/triangle (Möller–Trumbore, both faces)
  vec3 ta{0, 0, 0}, tb{1, 0, 0}, tc{0, 1, 0};
  CHECK(intersect(Ray{{0.25f, 0.25f, 1}, {0, 0, -1}}, ta, tb, tc, tt)); CHECK_NEAR(tt, 1, 1e-6);
  CHECK(intersect(Ray{{0.25f, 0.25f, -1}, {0, 0, 1}}, ta, tb, tc, tt)); CHECK_NEAR(tt, 1, 1e-6);   // back face
  CHECK(!intersect(Ray{{0.75f, 0.75f, 1}, {0, 0, -1}}, ta, tb, tc, tt));   // outside (u+v>1)
  CHECK(!intersect(Ray{{0.25f, 0.25f, 1}, {0, 0, 1}}, ta, tb, tc, tt));    // behind the origin
  CHECK(!intersect(Ray{{0.25f, 0.25f, 1}, {1, 0, 0}}, ta, tb, tc, tt));    // parallel
  CHECK(!intersect(Ray{{-0.1f, 0.5f, 1}, {0, 0, -1}}, ta, tb, tc, tt));    // u < 0

  // ---- screenRay round trip: centre pixel looks along the camera forward; corners project back to their pixels
  mat4 view = mat4::lookAt({3, 4, 5}, {0, 1, 0}, {0, 1, 0});
  mat4 proj = mat4::perspective(radians(50), 1.5f, 0.5f, 200);
  mat4 vpm = proj * view, inv = vpm.inverse();
  Ray centre = screenRay(0.5f, 0.5f, inv);
  vec3 f = normalize(vec3{0, 1, 0} - vec3{3, 4, 5});
  CHECK_NEAR(dot(centre.dir, f), 1, 1e-4);
  CHECK_NEAR(length(centre.origin - vec3{3, 4, 5}), 0.5, 1e-2);   // starts on the near plane
  for (float sx : {0.f, 0.2f, 0.9f, 1.f}) for (float sy : {0.f, 0.35f, 1.f}) {
    Ray r = screenRay(sx, sy, inv);
    vec3 pw = r.at(20);
    vec4 clip = vpm * vec4(pw, 1);
    float px = (clip.x / clip.w + 1) / 2, py = (1 - clip.y / clip.w) / 2;
    CHECK_NEAR(px, sx, 1e-3); CHECK_NEAR(py, sy, 1e-3);
  }
  CHECK_NEAR(length(screenRay(0.1f, 0.9f, inv).dir), 1, 1e-6);
})
