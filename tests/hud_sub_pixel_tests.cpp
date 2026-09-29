// Standalone Win32 test; runs the real stand-in the DLL puts in place of the
// interface draw's two floor calls. No game, DLL build or Detours required.
#define HUD_SUB_PIXEL_TEST
#include "../PatcherDLL/src/render/hud_sub_pixel.cpp"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace {

using Floor = double(__cdecl*)(double);

double __cdecl stock_floor(double v) { return std::floor(v); }

// One axis of the draw's rounding as retail does it: the coordinate plus 0.5
// in single precision, widened to a double for the call, and the result
// stored back into the matrix as a float.
float draw(Floor f, float v) { return static_cast<float>(f(static_cast<double>(v + 0.5f))); }

// A position of either sign from 0.5 to 16384 pixels, with a random power of
// two and every fraction bit random, so fractions of every length turn up.
float any_position(std::mt19937& rng)
{
   const uint32_t a = rng(), b = rng();
   const uint32_t bits = (a & 0x807FFFFFu) | ((126u + b % 15u) << 23);
   float v;
   std::memcpy(&v, &bits, sizeof(v));
   return v;
}

// Every position comes back as it went in, give or take the rounding the
// draw's own add already did where 0.5 carried it into the next power of two:
// at most half a float step at 16384, 2^-10 of a pixel. Returns how many came
// back exactly.
int sweep(Floor keep, int count, unsigned seed)
{
   std::mt19937 rng(seed);
   int exact = 0;
   for (int i = 0; i < count; ++i) {
      const float v = any_position(rng);
      const float out = draw(keep, v);
      assert(std::fabs(out - v) <= 0x1.0p-10f);
      exact += out == v;
   }
   return exact;
}

} // namespace

int main()
{
   const Floor keep = &keep_fraction;

   // The stock call snaps to the nearest pixel; the stand-in keeps the fraction.
   assert(draw(stock_floor, 100.3f) == 100.0f && draw(keep, 100.3f) == 100.3f);
   assert(draw(stock_floor, 100.7f) == 101.0f && draw(keep, 100.7f) == 100.7f);
   assert(draw(stock_floor, -0.6f) == -1.0f && draw(keep, -0.6f) == -0.6f);
   assert(draw(keep, 0.0f) == 0.0f && draw(keep, 1920.25f) == 1920.25f);
   assert(keep(2.5) == 2.0 && keep(0.5) == 0.0);

   // The one inexact case: adding 0.5 carries into the next power of two,
   // where floats are twice as far apart, and the add itself rounds.
   const float edge = std::nextafter(1024.0f, 0.0f);
   assert(draw(keep, edge) == 1024.0f);
   // Below half a pixel the add keeps 24 bits of the sum, not of the position.
   for (float tiny : { 0.3f, -0.2f, 1e-3f, -1e-6f, 1e-30f })
      assert(std::fabs(draw(keep, tiny) - tiny) <= 0x1.0p-25f);

   const int count = 200000;
   const int exact = sweep(keep, count, 1234);
   assert(exact > count * 9 / 10);

   // Again under the 24-bit x87 precision Direct3D sets unless told not to.
   unsigned int original = 0, unused = 0;
   _controlfp_s(&original, 0, 0);
   _controlfp_s(&unused, _PC_24, _MCW_PC);
   const int exact24 = sweep(keep, count, 5678);
   assert(exact24 > count * 9 / 10);
   assert(draw(keep, 100.3f) == 100.3f && draw(keep, edge) == 1024.0f);
   _controlfp_s(&unused, original, _MCW_PC);

   // The x87 stack is left as it was found. A value left behind on each call
   // would fill the eight registers within nine calls and turn results NaN.
   double sum = 0.0;
   for (int i = 0; i < 100000; ++i) sum += keep(i + 0.5);
   assert(sum == 4999950000.0);

   std::printf("keep_fraction: %d and %d of %d random positions exact (53- and 24-bit x87), the "
               "rest, where 0.5 carried a power of two, within 2^-10 px; x87 stack balanced over "
               "100000 calls\n", exact, exact24, count);
   return 0;
}
