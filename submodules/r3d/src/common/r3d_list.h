/* r3d_list.h -- Generic list container (aka vector)
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#ifndef R3D_COMMON_LIST_H
#define R3D_COMMON_LIST_H

#include <raylib.h>
#include <stddef.h>

// ========================================
// LIST STRUCT
// ========================================

typedef struct {
    void* elements;
    size_t elemSize;
    size_t elemCount;
    size_t elemCapacity;
} r3d_list_t;

// ========================================
// LIST ITERATION
// ========================================

#define R3D_LIST_CREATE(type, capacity)         r3d_list_create(sizeof(type), (size_t)(capacity))
#define R3D_LIST_DESTROY(list)                  r3d_list_destroy((list))
#define R3D_LIST_RESERVE(list, capacity)        r3d_list_reserve(&(list), (size_t)(capacity))
#define R3D_LIST_RESIZE(list, count)            r3d_list_resize(&(list), (size_t)(count))
#define R3D_LIST_PUSH(list, elem)               r3d_list_push(&(list), &(elem))
#define R3D_LIST_PUSH_ALL(list, elems, count)   r3d_list_push_all(&(list), (void*)(elems), (size_t)(count))
#define R3D_LIST_POP(list, out)                 r3d_list_pop((list), (out))
#define R3D_LIST_UNORDERED_REMOVE(list, index)  r3d_list_unordered_remove((list), (size_t)(index))
#define R3D_LIST_LENGTH(list)                   r3d_list_length((list))
#define R3D_LIST_EMPTY(list)                    r3d_list_empty((list))
#define R3D_LIST_CLEAR(list)                    r3d_list_clear((list))
#define R3D_LIST_GET(list, type, index)         (*(type*)r3d_list_get((list), (size_t)(index), sizeof(type)))
#define R3D_LIST_SET(list, type, index, value)  (R3D_LIST_GET(list, type, index) = (value))
#define R3D_LIST_GET_INDEX(list, elemPtr)       (((char*)(elemPtr) - (char*)((list)->elements)) / (list)->elemSize)

#define R3D_LIST_FOR_EACH(list, type, elem) \
    for (type* elem = (list)->elements; elem < ((type*)((list)->elements)) + (list)->elemCount; elem++)

// ========================================
// LIST FUNCTIONS
// ========================================

r3d_list_t* r3d_list_create(size_t elemSize, size_t capacity);
void r3d_list_destroy(r3d_list_t* list);
void r3d_list_reserve(r3d_list_t** list, size_t capacity);
void r3d_list_resize(r3d_list_t** list, size_t count);
void r3d_list_push(r3d_list_t** list, const void* elem);
void r3d_list_push_all(r3d_list_t** list, const void* elems, size_t count);
void r3d_list_pop(r3d_list_t* list, void* out);
void r3d_list_unordered_remove(r3d_list_t* list, size_t index);
size_t r3d_list_length(r3d_list_t* list);
bool r3d_list_empty(r3d_list_t* list);
void r3d_list_clear(r3d_list_t* list);
void* r3d_list_get(r3d_list_t* list, size_t index, size_t typeSize);
void r3d_list_set(r3d_list_t* list, size_t index, size_t typeSize, const void* value);

#endif // R3D_COMMON_LIST_H
