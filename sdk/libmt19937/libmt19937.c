#include <mt19937.h>

#define MT_N 624
#define MT_M 397
#define MT_MATRIX_A 0x9908b0dfU
#define MT_UPPER_MASK 0x80000000U
#define MT_LOWER_MASK 0x7fffffffU

static unsigned int mt[MT_N];
static int mti = MT_N + 1;

int get_state_MT(void **state)
{
    if (!state) {
        return -1;
    }
    *state = mt;
    return mti;
}

void init_MT(unsigned int s)
{
    mt[0] = s;
    for (mti = 1; mti < MT_N; ++mti) {
        mt[mti] = 1812433253U * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + (unsigned int)mti;
    }
}

void init_by_array_MT(unsigned int init_key[], int key_length)
{
    int i = 1;
    int j = 0;
    int k;

    init_MT(19650218U);
    k = (MT_N > key_length) ? MT_N : key_length;
    for (; k > 0; --k) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525U)) +
                init_key[j] + (unsigned int)j;
        ++i;
        ++j;
        if (i >= MT_N) {
            mt[0] = mt[MT_N - 1];
            i = 1;
        }
        if (j >= key_length) {
            j = 0;
        }
    }
    for (k = MT_N - 1; k > 0; --k) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941U)) -
                (unsigned int)i;
        ++i;
        if (i >= MT_N) {
            mt[0] = mt[MT_N - 1];
            i = 1;
        }
    }
    mt[0] = MT_UPPER_MASK;
}

unsigned int rand_int32_MT(void)
{
    static const unsigned int mag01[2] = {0U, MT_MATRIX_A};
    unsigned int y;
    int kk;

    if (mti >= MT_N) {
        if (mti == MT_N + 1) {
            init_MT(5489U);
        }

        for (kk = 0; kk < MT_N - MT_M; ++kk) {
            y = (mt[kk] & MT_UPPER_MASK) | (mt[kk + 1] & MT_LOWER_MASK);
            mt[kk] = mt[kk + MT_M] ^ (y >> 1) ^ mag01[y & 1U];
        }
        for (; kk < MT_N - 1; ++kk) {
            y = (mt[kk] & MT_UPPER_MASK) | (mt[kk + 1] & MT_LOWER_MASK);
            mt[kk] = mt[kk + (MT_M - MT_N)] ^ (y >> 1) ^ mag01[y & 1U];
        }
        y = (mt[MT_N - 1] & MT_UPPER_MASK) | (mt[0] & MT_LOWER_MASK);
        mt[MT_N - 1] = mt[MT_M - 1] ^ (y >> 1) ^ mag01[y & 1U];
        mti = 0;
    }

    y = mt[mti++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= y >> 18;
    return y;
}

unsigned int rand_int31_MT(void)
{
    return rand_int32_MT() >> 1;
}

float rand_real1_MT(void)
{
    return (float)(rand_int32_MT() * (1.0 / 4294967295.0));
}

float rand_real2_MT(void)
{
    return (float)(rand_int32_MT() * (1.0 / 4294967296.0));
}

float rand_real3_MT(void)
{
    return (float)(((double)rand_int32_MT() + 0.5) * (1.0 / 4294967296.0));
}
