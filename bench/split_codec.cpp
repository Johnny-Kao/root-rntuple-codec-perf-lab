// Isolated, non-sensitive benchmark of the byte-split routines in ROOT RNTuple.
// Reference: tree/ntuple/src/RColumnElement.hxx (CastSplitPack / CastSplitUnpack).
// Scope: same-width types on little-endian x86_64 only; no semantic modification.
// No local execution: tested by GitHub-hosted CI only.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

volatile std::uint64_t gSink = 0;

template <typename T>
NOINLINE void ReferencePack(void *destination, const void *source, std::size_t count)
{
   constexpr std::size_t N = sizeof(T);
   auto splitArray = reinterpret_cast<char *>(destination);
   auto src = reinterpret_cast<const T *>(source);
   for (std::size_t i = 0; i < count; ++i) {
      T val = src[i];
      for (std::size_t b = 0; b < N; ++b)
         splitArray[b * count + i] = reinterpret_cast<const char *>(&val)[b];
   }
}

template <typename T>
NOINLINE void ReferenceUnpack(void *destination, const void *source, std::size_t count)
{
   constexpr std::size_t N = sizeof(T);
   auto dst = reinterpret_cast<T *>(destination);
   auto splitArray = reinterpret_cast<const char *>(source);
   for (std::size_t i = 0; i < count; ++i) {
      T val{};
      for (std::size_t b = 0; b < N; ++b)
         reinterpret_cast<char *>(&val)[b] = splitArray[b * count + i];
      dst[i] = val;
   }
}

// Candidate A: loop interchange, full planes for sequential target byte stores.
template <typename T>
NOINLINE void PlanePack(void *destination, const void *source, std::size_t count)
{
   constexpr std::size_t N = sizeof(T);
   auto *dst = reinterpret_cast<unsigned char *>(destination);
   auto *src = reinterpret_cast<const unsigned char *>(source);
   for (std::size_t b = 0; b < N; ++b)
      for (std::size_t i = 0; i < count; ++i)
         dst[b * count + i] = src[i * N + b];
}

template <typename T>
NOINLINE void PlaneUnpack(void *destination, const void *source, std::size_t count)
{
   constexpr std::size_t N = sizeof(T);
   auto *dst = reinterpret_cast<unsigned char *>(destination);
   auto *src = reinterpret_cast<const unsigned char *>(source);
   for (std::size_t b = 0; b < N; ++b)
      for (std::size_t i = 0; i < count; ++i)
         dst[i * N + b] = src[b * count + i];
}

// Candidate B: tile planes so all accesses to a chunk stay in cache.
template <typename T>
NOINLINE void TilePack(void *destination, const void *source, std::size_t count)
{
   constexpr std::size_t N = sizeof(T);
   constexpr std::size_t kTile = 128;
   auto *dst = reinterpret_cast<unsigned char *>(destination);
   auto *src = reinterpret_cast<const unsigned char *>(source);
   for (std::size_t start = 0; start < count; start += kTile) {
      const auto end = std::min(start + kTile, count);
      for (std::size_t b = 0; b < N; ++b)
         for (std::size_t i = start; i < end; ++i)
            dst[b * count + i] = src[i * N + b];
   }
}

template <typename T>
NOINLINE void TileUnpack(void *destination, const void *source, std::size_t count)
{
   constexpr std::size_t N = sizeof(T);
   constexpr std::size_t kTile = 128;
   auto *dst = reinterpret_cast<unsigned char *>(destination);
   auto *src = reinterpret_cast<const unsigned char *>(source);
   for (std::size_t start = 0; start < count; start += kTile) {
      const auto end = std::min(start + kTile, count);
      for (std::size_t b = 0; b < N; ++b)
         for (std::size_t i = start; i < end; ++i)
            dst[i * N + b] = src[b * count + i];
   }
}

