/*! \file */
/* ************************************************************************
 * Copyright (C) 2019-2023 Advanced Micro Devices, Inc. All rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ************************************************************************ */

#include "testing.hpp"

template <typename T>
void testing_dotci_bad_arg(const Arguments& arg)
{
    static const size_t safe_size = 100;

    T h_result;

    // Create rocsparse handle
    rocsparse_local_handle local_handle;

    rocsparse_handle     handle = local_handle;
    rocsparse_int        nnz    = safe_size;
    const T*             x_val  = (const T*)0x4;
    const rocsparse_int* x_ind  = (const rocsparse_int*)0x4;
    const T*             y      = (T*)0x4;
    T*                   result = &h_result;
    rocsparse_index_base base   = rocsparse_index_base_zero;

#define PARAMS handle, nnz, x_val, x_ind, y, result, base
    auto_testing_bad_arg(rocsparse_dotci<T>, PARAMS);
#undef PARAMS
}

template <typename T>
void testing_dotci(const Arguments& arg)
{
    rocsparse_int        M    = arg.M;
    rocsparse_int        nnz  = arg.nnz;
    rocsparse_index_base base = arg.baseA;

    // Create rocsparse handle
    rocsparse_local_handle handle(arg);

    // Grab stream used by handle
    hipStream_t stream = handle.get_stream();

    // Argument sanity check before allocating invalid memory
    if(nnz <= 0)
    {
        static const size_t safe_size = 100;

        // Allocate memory on device
        device_vector<rocsparse_int> dx_ind(safe_size);
        device_vector<T>             dx_val(safe_size);
        device_vector<T>             dy(safe_size);

        if(!dx_ind || !dx_val || !dy)
        {
            CHECK_HIP_ERROR(hipErrorOutOfMemory);
            return;
        }

        T result;

        CHECK_ROCSPARSE_ERROR(rocsparse_set_pointer_mode(handle, rocsparse_pointer_mode_host));
        EXPECT_ROCSPARSE_STATUS(rocsparse_dotci<T>(handle, nnz, dx_val, dx_ind, dy, &result, base),
                                nnz < 0 ? rocsparse_status_invalid_size : rocsparse_status_success);

        return;
    }

    // Allocate host memory
    host_vector<rocsparse_int> hx_ind(nnz);
    host_vector<T>             hx_val(nnz);
    host_vector<T>             hy(M);
    host_vector<T>             hdot_1(1);
    host_vector<T>             hdot_2(1);
    host_vector<T>             hdot_gold(1);

    // Initialize data on CPU
    rocsparse_seedrand();
    rocsparse_init_index(hx_ind, nnz, 1, M);
    rocsparse_init_alternating_sign<T>(hx_val, 1, nnz, 1);
    rocsparse_init_exact<T>(hy, 1, M, 1);

    // Allocate device memory
    device_vector<rocsparse_int> dx_ind(nnz);
    device_vector<T>             dx_val(nnz);
    device_vector<T>             dy(M);
    device_vector<T>             ddot_2(1);

    if(!dx_ind || !dx_val || !dy || !ddot_2)
    {
        CHECK_HIP_ERROR(hipErrorOutOfMemory);
        return;
    }

    // Copy data from CPU to device
    CHECK_HIP_ERROR(hipMemcpy(dx_ind, hx_ind, sizeof(rocsparse_int) * nnz, hipMemcpyHostToDevice));
    CHECK_HIP_ERROR(hipMemcpy(dx_val, hx_val, sizeof(T) * nnz, hipMemcpyHostToDevice));
    CHECK_HIP_ERROR(hipMemcpy(dy, hy, sizeof(T) * M, hipMemcpyHostToDevice));

    if(arg.unit_check)
    {
        // Pointer mode host
        CHECK_ROCSPARSE_ERROR(rocsparse_set_pointer_mode(handle, rocsparse_pointer_mode_host));
        CHECK_ROCSPARSE_ERROR(
            testing::rocsparse_dotci<T>(handle, nnz, dx_val, dx_ind, dy, &hdot_1[0], base));
        CHECK_HIP_ERROR(hipStreamSynchronize(stream));

        // Pointer mode device
        CHECK_ROCSPARSE_ERROR(rocsparse_set_pointer_mode(handle, rocsparse_pointer_mode_device));
        CHECK_ROCSPARSE_ERROR(
            testing::rocsparse_dotci<T>(handle, nnz, dx_val, dx_ind, dy, ddot_2, base));

        // Copy output to host
        CHECK_HIP_ERROR(hipMemcpy(hdot_2, ddot_2, sizeof(T), hipMemcpyDeviceToHost));

        // CPU dotci
        host_dotci<rocsparse_int, T>(nnz, hx_val, hx_ind, hy, hdot_gold, base);

        hdot_gold.unit_check(hdot_1);
        hdot_gold.unit_check(hdot_2);
    }

    if(arg.timing)
    {
        int number_cold_calls = 2;
        int number_hot_calls  = arg.iters;

        CHECK_ROCSPARSE_ERROR(rocsparse_set_pointer_mode(handle, rocsparse_pointer_mode_host));

        // Warm up
        for(int iter = 0; iter < number_cold_calls; ++iter)
        {
            CHECK_ROCSPARSE_ERROR(
                rocsparse_dotci<T>(handle, nnz, dx_val, dx_ind, dy, &hdot_1[0], base));
            CHECK_HIP_ERROR(hipStreamSynchronize(stream));
        }

        double gpu_time_used = get_time_us();

        // Performance run
        for(int iter = 0; iter < number_hot_calls; ++iter)
        {
            CHECK_ROCSPARSE_ERROR(
                rocsparse_dotci<T>(handle, nnz, dx_val, dx_ind, dy, &hdot_1[0], base));
            CHECK_HIP_ERROR(hipStreamSynchronize(stream));
        }

        gpu_time_used = (get_time_us() - gpu_time_used) / number_hot_calls;

        double gflop_count = doti_gflop_count(nnz);
        double gbyte_count = doti_gbyte_count<T>(nnz);

        double gpu_gbyte  = get_gpu_gbyte(gpu_time_used, gbyte_count);
        double gpu_gflops = get_gpu_gflops(gpu_time_used, gflop_count);

        display_timing_info("nnz",
                            nnz,
                            s_timing_info_perf,
                            gpu_gflops,
                            s_timing_info_bandwidth,
                            gpu_gbyte,
                            s_timing_info_time,
                            get_gpu_time_msec(gpu_time_used));
    }
}

#define INSTANTIATE(TYPE)                                            \
    template void testing_dotci_bad_arg<TYPE>(const Arguments& arg); \
    template void testing_dotci<TYPE>(const Arguments& arg)
// INSTANTIATE(float);
// INSTANTIATE(double);
INSTANTIATE(rocsparse_float_complex);
INSTANTIATE(rocsparse_double_complex);
void testing_dotci_extra(const Arguments& arg) {}
