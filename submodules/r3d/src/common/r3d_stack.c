/* r3d_stack.c -- Temporary bump/stack allocator with push/pop scopes.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./r3d_stack.h"
#include <raylib.h>
#include <string.h>

#include "./r3d_helper.h"

// ========================================
// INTERNAL HELPERS
// ========================================

static inline size_t stack_total_size(size_t capacity)
{
    return sizeof(r3d_stack_t) + capacity;
}

static inline void stack_set_pointer(r3d_stack_t* stack)
{
    stack->memory = (uint8_t*)stack + sizeof(r3d_stack_t);
}

/* Reallocate to at least 'minCapacity' bytes (geometric x2 growth).
 * Only bytes below 'cursor' are copied; the rest is unused scratch. */
static r3d_stack_t* stack_grow(r3d_stack_t* stack, size_t minCapacity)
{
    size_t newCap = (stack->capacity > 0) ? stack->capacity : 64;

    while (newCap < minCapacity)
    {
        size_t doubled = newCap * 2;
        if (doubled <= newCap) // Overflow
        {
            newCap = minCapacity;
            break;
        }
        newCap = doubled;
    }

    r3d_stack_t* newStack = r3d_zalloc(stack_total_size(newCap));
    *newStack = *stack;
    newStack->capacity = newCap;

    stack_set_pointer(newStack);

    if (stack->cursor > 0)
    {
        memcpy(newStack->memory, stack->memory, stack->cursor);
    }

    r3d_free(stack);
    return newStack;
}

// ========================================
// STACK FUNCTIONS
// ========================================

r3d_stack_t* r3d_stack_create(size_t capacity)
{
    R3D_ASSERT(capacity > 0 && "r3d_stack: capacity must be non-zero");

    r3d_stack_t* stack = r3d_zalloc(stack_total_size(capacity));
    stack->capacity    = capacity;
    stack->cursor      = 0;
    stack->depth       = 0;

    stack_set_pointer(stack);

    return stack;
}

void r3d_stack_destroy(r3d_stack_t* stack)
{
    r3d_free(stack);
}

void r3d_stack_push(r3d_stack_t** stackPtr, size_t reserve)
{
    r3d_stack_t* stack = *stackPtr;

    R3D_ASSERT(stack->depth < R3D_STACK_MAX_DEPTH && "r3d_stack: max push() depth exceeded");

    if (reserve > 0 && stack->cursor + reserve > stack->capacity)
    {
        stack = stack_grow(stack, stack->cursor + reserve);
        *stackPtr = stack;
    }

    stack->marks[stack->depth++] = stack->cursor;
}

void r3d_stack_pop(r3d_stack_t* stack)
{
    R3D_ASSERT(stack->depth > 0 && "r3d_stack: pop() without matching push()");
    if (stack->depth == 0) return;

    stack->cursor = stack->marks[--stack->depth];
}

void* r3d_stack_alloc(r3d_stack_t** stackPtr, size_t size)
{
    return r3d_stack_alloc_aligned(stackPtr, size, R3D_STACK_DEFAULT_ALIGN);
}

void* r3d_stack_alloc_aligned(r3d_stack_t** stackPtr, size_t size, size_t align)
{
    if (size == 0) return NULL;
    if (align == 0) align = 1;

    R3D_ASSERT((align & (align - 1)) == 0 && "r3d_stack: align must be a power of two");

    r3d_stack_t* stack = *stackPtr;

    // Worst-case space needed to satisfy alignment before knowing
    // the final base address (it may change if we have to grow)
    size_t worst = size + (align - 1);

    if (stack->cursor + worst > stack->capacity)
    {
        stack = stack_grow(stack, stack->cursor + worst);
        *stackPtr = stack;
    }

    uintptr_t base = (uintptr_t)stack->memory + stack->cursor;
    uintptr_t aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t padding = (size_t)(aligned - base);

    stack->cursor += padding + size;

    return (void*)aligned;
}

void r3d_stack_reset(r3d_stack_t* stack)
{
    stack->cursor = 0;
    stack->depth = 0;
}
