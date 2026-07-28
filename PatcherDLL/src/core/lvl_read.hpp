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

// =============================================================================
// Modder-facing lvl path resolution
// =============================================================================
// Normalizes a path a modder typed — in a Lua call or a LoadConfig key — into a
// single stem relative to `data\_lvl_pc\`, and confirms the file is there.
//
// Three accepted forms, all collapsing to that one stem:
//
//   "LOAD\\load.lvl"                        -> LOAD\load
//   "dc:LOAD\\load.lvl"                     -> ..\..\<addon>\Data\_lvl_pc\LOAD\load
//   "..\\..\\addon\\VTR\\data\\_LVL_PC\\LOAD\\load"  -> unchanged
//
// The trailing ".lvl" is optional and stripped when present, because every
// consumer of the stem appends it again.
//
// The `dc:` form is rewritten as a relative climb rather than passed through,
// even though ReadDataFileOnHeap understands `dc:` natively. Two reasons: the
// other consumer, `LoadDisplay::LoadDataFile`, goes through LoadUtil::MakeFullName
// and understands no prefixes at all; and one stem for both readers means one
// resolution per config key, not two that can disagree.
//
// On failure `outReason` receives a finished sentence naming the cause — callers
// supply their own prefix, severity and consequence, since "keep the previous
// value" and "skip this load" want different wording. Every out parameter is
// optional; `outFull` is only filled once the on-disk name is known.
enum class LvlPathStatus {
    Ok = 0,
    Empty,             // nothing left after stripping the prefix and extension
    NoContentDir,      // "dc:" given but no addon content is active
    OutsideWorkingDir, // addon dir is not under the working directory
    NotFound,          // resolved name is not on disk
};

LvlPathStatus lvl_resolve_data_path(const char* path,
                                    char* outStem,   size_t stemSize,
                                    char* outFull,   size_t fullSize,
                                    char* outReason, size_t reasonSize);
