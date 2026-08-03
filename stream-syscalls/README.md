## STREAM C++ Benchmark

This is a variant of the STREAM benchmark that decoupled the STREAM kernels
from the main program. There STREAM kernels, i.e. Copy, Scale, Add, and Triad,
are written in the C language and are compiled with OpenMP. The main program
is written in C++.

By default, the benchmark will spawn as many threads as many hardware threads.
Each of the kernels is executed twice, and only the elapsed time of the second
execution is recorded for each kernel.

## Usage

```sh
<binary_name> <number_of_elements>
```

E.g.,

```sh
make -f makefiles/Makefile-hw
./stream.hw 10000000
```

## Compilation

### Using Native Hardware

```sh
make -f makefiles/Makefile-hw
# output: stream.hw
```

### Compilations for Specific Architectures

The following Makefile files are mostly identical to the Makefile-hw file,
except for the default `CC` and `CXX` environmental variables are set to
cross compiler, and the `CFLAGS_KERNEL` and `CXX_FLAGS` variables contain
specific `-march` options.

```sh
make -f makefiles/Makefile-rv64gc
# output: stream.rv64gc
make -f makefiles/Makefile-rvv
# output: stream.rvv
make -f makefiles/Makefile-arm
# output: stream.arm
make -f makefiles/Makefile-armsve
# output: stream.armsve
```

### Compiling with gem5 ROI Annotations

This requires setting those two environmental variables: `M5_BUILD_PATH` and
`M5OPS_HEADER_PATH`, where `M5_BUILD_PATH` is the path to the m5 build for the
corresponding ISA (see the example below), and `M5OPS_HEADER_PATH` is the
path to the `m5ops.h` header file (also see the example below).

The following is an example of compiling an arm64 binary with gem5 ROI
annotations on an x86\_64 machine,

```sh
# prerequisite steps
cd <gem5_dir>/util/m5
scons arm64.CROSS_COMPILE=aarch64-linux-gnu- build/arm64/out/m5
# stream compilation steps
cd <stream_dir>
make -f makefiles/Makefile-arm M5OPS_HEADER_PATH=<gem5_dir>/include/ M5_BUILD_PATH=<gem5_dir>/util/m5/build/arm64/
```

