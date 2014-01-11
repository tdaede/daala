#include <stdint.h>

struct od_hw_ctx {
  int fd;
  volatile uint8_t* ocm;
  volatile uint32_t* dma;
};
typedef struct od_hw_ctx od_hw_ctx;

void od_hw_init(od_hw_ctx * hw);

void od_hw_idct_4x4(od_hw_ctx *hw, int output_offset, int input_offset, int num_blocks);
