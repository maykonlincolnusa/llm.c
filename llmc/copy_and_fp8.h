/*
Helpers for FP8 including copy and transpose with format conversion, and absmax
See /dev/cuda/advanced_copy_transpose.cu for more information and options
*/
#ifndef FP8_HELPERS_CUH
#define FP8_HELPERS_CUH

#include <assert.h>
#include <typeinfo>
#include "cuda_common.h"
#include "cuda_utils.cuh"

// todo - tune these for performance (but should be close to optimal already)
#define ABSMAX_ITERATIONS_PER_THREAD 4
#define TRANSPOSE_TILE_SIZE 64UL

// ----------------------------------------------------------------------------
// elementwise functions which can be applied as part of the copy/transpose
// for elementwise kernels that require metadata (e.g. layernorm forward with known mean/std),
// we could maybe store it in constant buffers rather than in yet-another-function-parameter...
using elementwise_func_t = float (*) (float);
__device__ float nothing_elementwise(float x) {
    return x;
}
__device__ float gelu_forward_elementwise(float x) {
    float cube = 0.044715f * x * x * x;

    float tanh_out;
    float tanh_arg = sqrtf(2.0f / M_PI) * (x + cube);
    asm ("tanh.approx.f32 %0,%1;" : "=f"(tanh_out) : "f"(tanh_arg));

    // the following uses FMUL+FMA instead of FMUL+FADD+FMUL for "0.5f * x * (1.0f + tanh_out)"
    float half_x = 0.5f * x;
    return half_x * tanh_out + half_x;
}

// ----------------------------------------------------------------------------
// CUDA kernels

// Same as copy_simple_kernel but with optional absmax and elementwise function options
// absmax is calculated before scaling but after the elementwise function
template <int block_size=256, bool disable_scaling=false, bool reversed_order=false,
          elementwise_func_t elementwise_func=nothing_elementwise,
          typename T1=float, typename T2=float>
__global__ void copy_advanced_kernel(TensorGPU<T1> in, TensorGPU<T2> out) {
    constexpr size_t vec_size = 16 / ((sizeof(T1) < sizeof(T2)) ? sizeof(T2) : sizeof(T1));
    size_t adjusted_blockidx = reversed_order ? (gridDim.x - blockIdx.x - 1) : blockIdx.x;
    size_t idx = (adjusted_blockidx * blockDim.x + threadIdx.x) * vec_size;
    if (idx >= in.num_elements) { return; }

    auto inp128 = load_tensor128(in, idx, true, disable_scaling);
    auto out128 = new_tensor128(out);
    for (int k = 0; k < vec_size; k++) {
        float out_fp32 = elementwise_func(inp128.get(k));
        out128.set(k, out_fp32);
    }
    out128.store_same_length(idx);
    out128.update_absmax(threadIdx.x, block_size, true);
}

