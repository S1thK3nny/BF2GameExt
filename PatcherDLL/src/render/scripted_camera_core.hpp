#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// =============================================================================
// The scripted camera's maths, with nothing of the engine in it, so the tests
// run it as the game does (tests/scripted_camera_tests.cpp).
//
// A view is the engine's camera matrix: rows right, up and back, then the
// position. The camera looks along minus back. The free camera builds its
// look-at that way (FreeCamera::Update, M 0x004AE1B0): back from the target to
// the camera, right = up x back with up (0, 1, 0), up = back x right.
// An object's matrix is rows right, up, forward, position, and it faces along
// forward. docs/RE/ScriptedCamera.md has the engine side.
// =============================================================================

namespace scripted_camera {

struct V3 {
   float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 operator-(V3 a) { return { -a.x, -a.y, -a.z }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }
inline bool finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// `a` scaled to length 1, or `fallback` when it has no length to speak of.
inline V3 normalize(V3 a, V3 fallback)
{
   const float l = length(a);
   return l > 1e-6f && std::isfinite(l) ? a * (1.0f / l) : fallback;
}

inline float clamp01(float t) { return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; }

struct View {
   V3 right{ 1.0f, 0.0f, 0.0f };
   V3 up{ 0.0f, 1.0f, 0.0f };
   V3 back{ 0.0f, 0.0f, 1.0f };
   V3 pos{};
};

// The engine's 16 floats for a view: rows right, up, back, position, with the
// w column 0, 0, 0, 1.
inline void to_matrix(const View& v, float m[16])
{
   const V3* rows[4] = { &v.right, &v.up, &v.back, &v.pos };
   for (int i = 0; i < 4; ++i) {
      m[i * 4 + 0] = rows[i]->x;
      m[i * 4 + 1] = rows[i]->y;
      m[i * 4 + 2] = rows[i]->z;
      m[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
   }
}

inline View from_matrix(const float m[16])
{
   View v;
   V3* rows[4] = { &v.right, &v.up, &v.back, &v.pos };
   for (int i = 0; i < 4; ++i) *rows[i] = { m[i * 4 + 0], m[i * 4 + 1], m[i * 4 + 2] };
   return v;
}

constexpr V3 kWorldUp{ 0.0f, 1.0f, 0.0f };

// A level view (no roll) at `pos` looking along `dir`, the free camera's way.
// Straight up or down there is no level right: `rightHint` is kept then.
inline View look_along(V3 pos, V3 dir, V3 rightHint = { 1.0f, 0.0f, 0.0f })
{
   View v;
   v.pos = pos;
   v.back = normalize(-dir, { 0.0f, 0.0f, 1.0f });
   v.right = normalize(cross(kWorldUp, v.back), normalize(rightHint, { 1.0f, 0.0f, 0.0f }));
   v.up = normalize(cross(v.back, v.right), kWorldUp);
   return v;
}

inline View look_at(V3 pos, V3 target, V3 rightHint = { 1.0f, 0.0f, 0.0f })
{
   return look_along(pos, target - pos, rightHint);
}

// The view looking the way an object's matrix faces (rows right, up, forward,
// position), roll included: right and back flip, up stays.
inline View facing_view(const float m[16])
{
   View v;
   v.right = { -m[0], -m[1], -m[2] };
   v.up = { m[4], m[5], m[6] };
   v.back = { -m[8], -m[9], -m[10] };
   v.pos = { m[12], m[13], m[14] };
   // Rebuild an exact frame from the facing and the up, in case the matrix is
   // scaled or a little skewed.
   const V3 back = normalize(v.back, { 0.0f, 0.0f, 1.0f });
   V3 right = normalize(cross(v.up, back), normalize(v.right, { 1.0f, 0.0f, 0.0f }));
   v.back = back;
   v.right = right;
   v.up = normalize(cross(back, right), kWorldUp);
   return v;
}

// A point given in an object's frame, {right, up, forward}, in the world.
inline V3 from_object(const float m[16], V3 local)
{
   return { m[12] + m[0] * local.x + m[4] * local.y + m[8] * local.z,
            m[13] + m[1] * local.x + m[5] * local.y + m[9] * local.z,
            m[14] + m[2] * local.x + m[6] * local.y + m[10] * local.z };
}

// -----------------------------------------------------------------------------
// Rotations, for blending one view into another
// -----------------------------------------------------------------------------

struct Quat {
   float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

// The rotation whose matrix has these rows, a column vector convention on the
// transposed rows: a point's camera coordinates c go to the world as
// c.x * right + c.y * up + c.z * back. to_rows undoes it exactly.
inline Quat to_quat(const View& v)
{
   // R's columns are right, up, back.
   const float m00 = v.right.x, m01 = v.up.x, m02 = v.back.x;
   const float m10 = v.right.y, m11 = v.up.y, m12 = v.back.y;
   const float m20 = v.right.z, m21 = v.up.z, m22 = v.back.z;
   Quat q;
   const float trace = m00 + m11 + m22;
   if (trace > 0.0f) {
      const float s = std::sqrt(trace + 1.0f) * 2.0f;
      q.w = 0.25f * s;
      q.x = (m21 - m12) / s;
      q.y = (m02 - m20) / s;
      q.z = (m10 - m01) / s;
   } else if (m00 > m11 && m00 > m22) {
      const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
      q.w = (m21 - m12) / s;
      q.x = 0.25f * s;
      q.y = (m01 + m10) / s;
      q.z = (m02 + m20) / s;
   } else if (m11 > m22) {
      const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
      q.w = (m02 - m20) / s;
      q.x = (m01 + m10) / s;
      q.y = 0.25f * s;
      q.z = (m12 + m21) / s;
   } else {
      const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
      q.w = (m10 - m01) / s;
      q.x = (m02 + m20) / s;
      q.y = (m12 + m21) / s;
      q.z = 0.25f * s;
   }
   const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
   if (n > 1e-6f && std::isfinite(n)) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
   else q = Quat{};
   return q;
}

inline void to_rows(Quat q, View& v)
{
   const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
   const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
   const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
   // Columns of R.
   v.right = { 1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy) };
   v.up    = { 2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx) };
   v.back  = { 2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy) };
}

inline Quat slerp(Quat a, Quat b, float t)
{
   float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
   if (d < 0.0f) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
   float wa, wb;
   if (d > 0.9995f) {
      wa = 1.0f - t;
      wb = t;
   } else {
      const float th = std::acos(d);
      const float s = std::sin(th);
      wa = std::sin((1.0f - t) * th) / s;
      wb = std::sin(t * th) / s;
   }
   Quat q{ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
   const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
   if (n > 1e-6f) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
   return q;
}

// `w` of the way from `a` to `b`: the position along a line, the turn the
// short way round.
inline View blend(const View& a, const View& b, float w)
{
   w = clamp01(w);
   if (w <= 0.0f) return a;
   if (w >= 1.0f) return b;
   View v;
   v.pos = lerp(a.pos, b.pos, w);
   to_rows(slerp(to_quat(a), to_quat(b), w), v);
   return v;
}

// -----------------------------------------------------------------------------
// Timing
// -----------------------------------------------------------------------------

enum class Ease { InOut, In, Out, Linear };

inline float ease(Ease e, float t)
{
   t = clamp01(t);
   switch (e) {
   case Ease::In:     return t * t;
   case Ease::Out:    return 1.0f - (1.0f - t) * (1.0f - t);
   case Ease::Linear: return t;
   default:           return t * t * (3.0f - 2.0f * t);
   }
}

// How far a blend over `duration` has got after `elapsed`: eased in and out,
// and done at once for a duration of 0 (a cut).
inline float blend_weight(float elapsed, float duration)
{
   if (!(duration > 0.0f)) return 1.0f;
   return ease(Ease::InOut, elapsed / duration);
}

// A point trailing `target` by `lag` seconds: it covers 1 - e^(-dt / lag) of
// the gap each step, the same share whatever the frame rate.
inline V3 trail(V3 current, V3 target, float dt, float lag)
{
   if (!(lag > 0.0f) || !(dt > 0.0f)) return target;
   const float k = 1.0f - std::exp(-dt / lag);
   return lerp(current, target, k);
}

// -----------------------------------------------------------------------------
// Field of view
// -----------------------------------------------------------------------------

constexpr float kPi = 3.14159265358979f;
inline float radians(float deg) { return deg * (kPi / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / kPi); }

// A FOV in the game's measure (horizontal at 4:3, in degrees; 48 is the
// default) as the camera takes it on a screen of `aspect` (height over width),
// holding the vertical, as GetWidescreenFOVAdjustment widens the game's own:
// 2 atan(0.75 tan(fov / 2) / aspect), in radians.
inline float widened_fov(float fovDegrees, float aspect)
{
   const float half = std::tan(radians(fovDegrees) * 0.5f);
   if (!(aspect > 0.0f) || !std::isfinite(aspect)) return 2.0f * std::atan(half);
   return 2.0f * std::atan(0.75f * half / aspect);
}

// The game's measure back from a camera FOV in radians (the inverse of
// widened_fov), for blending from the FOV on screen.
inline float game_fov(float cameraFov, float aspect)
{
   const float half = std::tan(cameraFov * 0.5f);
   if (!(aspect > 0.0f) || !std::isfinite(aspect)) return degrees(2.0f * std::atan(half));
   return degrees(2.0f * std::atan(half * aspect / 0.75f));
}

// -----------------------------------------------------------------------------
// A path the camera flies: a centripetal Catmull-Rom curve through the nodes,
// so it passes through every node, never overshoots into a loop between close
// ones, and is flown at an even speed along its length.
// -----------------------------------------------------------------------------

class Curve {
public:
   static constexpr int kSamples = 24;   // per segment, for the length table

   // False with fewer than two nodes.
   bool build(const std::vector<V3>& nodes, bool closed)
   {
      points_ = nodes;
      closed_ = closed && nodes.size() >= 3;
      table_.clear();
      length_ = 0.0f;
      if (points_.size() < 2) return false;
      const int segs = segments();
      table_.push_back(0.0f);
      V3 prev = segment_point(0, 0.0f);
      for (int s = 0; s < segs; ++s)
         for (int i = 1; i <= kSamples; ++i) {
            const V3 p = segment_point(s, (float)i / kSamples);
            length_ += length(p - prev);
            table_.push_back(length_);
            prev = p;
         }
      return true;
   }

   float total() const { return length_; }
   int segments() const { return (int)points_.size() - (closed_ ? 0 : 1); }
   bool closed() const { return closed_; }

   // The point `distance` along the curve (clamped, or wrapped when closed),
   // and which segment it is on and how far through it.
   V3 at(float distance, int* segment = nullptr, float* through = nullptr) const
   {
      if (points_.empty()) return {};
      if (points_.size() == 1 || !(length_ > 0.0f)) {
         if (segment) *segment = 0;
         if (through) *through = 0.0f;
         return points_[0];
      }
      if (closed_) {
         distance = std::fmod(distance, length_);
         if (distance < 0.0f) distance += length_;
      } else {
         distance = (std::min)((std::max)(distance, 0.0f), length_);
      }
      // The sample at or after it.
      const auto it = std::lower_bound(table_.begin(), table_.end(), distance);
      size_t i = (size_t)(it - table_.begin());
      if (i == 0) i = 1;
      if (i >= table_.size()) i = table_.size() - 1;
      const float a = table_[i - 1], b = table_[i];
      const float f = b > a ? (distance - a) / (b - a) : 0.0f;
      const float u = ((float)(i - 1) + f) / kSamples;   // segments from the start
      int seg = (int)u;
      if (seg >= segments()) seg = segments() - 1;
      const float t = u - (float)seg;
      if (segment) *segment = seg;
      if (through) *through = clamp01(t);
      return segment_point(seg, clamp01(t));
   }

private:
   const V3& node(int i) const
   {
      const int n = (int)points_.size();
      if (closed_) return points_[(size_t)(((i % n) + n) % n)];
      return points_[(size_t)(std::min)((std::max)(i, 0), n - 1)];
   }

   // The end segments' outer control points mirror the next node, so the curve
   // leaves its first node and reaches its last heading along the path.
   V3 control(int i) const
   {
      const int n = (int)points_.size();
      if (!closed_ && i < 0) return node(0) * 2.0f - node(1);
      if (!closed_ && i >= n) return node(n - 1) * 2.0f - node(n - 2);
      return node(i);
   }

   // Barry and Goldman's form of the centripetal (alpha 0.5) curve.
   V3 segment_point(int s, float t) const
   {
      const V3 p0 = control(s - 1), p1 = control(s), p2 = control(s + 1), p3 = control(s + 2);
      const auto knot = [](V3 a, V3 b) { return std::sqrt((std::max)(length(b - a), 1e-4f)); };
      const float t0 = 0.0f;
      const float t1 = t0 + knot(p0, p1);
      const float t2 = t1 + knot(p1, p2);
      const float t3 = t2 + knot(p2, p3);
      const float u = t1 + (t2 - t1) * t;
      const V3 a1 = p0 * ((t1 - u) / (t1 - t0)) + p1 * ((u - t0) / (t1 - t0));
      const V3 a2 = p1 * ((t2 - u) / (t2 - t1)) + p2 * ((u - t1) / (t2 - t1));
      const V3 a3 = p2 * ((t3 - u) / (t3 - t2)) + p3 * ((u - t2) / (t3 - t2));
      const V3 b1 = a1 * ((t2 - u) / (t2 - t0)) + a2 * ((u - t0) / (t2 - t0));
      const V3 b2 = a2 * ((t3 - u) / (t3 - t1)) + a3 * ((u - t1) / (t3 - t1));
      return b1 * ((t2 - u) / (t2 - t1)) + b2 * ((u - t1) / (t2 - t1));
   }

   std::vector<V3> points_;
   std::vector<float> table_;   // length from the start at each sample
   float length_ = 0.0f;
   bool closed_ = false;
};

// Where a camera flying `curve` looks when it looks the way it travels: a
// point a little way ahead, or past the end along the last stretch.
inline V3 ahead_on(const Curve& c, float distance, float lead)
{
   const V3 here = c.at(distance);
   if (c.closed() || distance + lead <= c.total()) return c.at(distance + lead);
   const V3 before = c.at(c.total() - lead);
   const V3 end = c.at(c.total());
   return here + normalize(end - before, { 0.0f, 0.0f, -1.0f }) * lead;
}

} // namespace scripted_camera
