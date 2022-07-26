/*
 * gcc -msse4.2 -O3 -Wall -fPIC -shared -o libmotion.so motion.c
 */
#include <xmmintrin.h>
#include <nmmintrin.h>

long count_different_bytes(
    unsigned char *arr1,
    unsigned char *arr2,
    unsigned long length,
    unsigned char threshold
) {
    __m128i mthreshold = _mm_set1_epi8((char)threshold);
    __m128i m0 = _mm_setzero_si128();
    unsigned long result = 0;
    unsigned long i;

    if ((unsigned long)arr1 & 15 || (unsigned long)arr2 & 15 || length & 15)
        return -1;

    for (i = 0; i < length; i += 16) {
        // load next 16 bytes from arr1 and arr2
        __m128i m1 = _mm_loadu_si128((__m128i *) &arr1[i]);
        __m128i m2 = _mm_loadu_si128((__m128i *) &arr2[i]);

        // calculate absolute difference
        __m128i absdiff = _mm_adds_epu8(_mm_subs_epu8(m1, m2),
                                        _mm_subs_epu8(m2, m1));

        // remove noise and compare to zeros
        __m128i mres = _mm_cmpeq_epi8(_mm_subs_epu8(absdiff, mthreshold), m0);

        // count zero bytes
        result += _mm_popcnt_u32(_mm_movemask_epi8(mres));
    }

    return length - result;
}