/*
// transpose + copy + format conversion (+ elementwise + absmax) kernel
template<size_t BLOCK_ROWS=8UL, size_t TILE_DIM=TRANSPOSE_TILE_SIZE, bool reciprocal_scale=true, bool enable_copy=false, bool scaling=true,
         uint absmax_factor=0, elementwise_func_t elementwise_func=nothing_elementwise, typename T1, typename T2>
__global__ void transpose_kernel(T1* __restrict__ transposed, T1* __restrict__ copy, const T2* __restrict__ input,
                                 const float* __restrict__ descale_pointer=(float*)NULL, const float* __restrict__ scale_pointer=(float*)NULL,
                                 unsigned int* absmax_output=(unsigned int*)NULL, const void** meta=NULL)
{
    constexpr size_t TILE_DIM_PADDED = TILE_DIM + 4/sizeof(T1);
    __shared__ T1 tile[TILE_DIM][TILE_DIM_PADDED];
    int width  = gridDim.x * TILE_DIM;
    int height = gridDim.y * TILE_DIM;

    constexpr size_t T1_elements = 16 / sizeof(T1);
    constexpr size_t T2_elements = 16 / sizeof(T2);
    constexpr size_t copy_vectors = (sizeof(T1) >= sizeof(T2)) ? (sizeof(T1) / sizeof(T2)) : 1;

    float descale_factor = (scaling && descale_pointer) ? *descale_pointer : 1.0f; // never reciprocal
    float scale_factor = (scaling && scale_pointer) ? *scale_pointer : 1.0f;
    scale_factor = (reciprocal_scale && scale_factor != 0.0f) ? (1.0f / scale_factor) : scale_factor;
    int x = blockIdx.x * TILE_DIM + (threadIdx.x * T2_elements);
    int y = blockIdx.y * TILE_DIM + threadIdx.y;
    uint absmax_uint = 0;

    #pragma unroll
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        Packed128<T2> in128 = load128cs<T2>(input + x + (y+j)*width);
        Packed128<T1> copy128[copy_vectors];
        for (int k = 0; k < in128.size; k++) {
            T2 in = in128[k];
            float out_float = elementwise_func((float)in * descale_factor);
            update_local_absmax(absmax_uint, out_float, absmax_factor); // optional absmax

            T1 out = (T1)(out_float * scale_factor);
            copy128[k/T1_elements][k%T1_elements] = out; // optimised away by compiler if unused
        }

        for (int o = 0; o < copy_vectors; o++) {
            if constexpr (enable_copy) {
                store_same_length<T2,T1>(copy + x + (y+j)*width + o*T1_elements, copy128[o]);
            }

            size_t tile_offset = (threadIdx.x * T2_elements) + (threadIdx.y+j)*TILE_DIM_PADDED + o*T1_elements;
            int* one_bank = reinterpret_cast<int*>(&tile[0][0] + tile_offset);
            for (int k = 0; k < 4; k++) {
                one_bank[k] = *(int*)(&copy128[o][k*4/sizeof(T1)]);
            }
            //store_same_length<T2,T1>(&tile[0][0] + tile_offset, copy128[o]);
        }
    }

    if constexpr (absmax_factor != 0) {
        update_global_absmax<true>(absmax_output, absmax_uint);
    } else {
        __syncthreads();
    }

    // reduce the number of threads for the write if T1_elements > T2_elements
    // we want to keep all 32 threads in a warp active, so we try to eliminate in y dimension first
    // so we create fake/adjusted tid.x/tid.y where "extra" threadIdx.x adds to the effective tid.y
    constexpr size_t block_size_x = (TILE_DIM * sizeof(T2)) / 16;
    constexpr size_t block_size_y = BLOCK_ROWS;

    constexpr size_t desired_ratio = (sizeof(T2) >= sizeof(T1)) ? (sizeof(T2) / sizeof(T1)) : 1;
    constexpr size_t ratio = (desired_ratio <= block_size_y) ? desired_ratio : block_size_y;
    constexpr size_t block_size_x_div_r = block_size_x / ratio;
    constexpr size_t block_size_y_div_r = block_size_y / ratio;

    int adjusted_tid_x = threadIdx.x % block_size_x_div_r;
    int adjusted_tid_y = (threadIdx.y * ratio) + (threadIdx.x / block_size_x_div_r);
    if (threadIdx.y >= block_size_y_div_r) { return; }

    // if we cannot reduce block_size.y enough, also reduce x (hurting perf with partial warps)
    if (ratio != desired_ratio && adjusted_tid_x >= TILE_DIM / T1_elements) { return; }

    // x/y for final write to global memory
    x = blockIdx.y * TILE_DIM + adjusted_tid_x * T1_elements;
    y = blockIdx.x * TILE_DIM + adjusted_tid_y;

    constexpr int in_parallel = 4/sizeof(T1);

    #pragma unroll
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS * in_parallel) {
        if ((j+adjusted_tid_y) * in_parallel >= TILE_DIM) { return; }

        // we need more instructions for the write than the read if T2_elements > T1_elements
        #pragma unroll
        for (int o = 0; o < copy_vectors; o++) {
            Packed128<T1> out128[in_parallel];
            #pragma unroll
            for (int k = 0; k < Packed128<T1>::size; k++) {
                int in32 = *(int*)(&tile[k + (adjusted_tid_x + o * blockDim.x) * Packed128<T1>::size][(adjusted_tid_y + j) * in_parallel]);
                for (int p = 0; p < in_parallel; p++) {
                    out128[p][k] = ((T1*)&in32)[p];
                }
            }
            for (int p = 0; p < in_parallel; p++) {
                store128<T1>(transposed + x + (o * blockDim.x * Packed128<T1>::size) + (y+p + j * in_parallel)*height, out128[p]);
            }
        }
    }
}
*/

