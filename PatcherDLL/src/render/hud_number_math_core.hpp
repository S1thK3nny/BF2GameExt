#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// Engine-independent, also used by the standalone tests. HUD numbers are floats;
// calculate in double so a finite clamp can rescue an overflowing float result.
namespace hud_number_math {

constexpr uint32_t hash(const char* text)
{
   uint32_t value = 2166136261u;
   while (*text) value = (value ^ (static_cast<unsigned char>(*text++) | 0x20u)) * 16777619u;
   return value;
}

// Case-insensitive whole-name match, or -1. Compare strings, not just hashes:
// a typo must not silently select an operation.
inline int find_name(const char* name, const char* const* names, unsigned count)
{
   if (!name) return -1;
   for (unsigned i = 0; i < count; ++i) {
      const char* a = name;
      const char* b = names[i];
      while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { ++a; ++b; }
      if (!*a && !*b) return static_cast<int>(i);
   }
   return -1;
}

enum class Operation { Add, Subtract, Multiply, Divide, Min, Max };

inline bool parse_operation(const char* name, Operation& result)
{
   const char* names[] = { "Add", "Subtract", "Multiply", "Divide", "Min", "Max" };
   const int i = find_name(name, names, 6);
   if (i < 0) return false;
   result = static_cast<Operation>(i);
   return true;
}

enum class Comparison { Greater, GreaterOrEqual, Less, LessOrEqual, Equal, NotEqual };

inline bool parse_comparison(const char* name, Comparison& result)
{
   const char* names[] = { "Greater", "GreaterOrEqual", "Less", "LessOrEqual", "Equal", "NotEqual" };
   const int i = find_name(name, names, 6);
   if (i < 0) return false;
   result = static_cast<Comparison>(i);
   return true;
}

struct Config {
   Operation operation = Operation::Add;
   bool clamp = false;
   float minimum = 0.0f;
   float maximum = 1.0f;
};

inline bool valid(const Config& config)
{
   return !config.clamp || (std::isfinite(config.minimum) && std::isfinite(config.maximum)
                           && config.minimum <= config.maximum);
}

inline bool calculate(const Config& config, double a, double b, float& output)
{
   if (!std::isfinite(a) || !std::isfinite(b) || !valid(config)) return false;
   double result = 0;
   switch (config.operation) {
   case Operation::Add:      result = a + b; break;
   case Operation::Subtract: result = a - b; break;
   case Operation::Multiply: result = a * b; break;
   case Operation::Divide:
      if (b == 0.0) return false; // Includes -0.0; tiny, nonzero divisors are valid.
      result = a / b;
      break;
   case Operation::Min: result = a < b ? a : b; break;
   case Operation::Max: result = a > b ? a : b; break;
   default: return false;
   }
   if (!std::isfinite(result)) return false;
   if (config.clamp) {
      if (result < config.minimum) result = config.minimum;
      if (result > config.maximum) result = config.maximum;
   }
   constexpr double limit = (std::numeric_limits<float>::max)();
   if (result < -limit || result > limit) return false;
   output = static_cast<float>(result);
   return true;
}

inline bool numeric_event(uint32_t type, uint32_t bits, double& value)
{
   switch (type) {
   case 2: {
      int32_t number;
      std::memcpy(&number, &bits, sizeof(number));
      value = number;
      return true;
   }
   case 3: value = bits; return true;
   case 4: {
      float number;
      std::memcpy(&number, &bits, sizeof(number));
      if (!std::isfinite(number)) return false;
      value = number;
      return true;
   }
   default: return false; // Bool is deliberately not a numeric operand.
   }
}

struct State {
   Config config;
   double values[2]{};
   bool known[2]{};
   bool sent = false;
   float previous = 0.0f;

   void input(unsigned operand, bool acceptable, double value)
   {
      if (operand > 1) return;
      known[operand] = acceptable && std::isfinite(value);
      values[operand] = value;
   }

   // Invalid/missing input holds the last published result. A subsequent valid
   // input recovers normally. No NaNs, fabricated zeros, or per-frame resends.
   bool next(float& output)
   {
      if (!known[0] || !known[1] || !calculate(config, values[0], values[1], output)) return false;
      if (sent && previous == output) return false;
      previous = output;
      sent = true;
      return true;
   }
};

// TransformNumberLerp: output = A + (B - A) * s. s follows where the input sits
// in InputRange (0 to 1 unless set), clamped to 0..1, at no more than 1 / RiseTime
// per second on the way up and 1 / FallTime on the way down; a time of 0 follows
// at once. A and B default to 0 and 1, so an input with only times set is just
// eased.
struct Lerp {
   double ends[2]  = { 0.0, 1.0 };   // A, B
   bool   known[2] = { true, true };
   double range[2] = { 0.0, 1.0 };   // InputRange: the inputs that give A and B
   bool   rangeKnown[2] = { true, true };
   float  riseTime = 0.0f;
   float  fallTime = 0.0f;
   double raw      = 0.0;            // the input as it arrived
   bool   rawKnown = false;
   double target   = 0.0;
   double s        = 0.0;
   bool   targetKnown = false;
   bool   started  = false;
   bool   sent     = false;
   float  previous = 0.0f;

   void input(bool acceptable, double value)
   {
      raw = value;
      rawKnown = acceptable && std::isfinite(value);
      retarget();
   }

