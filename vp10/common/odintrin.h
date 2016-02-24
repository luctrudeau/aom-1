#include "vp10/common/enums.h"
#include "vpx/vpx_integer.h"
#include "vpx_dsp/vpx_dsp_common.h"
#include "vpx_ports/bitops.h"

/*Smallest blocks are 4x4*/
# define OD_LOG_BSIZE0 (2)
/*There are 5 block sizes total (4x4, 8x8, 16x16, 32x32 and 64x64).*/
# define OD_NBSIZES    (5)
/*The log of the maximum length of the side of a block.*/
# define OD_LOG_BSIZE_MAX (OD_LOG_BSIZE0 + OD_NBSIZES - 1)
/*The maximum length of the side of a block.*/
# define OD_BSIZE_MAX     (1 << OD_LOG_BSIZE_MAX)

typedef int od_coeff;
#define OD_COEFF_SHIFT (0)  // NB: differs from daala

typedef int16_t dering_in;

#define OD_DIVU_SMALL(_x, _d) ((_x) / (_d))

#define OD_MINI VPXMIN
#define OD_CLAMPI(min, val, max) clamp((val), (min), (max))

#  define OD_ILOG_NZ(x) get_msb(x)
/*Note that __builtin_clz is not defined when x == 0, according to the gcc
 *    documentation (and that of the x86 BSR instruction that implements it), so
 *       we have to special-case it.
 *         We define a special version of the macro to use when x can be zero.*/
#  define OD_ILOG(x) ((x) ? OD_ILOG_NZ(x) : 0)