/*
template<size_t BLOCK_ROWS=8UL, size_t TILE_DIM=TRANSPOSE_TILE_SIZE, bool enable_copy=false,
         elementwise_func_t elementwise_func=nothing_elementwise, typename T1, typename T2>
__global__ void transpose_kernel_tensor(TensorGPU<T1> transposed, TensorGPU<T1> copy, TensorGPU<T2> input, int height) {
    __shared__ T1 tile[TILE_DIM][TILE_DIM];
    int width  = gridDim.x * TILE_DIM;
    height = gridDim.y * TILE_DIM;

    constexpr bool disable_scaling = (sizeof(T1) == sizeof(T2)); // TODO - THIS IS WRONG - need to check types are identical, not just same size!
    constexpr size_t T1_elements = 16 / sizeof(T1);
    constexpr size_t T2_elements = 16 / sizeof(T2);
    constexpr size_t copy_vectors = (sizeof(T1) >= sizeof(T2)) ? (sizeof(T1) / sizeof(T2)) : 1;

    int x = blockIdx.x * TILE_DIM + (threadIdx.x * T2_elements);
    int y = blockIdx.y * TILE_DIM + threadIdx.y;

    tensor128<T1> copy128 = new_tensor128<enable_copy>(copy, disable_scaling);

    #pragma unroll
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        auto in128 = load_tensor128(input, x + (y+j)*width, true, disable_scaling);
        Packed128<T2> in128 = load128cs<T2>(input + x + (y+j)*width);
        Packed128<T1> copy128[copy_vectors];
        for (int k = 0; k < in128.size; k++) {
            float out_float = elementwise_func(in128.get(k));
            copy128.set(k % T1_elements, out_float * scale_factor); // optimised away by compiler if unused

            if (k+1 == out128.size) {
                // ...

            }
        }

        for (int o = 0; o < copy_vectors; o++) {
            if constexpr (enable_copy) {
                store_same_length<T2,T1>(copy + x + (y+j)*width + o*T1_elements, copy128[o]);
            }
            size_t tile_offset = (threadIdx.x * T2_elements) + (threadIdx.y+j)*TILE_DIM + o*T1_elements;
            store_same_length<T2,T1>(&tile[0][0] + tile_offset, copy128[o]);
        }
    }


}
*/





// transpose + copy + format conversion (+ elementwise + absmax) kernel
template<size_t BLOCK_ROWS=8UL, size_t TILE_DIM=TRANSPOSE_TILE_SIZE, bool reciprocal_scale=true, bool enable_copy=false, bool scaling=true,
         uint absmax_factor=0, elementwise_func_t elementwise_func=nothing_elementwise, typename T1, typename T2>
__global__ void transpose_kernel(T1* __restrict__ transposed, T1* __restrict__ copy, const T2* __restrict__ input, int height,
                                 const float* __restrict__ descale_pointer=(float*)NULL, const float* __restrict__ scale_pointer=(float*)NULL,
                                 unsigned int* absmax_output=(unsigned int*)NULL, const void** meta=NULL)
{
    /*
    __shared__ T1 tile[TILE_DIM][TILE_DIM];
    int width  = gridDim.x * TILE_DIM;
    height = gridDim.y * TILE_DIM;

    constexpr size_t T1_elements = 16 / sizeof(T1);
    constexpr size_t T2_elements = 16 / sizeof(T2);
    constexpr size_t copy_vectors = (sizeof(T1) >= sizeof(T2)) ? (sizeof(T1) / sizeof(T2)) : 1;

    float descale_factor = (scaling && descale_pointer) ? *descale_pointer : 1.0f; // never reciprocal
    float scale_factor = (scaling && scale_pointer) ? *scale_pointer : 1.0f;
    scale_factor = (reciprocal_scale && scale_factor != 0.0f) ? (1.0f / scale_factor) : scale_factor;
    int x = blockIdx.x * TILE_DIM + (threadIdx.x * T2_elements);
    int y = blockIdx.y * TILE_DIM + threadIdx.y;
    uint absmax_uint = 0;

    #pragma unroll
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        Packed128<T2> in128 = load128cs<T2>(input + x + (y+j)*width);
        Packed128<T1> copy128[copy_vectors];
        for (int k = 0; k < in128.size; k++) {
            T2 in = in128[k];
            float out_float = elementwise_func((float)in * descale_factor);
            update_local_absmax(absmax_uint, out_float, absmax_factor); // optional absmax

            T1 out = (T1)(out_float * scale_factor);
            copy128[k/T1_elements][k%T1_elements] = out; // optimised away by compiler if unused
        }

        for (int o = 0; o < copy_vectors; o++) {
            if constexpr (enable_copy) {
                store_same_length<T2,T1>(copy + x + (y+j)*width + o*T1_elements, copy128[o]);
            }
            size_t tile_offset = (threadIdx.x * T2_elements) + (threadIdx.y+j)*TILE_DIM + o*T1_elements;
            store_same_length<T2,T1>(&tile[0][0] + tile_offset, copy128[o]);
        }
    }

    if constexpr (absmax_factor != 0) {
        update_global_absmax<true>(absmax_output, absmax_uint);
    } else {
        __syncthreads();
    }

    // reduce the number of threads for the write if T1_elements > T2_elements
    // we want to keep all 32 threads in a warp active, so we try to eliminate in y dimension first
    // so we create fake/adjusted tid.x/tid.y where "extra" threadIdx.x adds to the effective tid.y
    constexpr size_t block_size_x = (TILE_DIM * sizeof(T2)) / 16;
    constexpr size_t block_size_y = BLOCK_ROWS;

    constexpr size_t desired_ratio = (sizeof(T2) >= sizeof(T1)) ? (sizeof(T2) / sizeof(T1)) : 1;
    constexpr size_t ratio = (desired_ratio <= block_size_y) ? desired_ratio : block_size_y;
    constexpr size_t block_size_x_div_r = block_size_x / ratio;
    constexpr size_t block_size_y_div_r = block_size_y / ratio;

    int adjusted_tid_x = threadIdx.x % block_size_x_div_r;
    int adjusted_tid_y = (threadIdx.y * ratio) + (threadIdx.x / block_size_x_div_r);
    if (threadIdx.y >= block_size_y_div_r) { return; }

    // if we cannot reduce block_size.y enough, also reduce x (hurting perf with partial warps)
    if (ratio != desired_ratio && adjusted_tid_x >= TILE_DIM / T1_elements) { return; }

    // x/y for final write to global memory
    x = blockIdx.y * TILE_DIM + adjusted_tid_x * T1_elements;
    y = blockIdx.x * TILE_DIM + adjusted_tid_y;

    #pragma unroll
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        // we need more instructions for the write than the read if T2_elements > T1_elements
        #pragma unroll
        for (int o = 0; o < copy_vectors; o++) {
            Packed128<T1> out128;
            #pragma unroll
            for (int k = 0; k < out128.size; k++) {
                // these are tiny 8-bit loads with loads of bank conflicts for FP8
                // extremely hard to avoid and not a bottleneck when everything else is well optimised
                out128[k] = tile[k + (adjusted_tid_x + o * blockDim.x) * out128.size][adjusted_tid_y + j];
            }
            store128<T1>(transposed + x + (o * blockDim.x * out128.size) + (y+j)*height, out128);
        }
    }
    */
}


