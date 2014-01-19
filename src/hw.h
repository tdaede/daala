#ifndef HW_H
#define HW_H
#include <stdint.h>
#include "filter.h"

struct od_hw_block {
  od_coeff* dest;
  int stride;
};
typedef struct od_hw_block od_hw_block;

struct od_hw_ctx {
  int available;
  int fd;
  volatile uint8_t* ocm;
  volatile uint32_t* dma;
  volatile od_coeff* block_in;
  volatile od_coeff* block_out;
  od_hw_block block_queue[1000];
  int block_queue_index;
};
typedef struct od_hw_ctx od_hw_ctx;

void od_hw_init(od_hw_ctx * hw);

void od_hw_idct4x4(od_hw_ctx *hw, od_coeff *_x, int _xstride, const od_coeff *_y,
 int _ystride);

void od_hw_submit_block(od_hw_ctx *hw, od_coeff *_x, int _xstride, od_coeff *_y, int _ystride);

void od_hw_flush(od_hw_ctx *hw);
 
#endif
