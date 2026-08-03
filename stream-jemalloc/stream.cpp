#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <sys/time.h>

#include <vector>
#include <iostream>
#include <cstdlib>
#include <chrono>

// jemalloc's public introspection API (mallctl) and (optionally) libnuma are
// used purely to *report* the NUMA topology and the active jemalloc tuning
// at start up. Neither header is used to alter jemalloc's behavior; the
// array allocations below are plain std::vector/new allocations that are
// transparently served by jemalloc because the binary is linked against
// libjemalloc (see makefiles/Makefile-hw and README.md).
#include <jemalloc/jemalloc.h>
#ifdef USE_LIBNUMA
#include <numa.h>
#endif

#ifdef GEM5_ANNOTATION
#include <gem5/m5ops.h>
#endif

// https://stackoverflow.com/questions/3437404/min-and-max-in-c
#define max(a,b) \
	({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define min(a,b) \
	({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

typedef double TElement;

static TElement expected_a, expected_b, expected_c;
static double t_copy = 0.0;
static double t_scale = 0.0;
static double t_add = 0.0;
static double t_triad = 0.0;
static int error_count_a = 0;
static double max_error_a = 0.0;
static double min_error_a = 0.0;
static int error_count_b = 0;
static double max_error_b = 0.0;
static double min_error_b = 0.0;
static int error_count_c = 0;
static double max_error_c = 0.0;
static double min_error_c = 0.0;

#define ARRAY_SIZE 10000000

// functions
void array_verify(const std::vector<TElement>&, TElement, int*, TElement*, TElement*);
void report(const size_t&, const size_t&);
size_t get_num_omp_threads();
void report_allocator_config();

// external functions
extern "C" void do_copy(TElement* __restrict__ dst, TElement* __restrict__ src, const size_t array_size);
extern "C" void do_scale(TElement* __restrict__ dst, TElement* __restrict__ src, const TElement scale_factor, const size_t array_size);
extern "C" void do_add(TElement* __restrict__ dst, TElement* __restrict__ src1, TElement* __restrict__ src2, const size_t array_size);
extern "C" void do_triad(TElement* __restrict__ dst, TElement* __restrict__ src1, TElement* __restrict__ src2, const TElement scale_factor, const size_t array_size);

// interfacing to the external functions
double copy(std::vector<TElement>& dst, std::vector<TElement>& src);
double scale(std::vector<TElement>& dst, std::vector<TElement>& src, const TElement& scale_factor);
double add(std::vector<TElement>& dst, std::vector<TElement>& src1, std::vector<TElement>& src2);
double triad(std::vector<TElement>& dst, std::vector<TElement>& src1, std::vector<TElement>& src2, const TElement& scale_factor);

// main
int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <array_size>" << std::endl;
        return 1;
    }

    size_t num_threads = get_num_omp_threads();
    report_allocator_config();

    size_t array_size = atoi(argv[1]);
    static std::vector<TElement> a(array_size, 1);
    static std::vector<TElement> b(array_size, 2);
    static std::vector<TElement> c(array_size, 0);
    static const TElement scale_factor = 3.0;

    // warm up
    copy(c, a);
    scale(b, c, scale_factor);
    add(c, a, b);
    triad(a, b, c, scale_factor);

    std::cout << "ready to start! size: " << array_size * sizeof(TElement) << std::endl;  
    // 1 iteration
#ifdef GEM5_ANNOTATION
    // checkpoint
    system("m5 --addr=0xFFFF0000 exit;");
    system("m5 --addr=0xFFFF0000 workbegin;");
#endif
    t_copy = copy(c, a);
#ifdef GEM5_ANNOTATION
    system("m5 --addr=0xFFFF0000 workend;");
    system("m5 --addr=0xFFFF0000 workbegin;");
#endif
    t_scale = scale(b, c, scale_factor);
#ifdef GEM5_ANNOTATION
    system("m5 --addr=0xFFFF0000 workend;");
    system("m5 --addr=0xFFFF0000 workbegin;");
#endif
    t_add = add(c, a, b);
#ifdef GEM5_ANNOTATION
    system("m5 --addr=0xFFFF0000 workend;");
    system("m5 --addr=0xFFFF0000 workbegin;");
#endif
    t_triad = triad(a, b, c, scale_factor);
#ifdef GEM5_ANNOTATION
    system("m5 --addr=0xFFFF0000 workend;");
    // end simulation
    system("m5 --addr=0xFFFF0000 exit;");
#endif

    expected_a = 1.0;
    expected_b = 2.0;
    expected_c = 0.0;
    for (size_t iter = 0; iter < 2; iter++)
    {
        expected_c = expected_a;
        expected_b = expected_c * scale_factor;
        expected_c = expected_a + expected_b;
        expected_a = expected_b + expected_c * scale_factor;
    }

    array_verify(a, expected_a, &error_count_a, &min_error_a, &max_error_a);
    array_verify(b, expected_b, &error_count_b, &min_error_b, &max_error_b);
    array_verify(c, expected_c, &error_count_c, &min_error_c, &max_error_c);

    report(array_size, num_threads);
}

void
array_verify(const std::vector<TElement>& arr, TElement expected_value,
             int* error_count, TElement* min_error, TElement* max_error)

{
    *error_count = 0;
    *min_error = DBL_MAX;
    *max_error = 0.0;

    for (auto const & element: arr)
    {
        if (element != expected_value)
        {
            *error_count += 1;
            TElement diff = abs(element - expected_value);
            *min_error = min(diff, *min_error);
            *max_error = max(diff, *max_error);
        }
    }
}

// Prints the NUMA topology visible to the kernel, and the jemalloc runtime
// options that are currently in effect (queried via jemalloc's public
// mallctl(3) introspection interface). This is purely informational: it lets
// a user confirm, on real NUMA hardware, that jemalloc picked up the system's
// NUMA configuration (as documented in README.md) without having to modify
// or instrument jemalloc itself.
void report_allocator_config()
{
#ifdef USE_LIBNUMA
    if (numa_available() == -1)
    {
        std::cout << "NUMA: not available (libnuma reports no NUMA support on this kernel/system)" << std::endl;
    }
    else
    {
        std::cout << "NUMA: " << (numa_max_node() + 1) << " node(s) visible to this process" << std::endl;
        struct bitmask* mems = numa_get_mems_allowed();
        if (mems != nullptr)
        {
            std::cout << "NUMA: allowed memory nodes: ";
            for (int node = 0; node <= numa_max_node(); node++)
            {
                if (numa_bitmask_isbitset(mems, node))
                    std::cout << node << " ";
            }
            std::cout << std::endl;
            numa_bitmask_free(mems);
        }
    }
#else
    std::cout << "NUMA: libnuma diagnostics not compiled in (build with WITH_LIBNUMA=1)" << std::endl;
#endif

    auto print_jemalloc_str = [](const char* name) {
        const char* value = nullptr;
        size_t len = sizeof(value);
        if (mallctl(name, &value, &len, nullptr, 0) == 0 && value != nullptr)
            std::cout << "jemalloc " << name << ": " << value << std::endl;
    };
    auto print_jemalloc_bool = [](const char* name) {
        bool value = false;
        size_t len = sizeof(value);
        if (mallctl(name, &value, &len, nullptr, 0) == 0)
            std::cout << "jemalloc " << name << ": " << (value ? "true" : "false") << std::endl;
    };
    auto print_jemalloc_unsigned = [](const char* name) {
        unsigned value = 0;
        size_t len = sizeof(value);
        if (mallctl(name, &value, &len, nullptr, 0) == 0)
            std::cout << "jemalloc " << name << ": " << value << std::endl;
    };

    print_jemalloc_str("opt.percpu_arena");
    print_jemalloc_str("opt.metadata_thp");
    print_jemalloc_bool("opt.background_thread");
    print_jemalloc_unsigned("opt.narenas");
    print_jemalloc_unsigned("arenas.narenas");
}

size_t get_num_omp_threads()
{
    size_t num_threads = 0;
#ifdef _OPENMP
#pragma omp parallel
#pragma omp atomic
    num_threads++;
#endif
    return num_threads;
}

double copy(std::vector<TElement>& dst, std::vector<TElement>& src)
{
    const auto t_start = std::chrono::steady_clock::now();
    do_copy(dst.data(), src.data(), dst.size());
    const auto t_end = std::chrono::steady_clock::now();
    std::chrono::duration<double> delta_t = std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start);
    return delta_t.count();
}
double scale(std::vector<TElement>& dst, std::vector<TElement>& src, const TElement& scale_factor)
{
    const auto t_start = std::chrono::steady_clock::now();
    do_scale(dst.data(), src.data(), scale_factor, dst.size());
    const auto t_end = std::chrono::steady_clock::now();
    std::chrono::duration<double> delta_t = std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start);
    return delta_t.count();
}
double add(std::vector<TElement>& dst, std::vector<TElement>& src1, std::vector<TElement>& src2)
{
    auto t_start = std::chrono::steady_clock::now();
    do_add(dst.data(), src1.data(), src2.data(), dst.size());
    const auto t_end = std::chrono::steady_clock::now();
    std::chrono::duration<double> delta_t = std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start);
    return delta_t.count();

}
double triad(std::vector<TElement>& dst, std::vector<TElement>& src1, std::vector<TElement>& src2, const TElement& scale_factor)
{
    const auto t_start = std::chrono::steady_clock::now();
    do_triad(dst.data(), src1.data(), src2.data(), scale_factor, dst.size());
    const auto t_end = std::chrono::steady_clock::now();
    std::chrono::duration<double> delta_t = std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start);
    return delta_t.count();
}