/*
// best I could come up with (without using TMA) - no bank conflicts, but 64B reads/writes not ideal
// Z_DIM=2 improves perf by ~2% partly by improving L2 hit rates for the writes as far as I can tell
template<size_t BLOCK_ROWS=8UL, size_t TILE_DIM=TRANSPOSE_TILE_SIZE, bool reciprocal_scale=true, bool enable_copy=false, bool scaling=true,
         uint absmax_factor=0, elementwise_func_t elementwise_func=nothing_elementwise, int Z_DIM=1, typename T1, typename T2>
__global__ void transpose_kernel(T1* __restrict__ transposed, T1* __restrict__ copy, const T2* __restrict__ input, int height,
                                 const float* __restrict__ descale_pointer=(float*)NULL, const float* __restrict__ scale_pointer=(float*)NULL,
                                 unsigned int* absmax_output=(unsigned int*)NULL, const void** meta=NULL)
{
    constexpr int in_parallel = 4/sizeof(T1);

    constexpr size_t TILE_DIM_PADDED = (TILE_DIM * 33) / 32;
    __shared__ T1 tile[Z_DIM][TILE_DIM][TILE_DIM_PADDED];
    int w  = gridDim.x * TILE_DIM;

    constexpr size_t T1_elements = 16 / sizeof(T1);
    constexpr size_t T2_elements = 16 / sizeof(T2);
    constexpr size_t copy_vectors = (sizeof(T1) >= sizeof(T2)) ? (sizeof(T1) / sizeof(T2)) : 1;

    float descale_factor = (scaling && descale_pointer) ? *descale_pointer : 1.0f; // never reciprocal
    float scale_factor = (scaling && scale_pointer) ? *scale_pointer : 1.0f;
    scale_factor = (reciprocal_scale && scale_factor != 0.0f) ? (1.0f / scale_factor) : scale_factor;

    int x = blockIdx.x * TILE_DIM + (threadIdx.x * T2_elements);
    int y = blockIdx.y * TILE_DIM * Z_DIM + threadIdx.z * TILE_DIM + threadIdx.y;

    uint absmax_uint = 0;
    if (y < height) {
        #pragma unroll
        for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
            Packed128<T1> copy128[copy_vectors];

            int4 payload;
            const int4* address = reinterpret_cast<const int4*>(input + x + (y+j)*w);
            asm volatile("ld.global.L2::128B.v4.s32 {%0, %1, %2, %3}, [%4];"
                        : "=r"(payload.x), "=r"(payload.y), "=r"(payload.z), "=r"(payload.w)
                        : "l"(address));
            Packed128<T2> in128(payload);

            #pragma unroll
            for (int k = 0; k < in128.size; k++) {
                T2 in = in128[k];
                float out_float = elementwise_func((float)in * descale_factor);

                T1 out = (T1)(out_float * scale_factor);
                copy128[k/T1_elements][k%T1_elements] = out; // optimised away by compiler if unused
                update_local_absmax(absmax_uint, out_float, absmax_factor); // optional absmax
            }

            #pragma unroll
            for (int o = 0; o < copy_vectors; o++) {
                if constexpr (enable_copy) {
                    store_same_length<T2,T1>(copy + x + (y+j)*w + o*T1_elements, copy128[o]);
                }

                size_t offset_x = (threadIdx.x * T2_elements) + (o * T1_elements);
                size_t offset_y = (threadIdx.y + j) * TILE_DIM;
                offset_y += (offset_y / (128/sizeof(T1))) * in_parallel;

                int* one_bank = reinterpret_cast<int*>(&tile[threadIdx.z][0][0] + offset_x + offset_y);
                #pragma unroll
                for (int k = 0; k < 4; k++) {
                    one_bank[k] = *(int*)(&copy128[o][k*4/sizeof(T1)]);
                }
            }
        }
    }

    if constexpr (absmax_factor != 0) {
        update_global_absmax<true, true>(absmax_output, absmax_uint);
    } else {
        __syncthreads();
    }

    // reduce the number of threads for the write if T1_elements > T2_elements
    // we want to keep all 32 threads in a warp active, so we try to eliminate in y dimension first
    // so we create fake/adjusted tid.x/tid.y where "extra" threadIdx.x adds to the effective tid.y
    constexpr size_t block_size_x = (TILE_DIM * sizeof(T2)) / 16;
    constexpr size_t block_size_y = BLOCK_ROWS;
    constexpr size_t desired_ratio = (sizeof(T2) >= sizeof(T1)) ? (sizeof(T2) / sizeof(T1)) : 1;
    constexpr size_t ratio = (desired_ratio <= block_size_y) ? desired_ratio : block_size_y;
    constexpr size_t block_size_x_div_r = block_size_x / ratio;
    constexpr size_t block_size_y_div_r = block_size_y / ratio;

    int adjusted_tid_x = threadIdx.x % block_size_x_div_r;
    int adjusted_tid_y = (threadIdx.y * ratio) + (threadIdx.x / block_size_x_div_r);
    if (threadIdx.y >= block_size_y_div_r) { return; }

    // if we cannot reduce block_size.y enough, also reduce x (hurting perf with partial warps)
    if (ratio != desired_ratio && adjusted_tid_x >= TILE_DIM / T1_elements) { return; }

    // x/y for final write to global memory
    x = blockIdx.y * TILE_DIM * Z_DIM + threadIdx.z * TILE_DIM + adjusted_tid_x * T1_elements;
    y = blockIdx.x * TILE_DIM + (adjusted_tid_y*in_parallel);

    if (x >= height) { return; }

    #pragma unroll
    for (int j = 0; j < TILE_DIM / in_parallel; j += BLOCK_ROWS) {
        if ((j+adjusted_tid_y) * in_parallel * ratio >= TILE_DIM) { return; }

        // we need more instructions for the write than the read if T2_elements > T1_elements
        #pragma unroll
        for (int o = 0; o < copy_vectors; o++) {
            Packed128<T1> out128[in_parallel];
            #pragma unroll
            for (int k = 0; k < Packed128<T1>::size; k++) {
                int offset_x = (adjusted_tid_y + j) * in_parallel;
                int offset_y = ((adjusted_tid_x + o * blockDim.x) * Packed128<T1>::size + k) * TILE_DIM;
                offset_y += (offset_y / (128/sizeof(T1))) * in_parallel;

                int in32 = *(int*)(&tile[threadIdx.z][0][0] + offset_x + offset_y);
                for (int p = 0; p < in_parallel; p++) {
                    out128[p][k] = ((T1*)&in32)[p];
                }
            }
            #pragma unroll
            for (int p = 0; p < in_parallel; p++) {
                store128<T1>(transposed + x + (o * blockDim.x * Packed128<T1>::size) + (y+p + j * in_parallel) * height, out128[p]);
            }
        }
    }
}
*/

