# miniz

`miniz.c` and `miniz.h` are the unmodified amalgamated files from the official miniz 3.1.2 release:
https://github.com/richgel999/miniz/releases/tag/3.1.2 (`miniz-3.1.2.zip`).

SHA-256 of `miniz-3.1.2.zip`: `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`

BF2GameExt compiles it into the DLL for Galactic Conquest saves (`PatcherDLL/src/shell/gc_save_limit.cpp`), which deflate and inflate zlib streams the way the game does, without calling the game's own zlib 1.2.2. The build options are in `PatcherDLL/src/util/miniz_config.h` and the file is compiled through `PatcherDLL/src/util/miniz_impl.c`. miniz is under the MIT license in `LICENSE`, the same as BF2GameExt.

To update: drop in `miniz.c` and `miniz.h` from a newer release, update the version and hash above, and run `python tools/run_tests.py`.