void report(const size_t& array_size, const size_t& num_threads)
{
    double data_size_bytes = array_size * sizeof(TElement);
    double data_size_GiB = data_size_bytes / 1024.0 / 1024.0 / 1024.0;
    double copy_bandwidth = 2.0 * data_size_GiB / t_copy;
    double scale_bandwidth = 2.0 * data_size_GiB / t_scale;
    double add_bandwidth = 3.0 * data_size_GiB / t_add;
    double triad_bandwidth = 3.0 * data_size_GiB / t_triad;
    printf("Each array size: %f GiB\n", data_size_GiB);
    printf("Number of threads: %ld\n", num_threads);
    printf("Copy:   ");
    printf("Bandwidth: %f GiB/s", copy_bandwidth);
    printf("    Time: %f s\n", t_copy);
    printf("Scale:  ");
    printf("Bandwidth: %f GiB/s", scale_bandwidth);
    printf("    Time: %f s\n", t_scale);
    printf("Add:    ");
    printf("Bandwidth: %f GiB/s", add_bandwidth);
    printf("    Time: %f s\n", t_add);
    printf("Triad:  ");
    printf("    Bandwidth: %f GiB/s", triad_bandwidth);
    printf("Time: %f s\n", t_triad);
    printf("array_a error_count: %d, max_error: %f\n", error_count_a, max_error_a);
    printf("array_b error_count: %d, max_error: %f\n", error_count_b, max_error_b);
    printf("array_c error_count: %d, max_error: %f\n", error_count_c, max_error_c);
}