// only calculate absmax of the input tensor (non-fused)
template <bool disable_scaling=true, typename T>
__global__ void update_absmax_kernel(TensorGPU<T> inp) {
    size_t idx = ((blockIdx.x * blockDim.x * ABSMAX_ITERATIONS_PER_THREAD) + threadIdx.x) * inp.num_per_128();
    auto max128 = new_tensor128(inp, disable_scaling);
    if (idx < inp.num_elements) {
        #pragma unroll
        for (int i = 0; i < ABSMAX_ITERATIONS_PER_THREAD; i++) {
            auto inp128 = load_tensor128(inp, idx, disable_scaling);
            for(int k = 0; k < inp.num_per_128(); ++k) {
                float value = inp128.get(k);
                max128.add_value_stats(value);
            }
            idx += blockDim.x * inp.num_per_128();
        }
    }
    max128.update_absmax(threadIdx.x, blockDim.x, true, true);
}

// ----------------------------------------------------------------------------
// kernel launchers
/*
template <bool reciprocal=true, typename T1, typename T2>
void copy_simple(T1 *copy, const T2 *input, size_t N, float* scale_pointer=NULL, const size_t block_size=512) {
    size_t fewest_elements = min(Packed128<T1>::size, Packed128<T2>::size);
    const dim3 grid_size(CEIL_DIV(N, block_size * fewest_elements));

    if (scale_pointer) {
        copy_simple_kernel<reciprocal, true><<<grid_size, dim3(block_size)>>>(copy, input, N, scale_pointer);
    } else {
        copy_simple_kernel<reciprocal, false><<<grid_size, dim3(block_size)>>>(copy, input, N);
    }
    cudaCheck(cudaGetLastError());
}
*/

