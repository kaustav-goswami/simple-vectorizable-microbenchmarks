#include <unistd.h>

typedef double TElement;

void do_copy(TElement* restrict dst, TElement* restrict src, const size_t array_size)
{
    #pragma omp parallel for simd
    for (size_t k = 0; k < array_size; k++)
        dst[k] = src[k];
}

void do_scale(TElement* restrict dst, TElement* restrict src, const TElement scale_factor, const size_t array_size)
{
    #pragma omp parallel for simd
    for (size_t k = 0; k < array_size; k++)
        dst[k] = scale_factor * src[k];
}

void do_add(TElement* restrict dst, TElement* restrict src1, TElement* restrict src2, const size_t array_size)
{
    #pragma omp parallel for simd
    for (size_t k = 0; k < array_size; k++)
        dst[k] = src1[k] + src2[k];
}

void do_triad(TElement* restrict dst, TElement* restrict src1, TElement* restrict src2, const TElement scale_factor, const size_t array_size)
{
    #pragma omp parallel for simd
    for (size_t k = 0; k < array_size; k++)
        dst[k] = src1[k] + scale_factor * src2[k];
}
