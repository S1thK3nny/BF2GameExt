#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

// PblList<T>::Node — doubly-linked list node (16 bytes)
// Used by PblList template throughout the engine.
// Source: c:\battlefront2\main\battlefront2\source\Factory.h
template<typename T = void>
struct PblListNode {
    void*            _pList;    // +0x00  back-pointer to owning PblList
    PblListNode*     _pNext;    // +0x04
    PblListNode*     _pPrev;    // +0x08
    T*               _pObject;  // +0x0C  payload pointer
};
static_assert(sizeof(PblListNode<void>) == 0x10);

} // namespace game
