/*
 * mt19937.h - Mersenne Twister MT19937 pseudorandom number generator.
 *
 * Reference algorithm: Matsumoto & Nishimura (1998), "Mersenne Twister:
 * A 623-dimensionally equidistributed uniform pseudorandom number
 * generator", ACM Trans. Model. Comput. Simul. 8(1):3-30.
 *
 * Link with -lmt19937 for the compiled state-machine implementation.
 */
#ifndef __PS3DK_MT19937_H__
#define __PS3DK_MT19937_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns a pointer to the internal state vector.  The returned pointer
 * can be used to save/restore generator state across sessions.
 */
int get_state_MT(void **state);

/*
 * Seed the generator with a single 32-bit value.
 */
void init_MT(unsigned int s);

/*
 * Seed the generator with an array of unsigned integers.
 */
void init_by_array_MT(unsigned int init_key[], int key_length);

/*
 * Generate a 32-bit unsigned integer (full 32 bits of state).
 * This is the most efficient generator call.
 */
unsigned int rand_int32_MT(void);

/*
 * Generate a 31-bit unsigned integer (bit 31 cleared).  Slightly
 * faster than rand_int32_MT on some platforms when the caller only
 * needs a positive signed 32-bit range.
 */
unsigned int rand_int31_MT(void);

/*
 * Generate a uniform float on the closed interval [0, 1].
 */
float rand_real1_MT(void);

/*
 * Generate a uniform float on the half-open interval [0, 1).
 */
float rand_real2_MT(void);

/*
 * Generate a uniform float on the open interval (0, 1).
 */
float rand_real3_MT(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_MT19937_H__ */