template <bool reversed_order=false, elementwise_func_t elementwise_func=nothing_elementwise, bool reciprocal=true, typename T1, typename T2>
void copy_advanced(T1 *copy, const T2 *input, size_t N, float* descale_pointer=NULL, float* scale_pointer=NULL, void* absmax_output=NULL, /*bool memset_absmax=true,*/ cudaStream_t stream=0, const size_t block_size=512) {
    size_t fewest_elements = min(Packed128<T1>::size, Packed128<T2>::size);
    const dim3 grid_size(CEIL_DIV(N, block_size * fewest_elements));
    assert((N % fewest_elements) == 0);

    constexpr uint absmax_factor = 1;
    unsigned int* absmax_uint = (unsigned int*)absmax_output;

    if (absmax_output) {
        /*if (memset_absmax) {
            cudaMemset(absmax_output, 0, sizeof(unsigned int));
        }*/
        if (scale_pointer || descale_pointer) {
            copy_advanced_kernel<reciprocal, true, reversed_order, elementwise_func, absmax_factor><<<grid_size, dim3(block_size), 0, stream>>>(copy, input, N, descale_pointer, scale_pointer, absmax_uint);
        } else {
            copy_advanced_kernel<reciprocal, false, reversed_order, elementwise_func, absmax_factor><<<grid_size, dim3(block_size), 0, stream>>>(copy, input, N, NULL, NULL, absmax_uint);
        }
    } else {
        if (scale_pointer || descale_pointer) {
            copy_advanced_kernel<reciprocal, true, reversed_order, elementwise_func><<<grid_size, dim3(block_size), 0, stream>>>(copy, input, N, descale_pointer, scale_pointer);
        } else {
            copy_advanced_kernel<reciprocal, false, reversed_order, elementwise_func><<<grid_size, dim3(block_size), 0, stream>>>(copy, input, N);
        }
    }
    cudaCheck(cudaGetLastError());
}

// only 2 important template parameters: write_absmax and elementwise_func
// (use copy_and_transpose() rather than enable_copy=true for clarity)
// slight inefficiency in that we don't optimise away scaling for kernels that don't need it (kernel checks for NULL)
template <bool write_absmax=false, elementwise_func_t elementwise_func=nothing_elementwise, bool reciprocal=true,
          bool enable_copy=false, typename T1, typename T2> // advanced template options, usually don't need to be changed