   // An end of InputRange: 0 is the input that gives A, 1 the one that gives B.
   // A reversed range is fine; one with no width holds where it is.
   void bound(unsigned which, bool acceptable, double value)
   {
      if (which > 1) return;
      range[which] = value;
      rangeKnown[which] = acceptable && std::isfinite(value);
      retarget();
   }

   // The first place is taken at once, so nothing eases in at load. After that,
   // a direction with no time also moves at once, so a pulse only one update long
   // is never missed. An invalid input or range holds where it is.
   void retarget()
   {
      targetKnown = false;
      if (!rawKnown || !rangeKnown[0] || !rangeKnown[1] || range[0] == range[1]) return;
      const double t = (raw - range[0]) / (range[1] - range[0]);
      if (!std::isfinite(t)) return;
      target = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
      targetKnown = true;
      if (!started) { s = target; started = true; return; }
      if (target > s && !(riseTime > 0.0f)) s = target;
      if (target < s && !(fallTime > 0.0f)) s = target;
   }

   void end(unsigned which, bool acceptable, double value)
   {
      if (which > 1) return;
      known[which] = acceptable && std::isfinite(value);
      ends[which] = value;
   }

   // Moves s toward the input by dt seconds; true if it moved.
   bool advance(double dt)
   {
      if (!started || !targetKnown || !(dt > 0.0) || s == target) return false;
      if (s < target) s = riseTime > 0.0f ? std::fmin(target, s + dt / riseTime) : target;
      else            s = fallTime > 0.0f ? std::fmax(target, s - dt / fallTime) : target;
      return true;
   }

   // Like State::next: nothing until everything is known, no resend of an
   // unchanged value.
   bool next(float& output)
   {
      if (!started || !known[0] || !known[1]) return false;
      const double result = ends[0] + (ends[1] - ends[0]) * s;
      constexpr double limit = (std::numeric_limits<float>::max)();
      if (!std::isfinite(result) || result < -limit || result > limit) return false;
      output = static_cast<float>(result);
      if (sent && previous == output) return false;
      previous = output;
      sent = true;
      return true;
   }
};

// TransformNumberCompare: A against B, 1 or 0. Once on, Hysteresis keeps it on
// until A is that far back past B, so a value sitting on the line does not
// flicker; for Equal and NotEqual it is how close counts as equal.
struct Compare {
   Comparison op = Comparison::Greater;
   float  hysteresis = 0.0f;
   double values[2]{};
   bool   known[2]{};
   bool   on = false;
   bool   decided = false;

   void input(unsigned operand, bool acceptable, double value)
   {
      if (operand > 1) return;
      known[operand] = acceptable && std::isfinite(value);
      values[operand] = value;
   }

   bool evaluate() const
   {
      const double a = values[0], b = values[1], h = hysteresis;
      switch (op) {
      case Comparison::Greater:        return on ? a > b - h  : a > b;
      case Comparison::GreaterOrEqual: return on ? a >= b - h : a >= b;
      case Comparison::Less:           return on ? a < b + h  : a < b;
      case Comparison::LessOrEqual:    return on ? a <= b + h : a <= b;
      case Comparison::Equal:          return std::fabs(a - b) <= h;
      case Comparison::NotEqual:       return std::fabs(a - b) > h;
      }
      return false;
   }

   // True when the result is new: the first once both sides are known, then
   // each change. A missing or invalid side holds the last result.
   bool next(bool& result)
   {
      if (!known[0] || !known[1]) return false;
      const bool now = evaluate();
      if (decided && now == on) return false;
      on = now;
      decided = true;
      result = now;
      return true;
   }
};

// PblConfig::Data: hash, argument count, then 512 bytes of float bits/string
// offsets. Offsets are relative to args, not the beginning of Data.
struct Data {
   uint32_t id;
   uint32_t count;
   uint32_t args[128];

   // ConfigMunge keeps a comment that follows a property on its line: every
   // word becomes one more argument, unquoted ones hashed like names, so
   // "RiseTime(0.15) // fade" arrives as 0.15, hash("//"), hash("fade"). The
   // arguments end at the first hash("//"). No string offset has those bits,
   // and as a float they are about -5.7e-18, which nobody writes.
   unsigned arguments() const
   {
      if (count > 128) return count;
      for (unsigned i = 0; i < count; ++i)
         if (args[i] == hash("//")) return i;
      return count;
   }

   // A flag: a number, 0 off and anything else on, or an unquoted true or
   // false, which ConfigMunge hashes like a name. The engine's own GetBoolArg
   // only tests for 0.0, so there a hashed false reads as on; not here.
   bool flag(unsigned index, bool& value) const
   {
      if (count > 128 || index >= count || string(index)) return false;
      if (args[index] == hash("true"))  { value = true;  return true; }
      if (args[index] == hash("false")) { value = false; return true; }
      float number;
      if (!this->number(index, number)) return false;
      value = number != 0.0f;
      return true;
   }

   const char* string(unsigned index) const
   {
      if (count > 128 || index >= count) return nullptr;
      const uint32_t offset = args[index];
      if (offset < count * 4 || offset >= sizeof(args)) return nullptr;
      const char* start = reinterpret_cast<const char*>(args) + offset;
      return std::memchr(start, 0, sizeof(args) - offset) ? start : nullptr;
   }

   bool number(unsigned index, float& value) const
   {
      if (count > 128 || index >= count || string(index)) return false;
      std::memcpy(&value, &args[index], sizeof(value));
      return std::isfinite(value);
   }
};
static_assert(sizeof(Data) == 520, "Native PblConfig::Data size");

} // namespace hud_number_math
