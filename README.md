# ROOT RNTuple split codec — standalone performance research

**Status: research-only. No upstream patch proposed.**

## Scope

Static source studied: [`tree/ntuple/src/RColumnElement.hxx`](https://github.com/root-project/root/blob/master/tree/ntuple/src/RColumnElement.hxx), in particular `CastSplitPack` / `CastSplitUnpack`. For context, `RField<double>` defaults to `kSplitReal64` and `RIntegralField<uint64_t>` defaults to `kSplitUInt64`.

The benchmark extracts the same-width little-endian byte-split reference logic and compares:

- `reference`: element-first loops (mirrors upstream on little-endian same-width data).
- `planes`: byte-plane-first loops.
- `tiles128`: byte-plane-first loops within blocks of 128 elements.
- `guarded64`: plane-first only for 64-bit, reference for all other widths; decoder unchanged.
- `shift64`: reconstruct plane bytes via bit shifts for 64-bit; decoder unchanged.
- `explicit64`: fully written-out shift and plane stores for 64-bit; decoder unchanged.

## CI results

- [First pass, x86-64 GCC/Clang at O2/O3](https://github.com/Johnny-Kao/root-rntuple-codec-perf-lab/actions/runs/41110017829)
- [Second pass, x86-64 and ARM64 at O2/O3](https://github.com/Johnny-Kao/root-rntuple-codec-perf-lab/actions/runs/41110342960)
- [Third pass, six algorithms, x86-64 and ARM64](https://github.com/Johnny-Kao/root-rntuple-codec-perf-lab/actions/runs/41110653814)

All final-run jobs passed: sanitizer/correctness (ASan+UBSan), GCC/Clang times O2/O3 times x86-64/ARM64. Cross-checked bit-exact encoded bytes against the reference, including floating-point raw bits and short/non-power-of-two lengths.

Measured `guarded64` 64-bit **Pack** speedup over `reference` on 4,096–262,144 elements, summarized as geometric means over integer/float 64-bit cases and three larger lengths (greater than 1 = faster):

| Runner | GCC O2 | GCC O3 | Clang O2 | Clang O3 |
| --- | ---: | ---: | ---: | ---: |
| x86-64 | 1.79x | 2.76x | 1.66x | 1.26x |
| ARM64 | 3.15x | 1.79x | **0.63x** | **0.62x** |

The ARM64/Clang regression is a **blocking counterexample** to a portable, unconditional change. `shift64` and `explicit64` preserve current ARM64/Clang speed (~1.00x) but offer inconsistent improvements elsewhere.

Do **not** interpret these numbers as end-to-end ROOT speedup. The benchmark isolates uncompressed same-width codec kernels. It does not include real RNTuple writing, compression, I/O, memory pressure, field conversion/narrowing, big-endian handling, or independent hardware repetitions.

## Engineering decision

**Do not submit an upstream PR from these results.** An unconditional loop interchange regresses on ARM64/Clang; preserving portable behavior with compiler-specific special casing adds maintenance burden without a demonstrated end-to-end gain. A future attempt should first profile actual RNTuple workloads, verify deployment compiler/architecture distribution, then run an integrated A/B benchmark.

The earlier unrelated security issue and its reproducer are **not** part of this public repository.

Tests run exclusively in GitHub Actions. No local Mac builds or tests. No changes to `root-project/root`.
