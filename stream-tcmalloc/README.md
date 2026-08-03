# STREAM + TCMalloc (NUMA-aware)

This is the same STREAM C++ benchmark as [`../stream`](../stream) (Copy,
Scale, Add, Triad, decoupled C kernels compiled with OpenMP, C++ driver), but
the `a`/`b`/`c` STREAM arrays are allocated through Google's
[TCMalloc](https://github.com/google/tcmalloc) (built from the bundled
`tcmalloc` git submodule) instead of the platform's default `malloc`/`new`.

**TCMalloc itself is used completely unmodified**: not a single line inside
the `tcmalloc/` submodule is edited. `stream.cpp` and `stream_kernels.c` are
*also* completely unmodified from the plain [`../stream`](../stream)
variant -- they still just use `std::vector<double>`. All of the allocator
wiring lives in this directory's own `BUILD.bazel`/`MODULE.bazel`/Makefiles.
**You just build and run the binary**:

```sh
make -f makefiles/Makefile-hw
./stream.hw 10000000
```

## How this works

### Why Bazel

Unlike jemalloc (a plain autotools/`make` project), TCMalloc's own build
system is [Bazel](https://bazel.build/) (see
[`tcmalloc/README.md`](tcmalloc/README.md): "Bazel is the official build
system for TCMalloc. Experimental support for CMake is available as well.").
TCMalloc also depends on a specific, matched version of
[Abseil](https://abseil.io) (declared in
[`tcmalloc/MODULE.bazel`](tcmalloc/MODULE.bazel)) plus several other
libraries (protobuf, re2, googletest, benchmark, fuzztest -- the last three
only for TCMalloc's *own* test/fuzz suite). Re-deriving that dependency graph
by hand in a plain Makefile would mean hand-tracking Abseil compatibility
ourselves; using Bazel with
[Bzlmod](https://bazel.build/external/module) instead means we get exactly
the dependency versions TCMalloc itself was built and tested against, for
free, and:

* Because TCMalloc declares its test-only dependencies (googletest,
  benchmark, fuzztest, rules_fuzzing) as `dev_dependency = True` in its
  `MODULE.bazel`, Bzlmod *excludes them entirely* from the dependency graph
  when TCMalloc is consumed from another module (us) rather than built
  directly. Combined with the fact that the specific library target we need
  (`tcmalloc_numa_aware`, see below) only depends on Abseil, building it
  from this directory does **not** require fetching protobuf, re2,
  googletest, benchmark, or fuzztest at all.
* This is also why this directory does not attempt to hand-roll a CMake
  build against `tcmalloc/CMakeLists.txt`: that top-level file unconditionally
  configures TCMalloc's *entire* test/fuzz suite (`add_subdirectory(tcmalloc/testing)`)
  and therefore unconditionally needs protobuf, googletest, benchmark
  *and* fuzztest to even configure, regardless of which target you actually
  build.

`makefiles/Makefile-hw` (and `Makefile-hw+sve`) simply shell out to
`bazel build //:stream_tcmalloc` and then copy the resulting binary to
`stream.hw`, so the day-to-day workflow is identical to every other variant
in this repository.

### The NUMA-aware TCMalloc variant

TCMalloc's NUMA support is a **compile-time build variant**, not a runtime
flag on the default build: `tcmalloc/tcmalloc/common.h` only defines more
than one NUMA partition (`kNumaPartitions`) when the library is compiled
with `-DTCMALLOC_INTERNAL_NUMA_AWARE`. TCMalloc's own `BUILD` file expresses
this as a separate `cc_library` target,
[`//tcmalloc:tcmalloc_numa_aware`](tcmalloc/tcmalloc/BUILD), built with that
define. This is the target `BUILD.bazel` in this directory attaches to our
`stream_tcmalloc` binary via Bazel's
[`malloc` attribute](https://bazel.build/reference/be/c-cpp#cc_binary.malloc)
(the same mechanism used in
[`tcmalloc/docs/quickstart.md`](tcmalloc/docs/quickstart.md)):

```python
cc_binary(
    name = "stream_tcmalloc",
    srcs = ["stream.cpp", "stream_kernels.c"],
    malloc = "@tcmalloc//tcmalloc:tcmalloc_numa_aware",
    ...
)
```

At start-up, `tcmalloc_numa_aware`'s NUMA topology detection
([`tcmalloc/tcmalloc/internal/numa.cc`](tcmalloc/tcmalloc/internal/numa.cc))
automatically probes `/sys/devices/system/node/node<N>/cpulist` for every
`N`, with no configuration required, to:

1. Discover how many NUMA nodes the machine actually has, and which CPUs
   belong to each.
2. Create one heap "partition" per NUMA node (up to a small, fixed maximum
   compiled into TCMalloc).
3. Route each allocation to the partition matching whichever NUMA node the
   calling CPU currently belongs to (via `sched_getcpu()`), and use
   `mbind()` (in the default *advisory* binding mode -- see `NumaBindMode` in
   [`tcmalloc/tcmalloc/internal/numa.h`](tcmalloc/tcmalloc/internal/numa.h))
   to encourage the memory backing each partition to physically live on its
   corresponding node.

In other words, "automatically pick up the NUMA configuration" is a literal,
documented TCMalloc behavior here: it reads the system's real NUMA topology
straight from sysfs and partitions itself accordingly, without this repo (or
you) having to write or configure anything for it. As with the jemalloc
variant, TCMalloc never disables the kernel's own automatic NUMA balancing /
memory-tiering page migration between fast and slow nodes -- it only
influences *where new pages are first placed*; the kernel remains free to
migrate hot/cold pages between tiers over time exactly as it would for any
other allocator.

You can control (or double check) this at runtime via the `TCMALLOC_NUMA_AWARE`
environment variable, which `tcmalloc_numa_aware` reads on start-up:

| Value | Effect |
|---|---|
| *(unset)* | Enabled by default (this target links `want_numa_aware.cc`'s weak-symbol override, which flips the "enabled by default" bit -- see [`tcmalloc/tcmalloc/BUILD`](tcmalloc/tcmalloc/BUILD), `tcmalloc_numa_aware`'s `deps`). |
| `1` or `advisory-binding` | NUMA-aware, `mbind()` best-effort (a failure is logged, not fatal). |
| `no-binding` | NUMA-aware partitioning, but never calls `mbind()`. |
| `strict-binding` | NUMA-aware, `mbind()` failures are fatal. |
| `0` | Disable NUMA awareness (falls back to a single partition, like the plain `tcmalloc` target). |

```sh
TCMALLOC_NUMA_AWARE=1 ./stream.hw 10000000
```

### Why `--check_visibility=false`

Upstream TCMalloc scopes `//tcmalloc:tcmalloc_numa_aware`'s Bazel
`visibility` to its own `//tcmalloc/testing` package (that target is
normally only consumed by TCMalloc's internal variant test/benchmark
harness, see [`tcmalloc/tcmalloc/BUILD`](tcmalloc/tcmalloc/BUILD)). Since we
are a different Bazel module depending on it from the outside, Bazel would
otherwise reject the dependency with a visibility error.

We do **not** work around this by editing the `tcmalloc` submodule's `BUILD`
file (that would be modifying the allocator's own build configuration). We
instead build with Bazel's own, official escape hatch for exactly this
situation:
[`--check_visibility=false`](https://bazel.build/concepts/visibility)
("For prototyping, you can disable target visibility enforcement by setting
the flag `--check_visibility=false`."). This is a pure Bazel-level
access-control relaxation for our build invocation; it does not alter a
single byte of TCMalloc's source, build files, or behavior -- TCMalloc is
still compiled from its own, unmodified `BUILD` file, with the exact same
flags (`-DTCMALLOC_INTERNAL_NUMA_AWARE`) and dependencies it would use if
consumed from within its own `//tcmalloc/testing` package.
`makefiles/Makefile-hw` passes this flag automatically.

## Usage

```sh
<binary_name> <number_of_elements>
```

E.g.,

```sh
make -f makefiles/Makefile-hw
./stream.hw 10000000
```

The first `make` invocation will have Bazel fetch and build TCMalloc's
dependencies (currently just Abseil, plus small build-support repos such as
`rules_cc`/`platforms`/`bazel_skylib` -- see "Why Bazel" above), which
requires network access. Subsequent builds are served from Bazel's local
cache.

## Compilation

### Prerequisites

* [Bazel](https://bazel.build/) 7.0+ (Bzlmod support), or
  [Bazelisk](https://github.com/bazelbuild/bazelisk) (recommended: it reads
  `tcmalloc`'s pinned Bazel version automatically). Set `BAZEL=bazelisk` (or
  point `BAZEL` at whatever binary you use) if `bazel` isn't already on your
  `PATH`.
* A C++17 compiler (GCC or Clang). `.bazelrc` in this directory already adds
  `--cxxopt='-std=c++17'` for every C++ compilation action, matching
  [`tcmalloc/.bazelrc`](tcmalloc/.bazelrc).

### Using Native Hardware

```sh
make -f makefiles/Makefile-hw
# output: stream.hw
```

This is the variant meant to run on the real NUMA (fast/slow memory)
system.

### Native SVE build

```sh
make -f makefiles/Makefile-hw+sve
# output: stream.hw.sve
```

Identical to `Makefile-hw`, but passes `--copt=-march=armv8.1-a+sve` to
*every* C/C++ compilation in the build (including TCMalloc's own sources),
so this must be built (and run) natively on real Armv8.1+SVE hardware.

### Cross-compilation

`makefiles/Makefile-arm`, `Makefile-armsve`, `Makefile-rv64gc`, and
`Makefile-rvv` intentionally print an error and stop rather than attempting
a cross build. Unlike jemalloc's autotools build (which cross-compiles with
a simple `--host=<triple>` flag), cross-compiling *any* Bazel build requires
defining a full Bazel
[platform and C/C++ toolchain](https://bazel.build/extending/toolchains)
(sysroot, target constraints, linker flags, etc.) -- and TCMalloc's own
dependency (Abseil) would need the same. That is highly specific to your
cross sysroot and is out of scope for this repository. If you need a
NUMA-aware STREAM binary that cross-compiles easily, use
[`../stream-jemalloc`](../stream-jemalloc) instead, which cross-compiles
with a plain `--host=<triple>` passed to jemalloc's `configure`.

If you do need to cross-compile TCMalloc, the pieces you would need to add
are a `platform()` definition for the target, a matching
`cc_toolchain`/`cc_toolchain_config`, and a `--platforms=` flag on the
`bazel build` invocation in the corresponding Makefile; see the [Bazel
platforms guide](https://bazel.build/extending/platforms) and
[`tcmalloc/docs/platforms.md`](tcmalloc/docs/platforms.md).

### Compiling with gem5 ROI Annotations

`stream.cpp`'s `GEM5_ANNOTATION` code path shells out to the `m5` binary
directly (`system("m5 --addr=... exit;")`, etc. -- the same mechanism as
[`../stream-syscalls`](../stream-syscalls)), so, unlike the plain `stream`
variant, no `m5ops.h`/`libm5` linking is required. Just set `M5_BUILD_PATH`
(only used to trigger the `-DGEM5_ANNOTATION=1` define; the value itself
doesn't need to point anywhere in particular for this variant, but is kept
for a consistent interface with the other variants in this repository) and
make sure `m5` is on your `PATH` at run time:

```sh
make -f makefiles/Makefile-hw M5_BUILD_PATH=<gem5_dir>/util/m5/build/x86/
# output: stream.hw.m5
```

### Build-time knobs

| Variable | Default | Meaning |
|---|---|---|
| `BAZEL` | `bazel` | Path/name of the Bazel (or Bazelisk) binary to invoke. |
| `BAZEL_BUILD_FLAGS` | `-c opt --check_visibility=false` (+ `--copt=-march=armv8.1-a+sve` on `Makefile-hw+sve`) | Flags passed to `bazel build`. |

## Verifying NUMA behavior on real hardware

None of this can be meaningfully exercised on a machine without multiple
NUMA nodes, so it is intentionally not exercised as part of building this
repository. On the target system, useful things to check:

* `numactl --hardware` -- confirms the fast/slow node layout the kernel sees.
* `cat /sys/kernel/mm/numa/demotion_enabled` and
  `sysctl kernel.numa_balancing` -- whether the kernel's own tiering/page
  migration is enabled (see [`../stream-jemalloc/README.md`](../stream-jemalloc/README.md)
  for more detail; this applies identically here since it is a kernel, not
  allocator, feature).
* `numastat -p $(pidof stream.hw)` / `cat /proc/<pid>/numa_maps` while the
  benchmark is running -- shows which nodes the STREAM arrays' pages
  actually landed on.
* `TCMALLOC_NUMA_AWARE=strict-binding ./stream.hw 10000000` -- if TCMalloc's
  NUMA partitioning were somehow not active (e.g. wrong malloc linked), this
  would still just run normally (strict-binding only matters once NUMA
  awareness is actually active); the more direct check is `strace -f -e mbind
  ./stream.hw 1000` and confirming `mbind()` calls appear.
* TCMalloc's built-in stats dump also mentions its partitioning state; you
  can print it by calling `tcmalloc::MallocExtension::GetStats()` from a
  small scratch program linked the same way (see
  [`tcmalloc/tcmalloc/malloc_extension.h`](tcmalloc/tcmalloc/malloc_extension.h)
  and [`tcmalloc/docs/stats.md`](tcmalloc/docs/stats.md)); this repo's
  `stream.cpp` deliberately does not call TCMalloc-specific APIs itself, to
  keep it identical to the plain `../stream` variant.

## Troubleshooting

* **`ERROR: ... is not visible from target '//:stream_tcmalloc'`**: the
  build was invoked without `--check_visibility=false` -- use
  `make -f makefiles/Makefile-hw` (which already passes it), or add it to
  `BAZEL_BUILD_FLAGS` if invoking `bazel` directly. See "Why
  `--check_visibility=false`" above.
* **Bazel tries to download Abseil/protobuf/etc. from the network and
  fails**: Bazel/Bzlmod needs network access to the [Bazel Central
  Registry](https://registry.bazel.build/) and GitHub the first time you
  build (subsequent builds use Bazel's local cache under `~/.cache/bazel`).
* **`local_path_override` / "module not found" errors**: make sure the
  `tcmalloc` git submodule is checked out (`git submodule update --init
  --recursive` from the repository root).
* **Wrong C++ standard errors while compiling TCMalloc**: make sure you are
  building through the Makefiles (which use this directory's `.bazelrc`) or
  otherwise pass `--cxxopt='-std=c++17'` yourself.
