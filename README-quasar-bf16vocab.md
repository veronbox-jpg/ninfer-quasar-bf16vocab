# NInfer — `quasar-bf16vocab` branch (Windows)

Windows build branch of [Neroued/ninfer](https://github.com/Neroued/ninfer) for the
[Qwen3.8-27B QUASAR NVFP4 · BF16 vocab · DFlash2](https://huggingface.co/Kuramor/Qwen3.8-27B-QUASAR-NVFP4-BF16vocab-DFlash2-NInfer)
model artifact.

Base: upstream `5b4303c0ea0e8ab2be3efa54a677829f3edab6e5`. Three changes on top:

1. **MSVC/Windows build port** (22 modified + 2 new files; CMake to static cudart,
   `find_package(FFmpeg)`/`CURL` instead of PkgConfig, Winsock, `u128.h` for
   `unsigned __int128`, TMA descriptor port, ...). The port follows the
   [natpate/ninfer-windows](https://github.com/natpate/ninfer-windows) fork and its
   documented approach; the TMA files were re-resolved against this upstream commit
   (upstream had changed the kernels between the port's base and `5b4303c0`), and the
   Python artifact reader/writer got Win32 `ReadFile`/`WriteFile` positioned I/O
   (`tools/artifact/file_io.py` — the CRT fd path on the build host returned short,
   wrong reads for files above ~1 GB; the POSIX branch is untouched).
2. **BF16 linear shape 248320×5120** for the full-precision vocabulary head — the same
   patch the model repo ships as `engine/bf16-vocab-head.patch`.
3. (tools) Windows `ReadFile`/`WriteFile` in the Python artifact `file_io` (see 1).

## Building (Windows)

Requirements: Visual Studio 2022 (MSVC 14.4x, `vcvars64.bat`), CUDA toolkit 13.1+
(built and tested with 13.3), CMake, Ninja, `sm_120a` GPU (NVIDIA Blackwell), vcpkg
(curl / ffmpeg+zlib / pkgconf — declared in `vcpkg.json`).

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=120a ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DCMAKE_CUDA_COMPILER=<cuda>/bin/nvcc.exe -DBUILD_TESTING=OFF
cmake --build build -j 8
```

Outputs land in `build\apps\`: `ninfer-serve.exe`, `ninfer.exe`,
`ninfer-perplexity.exe` + runtime DLLs.

## Prebuilt binaries

The GitHub release for this branch ships `ninfer-quasar-bf16vocab-win64-cuda13.zip`
(exes + DLLs) with a SHA-256. Unsigned build. Linux: use upstream at the pinned
commit plus the model repo's `engine/bf16-vocab-head.patch` (Linux is unaffected by
the Windows port; the tools change is a no-op there).

## License

Apache-2.0, same as upstream — see `LICENSE`.
