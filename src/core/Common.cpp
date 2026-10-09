#include "core/Common.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace echo {

void enableFlushDenormals() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_setcsr(_mm_getcsr() | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#endif
}

} // namespace echo
