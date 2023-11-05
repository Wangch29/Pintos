#ifndef PINTOS_FLOAT_H
#define PINTOS_FLOAT_H

#include <stdint.h>

/** The quanta in fix-point format(p.q).
    q = 14, f = 1 << 14 = 16384. */
static int32_t f = 16384;

typedef int32_t fixed_point;

static inline fixed_point itof(int n) {
  return n * f;
}

static inline int ftoi(fixed_point x) {
  return x / f;
}

static inline int ftoi_round(fixed_point x) {
  return x >= 0 ? (x + f / 2) / f : (x - f / 2) / f;
}

static inline fixed_point add_fi(fixed_point x, int n) {
  return x + n * f;
}

static inline fixed_point sub_fi(fixed_point x, int n) {
  return x - n * f;
}

static inline fixed_point mul_ff(fixed_point x, fixed_point y) {
  return ((int64_t)x) * y / f;
}

static inline fixed_point div_ff(fixed_point x, fixed_point y) {
  return ((int64_t)x) * f / y;
}

#endif /**< lib/float.h */
