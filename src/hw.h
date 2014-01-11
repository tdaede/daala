#ifndef HW_H
#define HW_H
#include <stdint.h>
#include "filter.h"

struct od_hw_ctx {
  int fd;
  volatile uint8_t* ocm;
  volatile uint32_t* dma;
};
typedef struct od_hw_ctx od_hw_ctx;

void od_hw_init(od_hw_ctx * hw);

void od_hw_idct4x4(od_hw_ctx *hw, od_coeff *_x, int _xstride, const od_coeff *_y,
 int _ystride);
 
#endif
