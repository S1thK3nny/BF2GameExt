// Standalone Win32 test; runs the real lua_resume stub (lua/lua_coroutines.cpp)
// against a stand-in lua_resume. No game, DLL build or Detours required.
#define LUA_COROUTINES_TEST
#include "../PatcherDLL/src/lua/lua_coroutines.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

using Resume = int(__cdecl*)(void* co, int nargs);

// The engine's LuaHelper::mState, as the stub sees it.
void* g_mainState = reinterpret_cast<void*>(0x1000);
void* g_global = g_mainState;
void* g_seen = nullptr;
int   g_calls = 0;

Resume stub() { return reinterpret_cast<Resume>(&lua_coroutines::resume_stub); }

// Stands in for lua_resume: the global must be the coroutine being resumed.
// Seven arguments make it resume another coroutine from inside itself.
int __cdecl fake_resume(void* co, int nargs)
{
   ++g_calls;
   g_seen = g_global;
   assert(g_global == co);
   if (nargs == 7) {
      const int r = stub()(reinterpret_cast<void*>(0x3000), 1);
      assert(r == 10);
      assert(g_global == co);   // the inner one is over: back to this one
   }
   return nargs * 10;
}

} // namespace

int main()
{
   lua_coroutines::s_mState = &g_global;
   lua_coroutines::s_resume = reinterpret_cast<void*>(&fake_resume);

   // A resume: the coroutine is the global for the call, the main state after,
   // and the status comes back.
   int r = stub()(reinterpret_cast<void*>(0x2000), 3);
   assert(r == 30 && g_seen == reinterpret_cast<void*>(0x2000) && g_global == g_mainState);

   // A coroutine resuming another: each restores the one before it.
   g_calls = 0;
   r = stub()(reinterpret_cast<void*>(0x2000), 7);
   assert(r == 70 && g_calls == 2 && g_global == g_mainState);

   // Whatever the global was is what it goes back to, not the main state.
   g_global = reinterpret_cast<void*>(0x4000);
   r = stub()(reinterpret_cast<void*>(0x5000), 2);
   assert(r == 20 && g_global == reinterpret_cast<void*>(0x4000));
   g_global = g_mainState;

   // ECX and EDX come back as they went in, whatever lua_resume did to them,
   // and the stack is balanced: the caller pops the two arguments.
   uint32_t ecxOut = 0, edxOut = 0, espBefore = 0, espAfter = 0;
   int status = 0;
   void* co = reinterpret_cast<void*>(0x6000);
   void* fn = reinterpret_cast<void*>(&lua_coroutines::resume_stub);
   __asm {
      mov  espBefore, esp
      mov  ecx, 0x11111111
      mov  edx, 0x22222222
      push 5
      push co
      call fn
      add  esp, 8
      mov  ecxOut, ecx
      mov  edxOut, edx
      mov  status, eax
      mov  espAfter, esp
   }
   assert(ecxOut == 0x11111111u && edxOut == 0x22222222u);
   assert(status == 50 && espAfter == espBefore && g_global == g_mainState);

   std::printf("lua_resume stub: the coroutine is the global for the call and the state before it after, "
               "nested resumes restore in turn, ECX/EDX/ESP kept, status passed back\n");
   return 0;
}
