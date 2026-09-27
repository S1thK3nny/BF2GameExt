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

enum class Operation { Add, Subtract, Multiply, Divide, Min, Max };

inline bool parse_operation(const char* name, Operation& result)
{
   if (!name) return false;
   // Compare strings, not just hashes: a typo must not silently select an op.
   const char* names[] = { "Add", "Subtract", "Multiply", "Divide", "Min", "Max" };
   for (unsigned i = 0; i < 6; ++i) {
      const char* a = name;
      const char* b = names[i];
      while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { ++a; ++b; }
      if (!*a && !*b) { result = static_cast<Operation>(i); return true; }
   }
   return false;
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

// PblConfig::Data: hash, argument count, then 512 bytes of float bits/string
// offsets. Offsets are relative to args, not the beginning of Data.
struct Data {
   uint32_t id;
   uint32_t count;
   uint32_t args[128];

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
