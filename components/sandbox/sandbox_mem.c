#include "sandbox.h"

/* Pointer to dynamically allocated arena block in internal DRAM */
uint8_t *g_sandbox_arena = NULL;

/* Total capacity of the active dynamic arena in bytes */
size_t g_sandbox_arena_size = 0;

/* Dedicated U-mode stack aligned per RISC-V RV32 calling convention */
uint8_t g_sandbox_stack[SANDBOX_STACK_SIZE] __attribute__((aligned(16)));
