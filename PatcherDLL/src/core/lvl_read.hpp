#pragma once

// =============================================================================
// LoadUtil::ReadDataFile — the engine's own .lvl reader
// =============================================================================
// This is the loader that dispatches *every* chunk type in a lvl, sounds
// included:
//
//   ReadDataFile (steam 0x00579c30) -> ReadDataFileOnHeap 0x00579930
//     -> chunk dispatcher 0x00579210 -> Snd chunk reader 0x00734170
//     -> SoundProperties ctor 0x00739b70, which links the object into the list
//        at 0x007e36f8 that Snd::Sound::Properties::FindByHashID 0x00739d90 walks
//
// `LoadDisplay::LoadDataFile` is NOT a substitute for it. That function walks
// only 'modl' / 'tex_' / 'skel' / 'load' — verified on both modtools 0x0067dea0
// and steam 0x005776e0 — so a lvl opened through it never registers sounds, no
// matter what is inside it.
//
// `name` must include the .lvl extension. It is resolved the way script-side
// ReadDataFile resolves it, which is NOT relative to the working directory:
// ReadDataFileOnHeap (steam 0x00579ac1) builds the real path itself —
//
//   "dc:…"        -> DownloadableContent::GetContentDirectory()
//   "raw:…"       -> used as-is
//   "mission.lvl" -> the DLC dir when DLC is active
//   anything else -> "data\_lvl_pc\" + name
//
// so pass a bare name (`"prone.lvl"`), never one you have already prefixed with
// `data\_lvl_pc\` — that resolves to `data\_lvl_pc\data\_lvl_pc\…` and fails.
// A `;` in the name separates a second argument the reader parses off.
//
// Returns false when the file could not be opened, which is a clean failure —
// the engine tears down its PblFile and returns, no dialog.

bool lvl_read_data_file(const char* name);
