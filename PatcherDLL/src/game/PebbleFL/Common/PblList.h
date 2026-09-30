#pragma once

// Intrusive circular list. _head is the sentinel, so a walk runs from
// _head._pNext until it gets back to &_head.
template <class T>
struct PblList {
   struct Node {
      PblList* _pList;
      Node*    _pNext;
      Node*    _pPrev;
      T*       _pObject;
   };

   Node _head;
   int  _iCount;
};