// Conservative dispatch: only use reordered planes for same-width 64-bit data.
// Deliberately preserve the reference decoder and 32-bit encoder paths.
template <typename T>
NOINLINE void GuardedPack(void *destination, const void *source, std::size_t count)
{
   if constexpr (sizeof(T) == 8)
      PlanePack<T>(destination, source, count);
   else
      ReferencePack<T>(destination, source, count);
}

// Candidate C: compute byte planes by shifts; avoids strided source loads.
// Only 64-bit same-type data. Other widths preserve reference implementation.
template <typename T>
NOINLINE void ShiftPack(void *destination, const void *source, std::size_t count)
{
   if constexpr (sizeof(T) != 8) {
      ReferencePack<T>(destination, source, count);
   } else {
      auto *dst = reinterpret_cast<unsigned char *>(destination);
      auto *src = reinterpret_cast<const T *>(source);
      for (std::size_t i = 0; i < count; ++i) {
         std::uint64_t bits = 0;
         std::memcpy(&bits, &src[i], 8);
         for (std::size_t b = 0; b < 8; ++b)
            dst[b * count + i] = static_cast<unsigned char>(bits >> (8 * b));
      }
   }
}

// Candidate D: shift-based pack with explicit plane pointers, no inner loop.
template <typename T>
NOINLINE void ExplicitPack(void *destination, const void *source, std::size_t count)
{
   if constexpr (sizeof(T) != 8) {
      ReferencePack<T>(destination, source, count);
   } else {
      auto *p0 = reinterpret_cast<unsigned char *>(destination);
      auto *p1 = p0 + count, *p2 = p1 + count, *p3 = p2 + count;
      auto *p4 = p3 + count, *p5 = p4 + count, *p6 = p5 + count, *p7 = p6 + count;
      auto *src = reinterpret_cast<const T *>(source);
      for (std::size_t i = 0; i < count; ++i) {
         std::uint64_t v = 0;
         std::memcpy(&v, &src[i], 8);
         p0[i] = static_cast<unsigned char>(v);
         p1[i] = static_cast<unsigned char>(v >> 8);
         p2[i] = static_cast<unsigned char>(v >> 16);
         p3[i] = static_cast<unsigned char>(v >> 24);
         p4[i] = static_cast<unsigned char>(v >> 32);
         p5[i] = static_cast<unsigned char>(v >> 40);
         p6[i] = static_cast<unsigned char>(v >> 48);
         p7[i] = static_cast<unsigned char>(v >> 56);
      }
   }
}

template <typename T>
using Kernel = void (*)(void *, const void *, std::size_t);

template <typename T>
struct KernelSpec {
   const char *name;
   Kernel<T> pack;
   Kernel<T> unpack;
};

template <typename T>
constexpr std::array<KernelSpec<T>, 6> kKernels = {{
   {"reference", ReferencePack<T>, ReferenceUnpack<T>},
   {"planes", PlanePack<T>, PlaneUnpack<T>},
   {"tiles128", TilePack<T>, TileUnpack<T>},
   {"guarded64", GuardedPack<T>, ReferenceUnpack<T>},
   {"shift64", ShiftPack<T>, ReferenceUnpack<T>},
   {"explicit64", ExplicitPack<T>, ReferenceUnpack<T>},
}};

template <typename T>
void FillRandom(std::vector<T> &values, std::uint64_t seed)
{
   std::mt19937_64 rng(seed);
   for (auto &x : values) {
      const std::uint64_t bits = rng();
      std::memcpy(&x, &bits, sizeof(T));
   }
}

