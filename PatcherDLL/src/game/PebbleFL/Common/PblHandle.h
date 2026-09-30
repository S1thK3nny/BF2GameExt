#pragma once

#include <stdint.h>

// Base of every object a PblHandle can point at. mHandleId changes on every
// allocation, so a handle whose saved id no longer matches is stale.
struct PblHandled {
   void*    vfptr;
   uint32_t mHandleId;
};
static_assert(sizeof(PblHandled) == 0x8, "PblHandled");

// Where T's PblHandled base sits; specialised next to each T.
template <class T> struct PblHandledOffset;

template <class T>
struct PblHandle {
   T*       mObject;
   uint32_t mSavedHandleId;

   // The object, or null when empty or stale.
   T* Get() const
   {
      if (!mObject) return nullptr;
      const PblHandled* h = (const PblHandled*)((const char*)mObject + PblHandledOffset<T>::value);
      return h->mHandleId == mSavedHandleId ? mObject : nullptr;
   }
};