void transpose(T1 *transposed, const T2 *input, size_t w, size_t h, float* descale_pointer=NULL, float* scale_pointer=NULL, void* absmax_output=NULL,
               /*bool memset_absmax=true,*/ cudaStream_t stream=0, size_t block_size=128, T1 *copy=NULL) { // advanced parameters
    assert((w % TRANSPOSE_TILE_SIZE) == 0 && (h % TRANSPOSE_TILE_SIZE) == 0);
    cudaCheck(cudaGetLastError());
    constexpr int DIM_Z = 1;
    block_size /= DIM_Z;

    size_t block_size_x = (TRANSPOSE_TILE_SIZE * sizeof(T2)) / 16;
    size_t block_size_y = min(TRANSPOSE_TILE_SIZE, block_size / block_size_x);
    dim3 grid_size(w / TRANSPOSE_TILE_SIZE, h / (TRANSPOSE_TILE_SIZE * DIM_Z));
    dim3 block_size_dim(block_size_x, block_size_y, DIM_Z);

    constexpr uint absmax_factor = write_absmax ? 1 : 0;
    unsigned int* absmax_uint = (unsigned int*)absmax_output;
    /*if (write_absmax && memset_absmax) {
        cudaMemset(absmax_output, 0, sizeof(unsigned int));
    }*/

    switch (block_size_y) {
        case 64: transpose_kernel<64, TRANSPOSE_TILE_SIZE, reciprocal, enable_copy, true, absmax_factor, elementwise_func><<<grid_size, block_size_dim, 0, stream>>>(transposed, copy, input, h, descale_pointer, scale_pointer, absmax_uint); break;
        case 32: transpose_kernel<32, TRANSPOSE_TILE_SIZE, reciprocal, enable_copy, true, absmax_factor, elementwise_func><<<grid_size, block_size_dim, 0, stream>>>(transposed, copy, input, h, descale_pointer, scale_pointer, absmax_uint); break;
        case 16: transpose_kernel<16, TRANSPOSE_TILE_SIZE, reciprocal, enable_copy, true, absmax_factor, elementwise_func><<<grid_size, block_size_dim, 0, stream>>>(transposed, copy, input, h, descale_pointer, scale_pointer, absmax_uint); break;
        /*case 8: transpose_kernel<8, TRANSPOSE_TILE_SIZE, reciprocal, enable_copy, true, absmax_factor, elementwise_func><<<grid_size, block_size_dim, 0, stream>>>(transposed, copy, input, h, descale_pointer, scale_pointer, absmax_uint,); break;
        case 4: transpose_kernel<4, TRANSPOSE_TILE_SIZE, reciprocal, enable_copy, true, absmax_factor, elementwise_func><<<grid_size, block_size_dim, 0, stream>>>(transposed, copy, input, h, descale_pointer, scale_pointer, absmax_uint); break;
        case 2: transpose_kernel<2, TRANSPOSE_TILE_SIZE, reciprocal, enable_copy, true, absmax_factor, elementwise_func><<<grid_size, block_size_dim, 0, stream>>>(transposed, copy, input, h, descale_pointer, scale_pointer, absmax_uint); break;
        case 1: transpose_kernel<1, TRANSPOSE_TILE_SIZE, reciprocal, enable_copy, true, absmax_factor, elementwise_func><<<grid_size, block_size_dim, 0, stream>>>(transposed, copy, input, h, descale_pointer, scale_pointer, absmax_uint); break;*/
        default: printf("Invalid block size (might be easy to add): %lu\n", block_size_y); exit(1);
    }
    cudaCheck(cudaGetLastError());
}

// wrapper so the parameters of the standard transpose function are less messy
template <bool write_absmax=false, elementwise_func_t elementwise_func=nothing_elementwise, bool reciprocal=true, typename T1, typename T2>
void copy_and_transpose(T1 *transposed, T1 *copy, const T2 *input, size_t w, size_t h, float* descale_pointer=NULL, float* scale_pointer=NULL, unsigned int* absmax_output=NULL, /*bool memset_absmax=true,*/ cudaStream_t stream=0, const size_t block_size=256) {
    transpose<write_absmax, elementwise_func, reciprocal, true, T1, T2>(transposed, input, w, h, descale_pointer, scale_pointer, absmax_output, /*memset_absmax,*/ stream, block_size, copy);
}

template <bool write_absmax=false, elementwise_func_t elementwise_func=nothing_elementwise, bool reciprocal=true, typename T1, typename T2>
void copy_or_transpose(bool transposing, T1 *output, const T2 *input, size_t w, size_t h, float* descale_pointer=NULL, float* scale_pointer=NULL, unsigned int* absmax_output=NULL, /*bool memset_absmax=true,*/ cudaStream_t stream=0, const size_t block_size=0) {
    if (transposing) {
        transpose<write_absmax, elementwise_func, reciprocal, false, T1, T2>(output, input, w, h, descale_pointer, scale_pointer, absmax_output, /*memset_absmax,*/ stream, block_size ? block_size : 256);
    } else {
        copy_advanced<false, elementwise_func, reciprocal>(output, input, w*h, descale_pointer, scale_pointer, absmax_output, /*memset_absmax,*/ stream, block_size ? block_size : 512);
    }
    cudaCheck(cudaGetLastError());
}