template <typename T>
void Check(std::size_t n)
{
   std::vector<T> input(n), restored(n);
   std::vector<std::uint8_t> encoded(n * sizeof(T)), expected(n * sizeof(T));
   FillRandom(input, 12345 + n * 177 + sizeof(T));
   ReferencePack<T>(expected.data(), input.data(), n);
   for (auto &kernel : kKernels<T>) {
      std::fill(encoded.begin(), encoded.end(), 0xa5);
      kernel.pack(encoded.data(), input.data(), n);
      if (encoded != expected)
         throw std::runtime_error(std::string("pack mismatch ") + kernel.name + " n=" + std::to_string(n));
      // Cross-check both reference and optimized decoders.
      kernel.unpack(restored.data(), expected.data(), n);
      if (std::memcmp(restored.data(), input.data(), n * sizeof(T)) != 0)
         throw std::runtime_error(std::string("unpack mismatch ") + kernel.name + " n=" + std::to_string(n));
   }
}

template <typename T>
double Measure(Kernel<T> fn, void *dst, const void *src, std::size_t n, int reps)
{
   const auto t0 = std::chrono::steady_clock::now();
   for (int k = 0; k < reps; ++k)
      fn(dst, src, n);
   const auto t1 = std::chrono::steady_clock::now();
   const auto *out = static_cast<const unsigned char *>(dst);
   if (n) gSink = gSink + out[(static_cast<unsigned>(reps) * 13U) % (n * sizeof(T))];
   return std::chrono::duration<double, std::nano>(t1 - t0).count() / (static_cast<double>(n) * reps);
}

template <typename T>
void Benchmark(std::string type, std::size_t n)
{
   std::vector<T> input(n), decoded(n);
   std::vector<std::uint8_t> encoded(n * sizeof(T)), scratch(n * sizeof(T));
   FillRandom(input, 45234 + n);
   ReferencePack<T>(encoded.data(), input.data(), n);

   const int reps = std::clamp(static_cast<int>(4000000 / n), 8, 30000);
   std::array<std::vector<double>, 6> packTimes, unpackTimes;
   std::array<int, 6> order{{0, 1, 2, 3, 4, 5}};
   std::mt19937 orderRng(5599 + static_cast<unsigned>(n));
   for (int trial = 0; trial < 9; ++trial) {
      std::shuffle(order.begin(), order.end(), orderRng);
      for (int k : order) {
         auto &fn = kKernels<T>[k];
         packTimes[k].push_back(Measure<T>(fn.pack, scratch.data(), input.data(), n, reps));
         unpackTimes[k].push_back(Measure<T>(fn.unpack, decoded.data(), encoded.data(), n, reps));
      }
   }
   const auto median = [](std::vector<double> &v) {
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
   };
   double baselinePack = median(packTimes[0]), baselineUnpack = median(unpackTimes[0]);
   for (int k = 0; k < 6; ++k) {
      double p = median(packTimes[k]), u = median(unpackTimes[k]);
      std::cout << std::fixed << std::setprecision(3)
         << "RESULT type=" << type
         << " n=" << n
         << " kernel=" << kKernels<T>[k].name
         << " pack_ns_per_elem=" << p
         << " pack_speedup=" << baselinePack / p
         << " unpack_ns_per_elem=" << u
         << " unpack_speedup=" << baselineUnpack / u
         << '\n';
   }
}

template <typename T>
void Run(std::string type)
{
   for (std::size_t n : {0U, 1U, 2U, 3U, 4U, 7U, 8U, 9U, 15U, 16U, 31U, 32U, 33U, 63U,
                        64U, 65U, 127U, 128U, 129U, 255U, 256U, 257U, 1023U, 4096U, 65536U})
      Check<T>(n);
   std::cout << "CORRECTNESS PASS type=" << type << '\n';
#ifndef CHECK_ONLY
   for (std::size_t n : {256U, 4096U, 65536U, 262144U})
      Benchmark<T>(type, n);
#endif
}

int main()
{
   static_assert(sizeof(std::uint32_t) == 4 && sizeof(std::uint64_t) == 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
   throw std::runtime_error("This microbenchmark requires little-endian");
#endif
   Run<std::uint32_t>("u32");
   Run<std::uint64_t>("u64");
   Run<float>("f32");
   Run<double>("f64");
   std::cout << "ALL TESTS PASS checksum=" << gSink << '\n';
}
