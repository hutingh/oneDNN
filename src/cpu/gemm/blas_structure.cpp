/*******************************************************************************
* Copyright 2019 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <cstdint>
#include <mutex>

#include "blas_structure.hpp"

#include "cpu_isa_traits.hpp"
#include "jit_generator.hpp"
#include "mkldnn_traits.hpp"
#include "mkldnn_types.h"
#include "f32/common_f32.hpp"
#include "s8x8s32/common_u8.hpp"
#include "s8x8s32/jit_avx512_core_gemm_s8u8s32_kern.hpp"
#include "s8x8s32/jit_avx512_core_kernel_gemv_s8u8s32_kern.hpp"
#include "f32/jit_avx2_kernel_sgemm_kern.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

// TODO Is it okay to place this here?
enum {no_sum = 0, do_sum = 1};
enum {no_trans = 0, do_trans = 1};
enum {no_beta0 = 0, do_beta0 = 1};
enum {no_col_offset = 0, do_col_offset = 1};
enum {no_row_offset = 0, do_row_offset = 1};

template <typename a_type, typename b_type, typename c_type>
BlasStructure<a_type, b_type, c_type>::BlasStructure(const char *transA,
        const char *transB, const char *offsetC, const int *m, const int *n,
        const int *k, const float *alpha, const a_type *a, const int *lda,
        const a_type *oa, const b_type *b, const int *ldb, const a_type *ob,
        const float *beta, c_type *c, const int *ldc, const c_type *oc)
{
    char transa = *transA;
    char transb = *transB;

    this->transa  = (transa == 'N' || transa == 'n') ? 0 : 1;
    this->transb  = (transb == 'N' || transb == 'n') ? 0 : 1;
    this->m       = *m;
    this->n       = *n;
    this->k       = *k;
    this->alpha   = alpha;
    this->a       = a;
    this->lda     = *lda;
    this->b       = b;
    this->ldb     = *ldb;
    this->beta    = beta;
    this->c       = c;
    this->ldc     = *ldc;
    this->ao      = 0;
    this->bo      = 0;
    this->co      = NULL;
    this->offsetc = NO_OFFSET;

    if (data_traits<a_type>::data_type == data_type::s8) {
        this->ao = *oa;
        this->bo = *ob;
    }


    if (offsetC != NULL) {
        char offsetc = *offsetC;
        if (offsetc == 'F' || offsetc == 'f') {
            this->offsetc = FIX_OFFSET;
        } else if (offsetc == 'R' || offsetc == 'r') {
            this->offsetc = ROW_OFFSET;
        } else { // offsetc == 'C' || offsetc == 'c'
            this->offsetc = COL_OFFSET;
        }
        this->co = oc;
    }

    this->jit_init();
}

template<typename a_type, typename b_type, typename c_type>
void BlasStructure<a_type, b_type, c_type>::jit_init(void)
{
    static void (*copyA[2][2])(const dim_t *m, const dim_t *n,
            const a_type *src, const dim_t *ldsrc, const float *alpha,
            a_type *dst, const dim_t *dummy1, const dim_t *dummy2,
            c_type *row_col_sum) = {{NULL}};

    static void (*copyB[2][2])(const dim_t *m, const dim_t *n,
            const b_type *src, const dim_t *ldsrc, const float *alpha,
            b_type *dst, const dim_t *dummy1, const dim_t *dummy2,
            c_type *row_col_sum) = {{NULL}};

    static void (*kern[2][2][2])(const dim_t *m, const dim_t *n, const dim_t *k,
            const float *alpha, const a_type *a, const b_type *b, c_type *c,
            const dim_t ldc, const c_type *col_offset,
            const c_type *row_offset) = {{{NULL}}};

    static void (*gemv_s8u8s32_kern)(const dim_t, const dim_t, const float,
            const int8_t *, const dim_t, const uint8_t *, const float,
            int32_t *) = {NULL};

    static void (*gemv_u8s8s32_kern)(const dim_t, const dim_t, const float,
            const uint8_t *, const dim_t, const int8_t *, const float,
            int32_t *) = {NULL};

    switch (data_traits<a_type>::data_type) {
    case data_type::s8:
        if (mayiuse(avx512_core)) {
            this->um =   48;
            this->un =    8;
            this->uk =    1;
            this->bm = 9984;
            this->bn =  384;
            this->bk = mayiuse(avx512_core_vnni) ? 1536 : 768;

            this->bk_traditional   = 384;
            this->blocking_small_k =  48;
            this->bn_small_k       =  24;
        }
        break;

    case data_type::f32:
        if (mayiuse(avx512_core)) {
            this->um =   48;
            this->un =    8;
            this->uk =    1;
            this->bm = 9984;
            this->bn =  384;
            this->bk =  384;

            this->bk_traditional   = 384;
            this->blocking_small_k =  48;
            this->bn_small_k       =  24;

        } else if (mayiuse(avx2)) {
            this->um =    24;
            this->un =     4;
            this->uk =     1;
            this->bm = 10000;
            this->bn =   384;
            this->bk =   192;

            this->bk_traditional   = 256;
            this->blocking_small_k =  48;
            this->bn_small_k       =  24;
        }
        break;
    }

    static std::once_flag initialized;
    std::call_once(initialized, []{

        static jit_generator *copy_a[2][2] = {{NULL}};
        static jit_generator *copy_b[2][2] = {{NULL}};

        switch (data_traits<a_type>::data_type) {
        case data_type::s8:
            if (mayiuse(avx512_core)) {
                copy_a[no_trans][no_sum] = new jit_avx512_core_u8_copy_an_kern();
                copy_a[do_trans][no_sum] = new jit_avx512_core_u8_copy_at_kern();
                copy_a[no_trans][do_sum] = new jit_avx512_core_u8_copy_sum_an_kern();
                copy_a[do_trans][do_sum] = new jit_avx512_core_u8_copy_sum_at_kern();

                copy_b[no_trans][no_sum] = new jit_avx512_core_u8_copy_bn_kern();
                copy_b[do_trans][no_sum] = new jit_avx512_core_u8_copy_bt_kern();
                copy_b[no_trans][do_sum] = new jit_avx512_core_u8_copy_sum_bn_kern();
                copy_b[do_trans][do_sum] = new jit_avx512_core_u8_copy_sum_bt_kern();
            }
            break;

        case data_type::f32:
            if (mayiuse(avx512_core)) {
                copy_a[no_trans][no_sum] = new jit_avx512_core_f32_copy_an_kern();
                copy_a[do_trans][no_sum] = new jit_avx512_core_f32_copy_at_kern();

                copy_b[no_trans][no_sum] = new jit_avx512_core_f32_copy_bn_kern();
                copy_b[do_trans][no_sum] = new jit_avx512_core_f32_copy_bt_kern();
            } else if (mayiuse(avx2)) {
                copy_a[no_trans][no_sum] = new jit_avx2_f32_copy_an_kern();
                copy_a[do_trans][no_sum] = new jit_avx2_f32_copy_at_kern();

                copy_b[no_trans][no_sum] = new jit_avx2_f32_copy_bn_kern();
                copy_b[do_trans][no_sum] = new jit_avx2_f32_copy_bt_kern();
            }
            break;
        }

        static jit_generator *kernel[2][2][2] = {{{NULL}}};
        switch (data_traits<a_type>::data_type) {
        case data_type::s8:
            if (mayiuse(avx512_core)) {
                for (int isBeta0 : {no_beta0, do_beta0})
                    for (int isColOffset : {no_col_offset, do_col_offset})
                        for (int isRowOffset : {no_row_offset, do_row_offset}) {
                            kernel[isBeta0][isColOffset][isRowOffset] = new
                                jit_avx512_core_gemm_s8u8s32_kern(isBeta0,
                                        isColOffset, isRowOffset);
                        }
            }
            break;

        case data_type::f32:
            kernel[no_beta0][no_col_offset][no_row_offset] = new jit_avx2_kernel_sgemm_kern(false);
            kernel[do_beta0][no_col_offset][no_row_offset] = new jit_avx2_kernel_sgemm_kern(true);
            break;
        }

        static jit_avx512_core_gemv_s8u8s32_kern *gemv_s8u8s32_kernel = NULL;
        static jit_avx512_core_gemv_s8u8s32_kern *gemv_u8s8s32_kernel = NULL;
        if (data_traits<a_type>::data_type == data_type::s8) {
            if (mayiuse(avx512_core)) {
                gemv_s8u8s32_kernel = new jit_avx512_core_gemv_s8u8s32_kern();
                gemv_u8s8s32_kernel = new jit_avx512_core_gemv_s8u8s32_kern();
            }
        }

        // Set copy kernels function pointer table
        for (int isTrans : {no_trans, do_trans})
            for (int isSum : {no_sum, do_sum}) {
                auto *p_copy_a = copy_a[isTrans][isSum];
                if (p_copy_a != NULL)
                    copyA[isTrans][isSum] = p_copy_a->getCode<
                        void (*)(const dim_t *, const dim_t *, const a_type *,
                                const dim_t *, const float *, a_type *,
                                const dim_t *, const dim_t *, c_type *)>();
                auto *p_copy_b = copy_b[isTrans][isSum];
                if (p_copy_b != NULL)
                    copyB[isTrans][isSum] = p_copy_b->getCode<
                        void (*)(const dim_t *, const dim_t *, const b_type *,
                                const dim_t *, const float *, b_type *,
                                const dim_t *, const dim_t *, c_type *)>();
            }

        // Set compute kernel function pointer table
        for (int isBeta0 : {no_beta0, do_beta0})
            for (int isColOffset : {no_col_offset, do_col_offset})
                for (int isRowOffset : {no_row_offset, do_row_offset}) {
                    auto *p_kernel = kernel[isBeta0][isColOffset][isRowOffset];
                    if (p_kernel != NULL)
                        kern[isBeta0][isColOffset][isRowOffset] =
                            p_kernel->getCode<
                            void (*)(const dim_t *, const dim_t *,
                                    const dim_t *, const float *,
                                    const a_type *, const b_type *, c_type *,
                                    const dim_t, const c_type *,
                                    const c_type *)>();
                }

        // Set gemv integer gemm kernels
        if (data_traits<a_type>::data_type == data_type::s8) {
            gemv_s8u8s32_kern = gemv_s8u8s32_kernel ->
                generate<jit_avx512_core_gemv_s8u8s32_kern::gemv_s8u8s32_kernel_t>
                (mayiuse(avx512_core_vnni));

            gemv_u8s8s32_kern = gemv_u8s8s32_kernel ->
                generate<jit_avx512_core_gemv_s8u8s32_kern::gemv_u8s8s32_kernel_t>
                (mayiuse(avx512_core_vnni));
        }
    });

    // TODO We should use enums for transa and transb instead of 1 or 0
    int isTransA = this->transa == 1 ? do_trans : no_trans;
    int isTransB = this->transb == 1 ? do_trans : no_trans;

    int doSumA = this->bo != 0 ? do_sum : no_sum;
    int doSumB = this->ao != 0 ? do_sum : no_sum;

    this->copyA = copyA[isTransA][doSumA];
    this->copyB = copyB[isTransB][doSumB];

    for (int isBeta0 : {no_beta0, do_beta0})
        for (int isColOffset : {no_col_offset, do_col_offset})
            for (int isRowOffset : {no_row_offset, do_row_offset})
                this->kernel[isBeta0][isColOffset][isRowOffset] =
                    kern[isBeta0][isColOffset][isRowOffset];

    this->gemv_s8u8s32_kernel = NULL;
    this->gemv_u8s8s32_kernel = NULL;
    if (data_traits<a_type>::data_type == data_type::s8) {
        this->gemv_s8u8s32_kernel = gemv_s8u8s32_kern;
        this->gemv_u8s8s32_kernel = gemv_u8s8s32_kern;
    }
}

// Check if copy algorithm kernels were generated on supported ISAs.
// Copy algorithm supported for:
//      s8  : avx512_core, avx512_vnni
//      f32 : avx2, avx512_core
template <typename a_type, typename b_type, typename c_type>
bool BlasStructure<a_type, b_type, c_type>::hasKernels(void)
{
    switch (data_traits<a_type>::data_type) {

    case data_type::s8:
        if (mayiuse(avx512_core)) {
            for (int isBeta0 : {no_beta0, do_beta0})
                for (int isColOffset : {no_col_offset, do_col_offset})
                    for (int isRowOffset : {no_row_offset, do_row_offset})
                        if (!this->kernel[isBeta0][isColOffset][isRowOffset])
                            return false;

            if (!this->gemv_s8u8s32_kernel || !this->gemv_u8s8s32_kernel)
                return false;

            if (!this->copyA || !this->copyB)
                return false;
        }

        break;

    case data_type::f32:
        if (mayiuse(avx2)) {
            for (int isBeta0 : {no_beta0, do_beta0})
                if (!this->kernel[isBeta0][no_col_offset][no_row_offset])
                    return false;

            if (!this->copyA || !this->copyB)
                return false;

        }

        break;
    }

    // All kernels necessary have been found or ISA is not supported.
    return true;
}

// Instantiate the BlasStructure templates needed.
template struct BlasStructure<int8_t,  uint8_t, int32_t>; // For gemm_s8u8s32
template struct BlasStructure<float,   float,   float>;   // For sgemm

}
}
}