template <typename T>
void update_absmax(TensorGPU<T> inp, bool memset_absmax=false, cudaStream_t stream=main_stream, size_t max_block_size=512) {
    size_t N = inp.num_elements;
    if (N == 0 || inp.absmax_ptr == NULL) { return; }

    // find the largest block size that divides N
    size_t block_size = max_block_size;
    while ((N % (block_size * Packed128<T>::size * ABSMAX_ITERATIONS_PER_THREAD)) != 0) {
        block_size /= 2;
        assert(block_size >= 32); // block size of 1 would be OK, but so inefficient we'd rather fail and debug I think
    }

    const dim3 grid_size(CEIL_DIV(N, block_size * ABSMAX_ITERATIONS_PER_THREAD * Packed128<T>::size));
    if (memset_absmax) {
        cudaMemset(inp.absmax_ptr, 0, sizeof(unsigned int));
    }
    update_absmax_kernel<<<grid_size, block_size, 0, stream>>>(inp);
    cudaCheck(cudaGetLastError());
}

// ----------------------------------------------------------------------------
// Scratch allocation for FP8 conversions etc.
// todo - consider alternatives (or at least move it somewhere else)

#include <vector>
#include <algorithm>
#include <cuda_runtime.h>

class CudaScratchAllocator {
private:
    struct Allocation {
        void* ptr;
        size_t size;
        bool in_use;

        Allocation(void* p, size_t s) : ptr(p), size(s), in_use(false) {}
    };

    static std::vector<Allocation> allocations;
    static size_t total_allocated;

public:
    template<typename T>
    static T* getMemory(size_t count, bool exact=false) {
        size_t size = count * sizeof(T);

        // Find the smallest free allocation that fits the requested size
        auto it = std::min_element(allocations.begin(), allocations.end(),
            [size](const Allocation& a, const Allocation& b) {
                return !a.in_use && a.size >= size && (b.in_use || b.size < size || a.size < b.size);
            });

        if (it != allocations.end() && !it->in_use && it->size >= size && (!exact || it->size == size)) {
            it->in_use = true;
            return reinterpret_cast<T*>(it->ptr);
        }

        // If no suitable allocation found, create a new one
        void* new_ptr;
        cudaMalloc(&new_ptr, size);
        allocations.emplace_back(new_ptr, size);
        allocations.back().in_use = true;
        total_allocated += size;
        printf("Allocated CUDA scratch memory: %lu bytes (%p) ==> total allocated: %.1fGiB\n", size, new_ptr, total_allocated / (1024.0 * 1024.0 * 1024.0));
        return reinterpret_cast<T*>(new_ptr);
    }

    template<typename T>
    static void releaseMemory(T* ptr) {
        if (ptr == nullptr) { return; }
        auto it = std::find_if(allocations.begin(), allocations.end(),
            [ptr](const Allocation& a) { return a.ptr == (void*)ptr; });

        if (it != allocations.end()) {
            it->in_use = false;
        }
    }

    static void cleanup() {
        for (const auto& alloc : allocations) {
            cudaFree(alloc.ptr);
        }
        allocations.clear();
    }
};
std::vector<CudaScratchAllocator::Allocation> CudaScratchAllocator::allocations;
size_t CudaScratchAllocator::total_allocated = 0;

// ----------------------------------------------------------------------------
// Transposed Cache (for FP8 weights)

#include <functional>

// Custom hash function for std::pair<uint64_t, uint64_t>
// todo - why did we need this? complained about default constructor issue?
struct PairHash {
    std::size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
        return std::hash<uint64_t>{}(p.first) ^ (std::hash<uint64_t>{}(p.second) << 1);
    }
};

class TransposedCache {
private:
    struct CacheEntry {
        void* ptr;
        size_t size;
    };

    std::unordered_map<std::pair<uint64_t, uint64_t>, CacheEntry, PairHash> cache;

public:
    TransposedCache() = default;

    template<typename T, typename Tout=T>
    Tout* getTransposed(const T* original, const void* associatedTensor, size_t m, size_t k, bool compute=true, bool find_only=false, cudaStream_t stream=0) {
        uint64_t key1 = reinterpret_cast<uint64_t>(original);
        uint64_t key2 = reinterpret_cast<uint64_t>(associatedTensor);
        auto key = std::make_pair(key1, key2);
        size_t size = m * k * sizeof(T);

        auto it = cache.find(key);
        if (it != cache.end() && it->second.size == size) {
            return reinterpret_cast<Tout*>(it->second.ptr);
        }
        if (find_only) {
            return nullptr;
        }

        Tout* transposed = CudaScratchAllocator::getMemory<Tout>(m * k, true);
        if (compute) {
            copy_or_transpose<false>(true, transposed, original, m, k, nullptr, nullptr, nullptr, stream);
        }

        cache[key] = {transposed, size};
        return transposed;
    }

    void clearCache() {
        for (const auto& entry : cache) {
            CudaScratchAllocator::releaseMemory(entry.second.ptr);
        }
        cache.clear();
    }
};
TransposedCache g_transposed_cache;

#endif