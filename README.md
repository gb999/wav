# wav

Single-header C library for reading and writing WAV (RIFF and RF64).

- `wav.h`: containers, chunks, format, header, frames. Define `WAV_IMPLEMENTATION` in one `.c` file.
- `wav_convert.h`: optional PCM <-> float/double (1 to 64 bits), interleave, deinterleave. Includes `wav.h`.

Everything else is in `misc/`: tests, benchmarks, an example, and the CMake project that builds them.
The headers, tests and benchmarks are standard C11. Only the example needs an OS (`misc/example_platform.h`
wraps files, threads and memory maps for POSIX and Windows).

```
cmake -S misc -B build
cmake --build build
ctest --test-dir build --output-on-failure      # unit tests and the example
```

| Option | Effect |
|---|---|
| `-DWAV_SANITIZE=ON` | AddressSanitizer and UBSan for the tests |
| `-DWAV_WERROR=ON` | warnings are errors |
| `-DWAV_COVERAGE=ON` | clang only. Adds `coverage` (fails unless every region, line and branch of the headers is hit) and `coverage-html` |

```
cmake -S misc -B build/cov -DCMAKE_C_COMPILER=clang -DWAV_COVERAGE=ON
cmake --build build/cov --target coverage
cmake --build build --target bench              # benchmarks, built with optimisation
```

Coverage needs `llvm-cov` and `llvm-profdata` from the same LLVM as the compiler. They are found next to it, or set
`-DWAV_LLVM_COV=...` and `-DWAV_LLVM_PROFDATA=...`. Building also compiles both headers strictly in C and, if there is a
C++ compiler, in C++.

CI (`.github/workflows/ci.yml`) is manual: Actions > ci > Run workflow, then tick Linux, macOS and/or Windows.
