#pragma once

// =============================================================================
// The step tracking behind the walker foot diagnostic (walker_foot_diag.cpp),
// free of the game so tests/walker_foot_diag_tests.cpp can drive it.
//
// It is fed one update of one foot at a time, as EntityWalker::
// DoFootImpactEffects saw it: the foot's height over the walker's origin
// going in and coming out, which is the drop BF2 tests, and whether BF2
// counted a landing. A step is one descent of the foot: from the update it
// starts coming down to the one it stops in, which is where a
// StompDetectionType 1 walker counts its landing.
// =============================================================================

namespace walker_foot_diag {

// Dropping less than this in one update is standing still: the walker's body
// rocking moves a planted foot that much.
constexpr float kMoving = 0.001f;
// A descent ends after this many updates in a row without dropping, so a foot
// that pauses for one update on the way down is still one step.
constexpr int kStillUpdates = 2;
// A descent shorter than this is the body bobbing, not a step: it is reported
// only if BF2 counted a landing in it.
constexpr float kMinStep = 0.02f;

// One update of one foot.
struct FootUpdate {
   float before  = 0.0f;   // mLastFootHeight going in
   float after   = 0.0f;   // coming out: its height this update
   float lowest  = 0.0f;   // mMinFootHeight going in
   float seconds = 0.0f;   // how long the update was
   bool  counted = false;  // BF2 set the foot's landed bit
   bool  rearmed = false;  // type 1: BF2 cleared the foot's disarm bit
};

// One descent of one foot.
struct Step {
   float top            = 0.0f;   // the height it started down from
   float bottom         = 0.0f;   // and came down to
   float line           = 0.0f;   // type 0: its lowest plus StompThreshold, as it started
   float biggest        = 0.0f;   // the largest drop in one update
   float biggestSeconds = 0.0f;   // that update's length
   int   updates        = 0;      // updates it dropped in
   float seconds        = 0.0f;   // and how long they took
   bool  counted        = false;  // BF2 counted a landing in it
   bool  rearmed        = false;  // type 1: BF2 re-armed the foot in it

   float drop() const { return top - bottom; }
};

enum class Event {
   kNone,
   kStep,        // a step ended this update: step() has it
   kStrayCount,  // BF2 counted a landing while the foot was not coming down
};

class FootTracker {
public:
   Event update(const FootUpdate& u, float threshold)
   {
      const float drop = u.before - u.after;
      if (drop > kMoving) {
         if (!mDown) {
            mDown = true;
            mStep = Step{};
            mStep.top = u.before;
            mStep.line = u.lowest + threshold;
         }
         mStill = 0;
         mStep.bottom = u.after;
         ++mStep.updates;
         mStep.seconds += u.seconds;
         if (drop > mStep.biggest) {
            mStep.biggest = drop;
            mStep.biggestSeconds = u.seconds;
         }
         mStep.counted = mStep.counted || u.counted;
         mStep.rearmed = mStep.rearmed || u.rearmed;
         return Event::kNone;
      }
      if (mDown) {
         // The updates it stops in count toward the step: a type 1 walker
         // counts its landing in the first update the foot drops less than 0.1.
         mStep.counted = mStep.counted || u.counted;
         mStep.rearmed = mStep.rearmed || u.rearmed;
         if (++mStill < kStillUpdates) return Event::kNone;
         mDown = false;
         mStill = 0;
         return mStep.drop() >= kMinStep || mStep.counted ? Event::kStep : Event::kNone;
      }
      return u.counted ? Event::kStrayCount : Event::kNone;
   }

   const Step& step() const { return mStep; }
   bool descending() const { return mDown; }

private:
   bool mDown  = false;
   int  mStill = 0;
   Step mStep;
};

} // namespace walker_foot_diag
