/* r3d_list.c -- Generic list container (aka vector)
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./r3d_list.h"
#include <raylib.h>
#include <stddef.h>
#include <string.h>

#include "./r3d_helper.h"

// ========================================
// INTERNAL DECLARATIONS
// ========================================

static void reserve(r3d_list_t** list, size_t elemNeeded);
static void push(r3d_list_t* list, const void* elem);
static void push_all(r3d_list_t* list, const void* elems, size_t count);

// ========================================
// LIST FUNCTIONS
// ========================================

r3d_list_t* r3d_list_create(size_t elemSize, size_t capacity)
{
    r3d_list_t* list = r3d_zalloc(sizeof(r3d_list_t) + capacity * elemSize);

    list->elements     = (char*)list + sizeof(r3d_list_t);
    list->elemSize     = elemSize;
    list->elemCount    = 0;
    list->elemCapacity = capacity;

    return list;
}

void r3d_list_destroy(r3d_list_t* list)
{
    r3d_free(list);
}

void r3d_list_reserve(r3d_list_t** list, size_t capacity)
{
    reserve(list, capacity);
}

void r3d_list_resize(r3d_list_t** list, size_t count)
{
    if (count == (*list)->elemCount) return;

    if (count > (*list)->elemCount)
    {
        reserve(list, count);

        void* dst = &((char*)((*list)->elements))[(*list)->elemCount * (*list)->elemSize];
        memset(dst, 0, (count - (*list)->elemCount) * (*list)->elemSize);
    }

    (*list)->elemCount = count;
}

void r3d_list_push(r3d_list_t** list, const void* elem)
{
    reserve(list, (*list)->elemCount + 1);
    push(*list, elem);
}

void r3d_list_push_all(r3d_list_t** list, const void* elems, size_t count)
{
    reserve(list, (*list)->elemCount + count);
    push_all(*list, elems, count);
}

void r3d_list_pop(r3d_list_t* list, void* out)
{
    if (list->elemCount == 0) return;

    list->elemCount -= 1;

    if (out != NULL)
    {
        void* src = &((char*)(list->elements))[list->elemCount * list->elemSize];
        memcpy(out, src, list->elemSize);
    }
}

void r3d_list_unordered_remove(r3d_list_t* list, size_t index)
{
    void* dst = &((char*)(list->elements))[index * list->elemSize];
    void* src = &((char*)(list->elements))[(--list->elemCount) * list->elemSize];

    if (dst != src)
    {
        memcpy(dst, src, list->elemSize);
    }
}

size_t r3d_list_length(r3d_list_t* list)
{
    return list->elemCount;
}

bool r3d_list_empty(r3d_list_t* list)
{
    return list->elemCount == 0;
}

void r3d_list_clear(r3d_list_t* list)
{
    list->elemCount = 0;
}

void* r3d_list_get(r3d_list_t* list, size_t index, size_t typeSize)
{
    R3D_ASSERT(list != NULL);
    R3D_ASSERT(typeSize == list->elemSize && "type size does not match the list's element size");
    R3D_ASSERT(index < list->elemCount && "index out of bounds");

    return &((char*)list->elements)[index * list->elemSize];
}

void r3d_list_set(r3d_list_t* list, size_t index, size_t typeSize, const void* value)
{
    R3D_ASSERT(list != NULL);
    R3D_ASSERT(typeSize == list->elemSize && "type size does not match the list's element size");
    R3D_ASSERT(index < list->elemCount && "index out of bounds");

    void* dst = &((char*)list->elements)[index * list->elemSize];
    memcpy(dst, value, list->elemSize);
}

// ========================================
// INTERNAL FUNCTIONS
// ========================================

void reserve(r3d_list_t** list, size_t elemNeeded)
{
    if (elemNeeded <= (*list)->elemCapacity) return;

    size_t newElemCapacity = R3D_MAX(elemNeeded, (*list)->elemCapacity + (*list)->elemCapacity / 2);
    size_t newSize         = sizeof(r3d_list_t) + newElemCapacity * (*list)->elemSize;

    *list = r3d_realloc(*list, newSize);

    (*list)->elements     = (char*)(*list) + sizeof(r3d_list_t);
    (*list)->elemCapacity = newElemCapacity;
}

void push(r3d_list_t* list, const void* elem)
{
    void* dst = &((char*)list->elements)[list->elemCount * list->elemSize];
    memcpy(dst, elem, list->elemSize);
    list->elemCount++;
}

void push_all(r3d_list_t* list, const void* elems, size_t count)
{
    void* dst = &((char*)list->elements)[list->elemCount * list->elemSize];
    memcpy(dst, elems, count * list->elemSize);
    list->elemCount += count;
}
