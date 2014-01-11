#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "internal.h"
#include "hw.h"
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

#define MM2S_DMACR (0/4)
#define MM2S_DMASR (4/4)
#define MM2S_CURDESC (0x08/4)
#define MM2S_TAILDESC (0x10/4)
#define MM2S_SA (0x18/4)
#define MM2S_LENGTH (0x28/4)
#define SG_CTL (0x2C/4)
#define S2MM_DMACR (0x30/4)
#define S2MM_DMASR (0x34/4)
#define S2MM_CURDESC (0x38/4)
#define S2MM_TAILDESC (0x40/4)
#define S2MM_DA (0x48/4)
#define S2MM_LENGTH (0x58/4)

#define DMA_RS (1<<0)
#define DMA_Reset (1<<2)
#define DMA_Halted (1<<0)
#define DMA_Idle (1<<1)

#define BLOCK_4X4_BYTES (16*2)

#define OCM_ADDR (0xfffc0000)
#define OCM_OUTPUT_ADDR (OCM_ADDR)
#define OCM_INPUT_ADDR (OCM_ADDR+OCM_BUFFER_SIZE)
#define OCM_SIZE (256*1024)

#define BRAM_ADDR (0xC0000000)
#define BRAM_SIZE (8192)

void od_hw_init(od_hw_ctx *hw) {
  hw->fd = open("/dev/mem", O_RDWR|O_SYNC);
  hw->ocm = mmap(0, 1024*256, PROT_READ|PROT_WRITE, MAP_SHARED, hw->fd, 0xfffc0000);
  hw->dma = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, hw->fd, 0x40400000);
  printf("Resetting DMA...\n");
  hw->dma[MM2S_DMACR] |= DMA_Reset;
  while(hw->dma[MM2S_DMACR] & DMA_Reset);
  printf("Reset complete\n");
  printf("Enabling DMA...\n");
  hw->dma[MM2S_DMACR] |= DMA_RS;
  while(hw->dma[MM2S_DMASR] & DMA_Halted);
  hw->dma[S2MM_DMACR] |= DMA_RS;
  while(hw->dma[S2MM_DMASR] & DMA_Halted);
  printf("DMA enabled\n");
}

void od_hw_idct_4x4(od_hw_ctx *hw, int output_offset, int input_offset, int num_blocks) {
  OD_ASSERT(num_blocks < (BRAM_SIZE/BLOCK_4X4_BYTES));
  hw->dma[MM2S_SA] = OCM_ADDR + input_offset;
  hw->dma[MM2S_LENGTH] = BLOCK_4X4_BYTES * num_blocks;
  hw->dma[S2MM_DA] = BRAM_ADDR;
  hw->dma[S2MM_LENGTH] = BLOCK_4X4_BYTES * num_blocks;
  while(!(hw->dma[S2MM_DMASR] & DMA_Idle));
  hw->dma[MM2S_SA] = BRAM_ADDR;
	hw->dma[MM2S_LENGTH] = BLOCK_4X4_BYTES * num_blocks;
	hw->dma[S2MM_DA] = OCM_ADDR + output_offset;
	hw->dma[S2MM_LENGTH] = BLOCK_4X4_BYTES * num_blocks;
	while(!(hw->dma[S2MM_DMASR] & DMA_Idle));
}
