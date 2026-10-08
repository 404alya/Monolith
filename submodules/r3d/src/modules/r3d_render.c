/* r3d_render.c -- Internal R3D render module.
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./r3d_render.h"
#include <r3d_config.h>
#include <raymath.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <glad.h>

#include "../common/r3d_helper.h"
#include "../common/r3d_math.h"
#include "../common/r3d_hash.h"
#include "../r3d_core_state.h"

// ========================================
// MODULE CONSTANTS
// ========================================

static const GLenum INSTANCE_FORMAT_GL_TYPE[R3D_INSTANCE_FORMAT_COUNT] = {
    [R3D_INSTANCE_FORMAT_FLOAT32] = GL_FLOAT,
    [R3D_INSTANCE_FORMAT_FLOAT16] = GL_HALF_FLOAT,
    [R3D_INSTANCE_FORMAT_UNORM16] = GL_UNSIGNED_SHORT,
    [R3D_INSTANCE_FORMAT_SNORM16] = GL_SHORT,
    [R3D_INSTANCE_FORMAT_UNORM8]  = GL_UNSIGNED_BYTE,
    [R3D_INSTANCE_FORMAT_SNORM8]  = GL_BYTE,
};

static const GLboolean INSTANCE_FORMAT_NORMALIZED[R3D_INSTANCE_FORMAT_COUNT] = {
    [R3D_INSTANCE_FORMAT_FLOAT32] = GL_FALSE,
    [R3D_INSTANCE_FORMAT_FLOAT16] = GL_FALSE,
    [R3D_INSTANCE_FORMAT_UNORM16] = GL_TRUE,
    [R3D_INSTANCE_FORMAT_SNORM16] = GL_TRUE,
    [R3D_INSTANCE_FORMAT_UNORM8]  = GL_TRUE,
    [R3D_INSTANCE_FORMAT_SNORM8]  = GL_TRUE,
};

static const int INSTANCE_ATTRIBUTE_COMPONENTS[R3D_INSTANCE_ATTRIBUTE_COUNT] = {
    /* POSITION */  3,
    /* ROTATION */  4,
    /* SCALE    */  3,
    /* COLOR    */  4,
    /* CUSTOM   */  4,
};

static const int INSTANCE_FORMAT_SIZE[R3D_INSTANCE_FORMAT_COUNT] = {
    [R3D_INSTANCE_FORMAT_FLOAT32] = 4,
    [R3D_INSTANCE_FORMAT_FLOAT16] = 2,
    [R3D_INSTANCE_FORMAT_UNORM16] = 2,
    [R3D_INSTANCE_FORMAT_SNORM16] = 2,
    [R3D_INSTANCE_FORMAT_UNORM8]  = 1,
    [R3D_INSTANCE_FORMAT_SNORM8]  = 1,
};

// ========================================
// MODULE STATE
// ========================================

struct r3d_mod_render R3D_MOD_RENDER;

// ========================================
// INTERNAL BUFFER RESIZE FUNCTIONS
// ========================================

/*
 * Sets up vertex attribute pointers on the global VAO (already bound).
 * Pass rebindVbo=true after a VBO resize to update the stored buffer ID.
 * Pass configInstances=true only at init to set divisors and default values.
 */
static void vao_configure(bool rebindVbo, bool configInstances)
{
    if (rebindVbo)
    {
        glBindBuffer(GL_ARRAY_BUFFER, R3D_MOD_RENDER.globalVbo);
    }

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(R3D_Vertex), (void*)offsetof(R3D_Vertex, position));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_HALF_FLOAT, GL_FALSE, sizeof(R3D_Vertex), (void*)offsetof(R3D_Vertex, texcoord));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_BYTE, GL_TRUE, sizeof(R3D_Vertex), (void*)offsetof(R3D_Vertex, normal));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_BYTE, GL_TRUE, sizeof(R3D_Vertex), (void*)offsetof(R3D_Vertex, tangent));

    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(R3D_Vertex), (void*)offsetof(R3D_Vertex, color));

    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, sizeof(R3D_Vertex), (void*)offsetof(R3D_Vertex, boneIndices));

    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(R3D_Vertex), (void*)offsetof(R3D_Vertex, boneWeights));

    if (configInstances)
    {
        glVertexAttribDivisor(10, 1);
        glVertexAttribDivisor(11, 1);
        glVertexAttribDivisor(12, 1);
        glVertexAttribDivisor(13, 1);
        glVertexAttribDivisor(14, 1);

        glVertexAttrib3f(10, 0.0f, 0.0f, 0.0f);
        glVertexAttrib4f(11, 0.0f, 0.0f, 0.0f, 1.0f);
        glVertexAttrib3f(12, 1.0f, 1.0f, 1.0f);
        glVertexAttrib4f(13, 1.0f, 1.0f, 1.0f, 1.0f);
        glVertexAttrib4f(14, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

/*
 * Grows the global VBO to at least 'minCapacity' vertices.
 * Creates a new buffer, copies the old content via glCopyBufferSubData,
 * deletes the old buffer, and reconfigures the VAO attrib pointers.
 */
static bool vbo_grow(int minCapacity)
{
    int newCapacity = R3D_MOD_RENDER.globalVertexCapacity * 2;
    while (newCapacity < minCapacity) newCapacity *= 2;

    GLuint newVbo;
    glGenBuffers(1, &newVbo);

    // Allocate the new buffer without data
    glBindBuffer(GL_COPY_WRITE_BUFFER, newVbo);
    glBufferData(GL_COPY_WRITE_BUFFER, newCapacity * sizeof(R3D_Vertex), NULL, GL_DYNAMIC_DRAW);

    // Copy the old content
    glBindBuffer(GL_COPY_READ_BUFFER, R3D_MOD_RENDER.globalVbo);
    glCopyBufferSubData(
        GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
        0, 0,
        R3D_MOD_RENDER.globalVertexCount * sizeof(R3D_Vertex)
    );

    // Replaces the old buffer
    glDeleteBuffers(1, &R3D_MOD_RENDER.globalVbo);
    R3D_MOD_RENDER.globalVbo = newVbo;
    R3D_MOD_RENDER.globalVertexCapacity = newCapacity;

    // Reconfigure the assignments on the new VBO
    glBindVertexArray(R3D_MOD_RENDER.globalVao);
    vao_configure(true, false);
    glBindVertexArray(0);

    return true;
}

/*
 * Grows the global EBO to at least 'minCapacity' indices.
 * Same strategy as vbo_grow: new buffer + copy + delete + rebind.
 */
static bool ebo_grow(int minCapacity)
{
    int newCapacity = R3D_MOD_RENDER.globalElementCapacity * 2;
    while (newCapacity < minCapacity) newCapacity *= 2;

    GLuint newEbo;
    glGenBuffers(1, &newEbo);

    glBindBuffer(GL_COPY_WRITE_BUFFER, newEbo);
    glBufferData(GL_COPY_WRITE_BUFFER, newCapacity * sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_COPY_READ_BUFFER, R3D_MOD_RENDER.globalEbo);
    glCopyBufferSubData(
        GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
        0, 0,
        R3D_MOD_RENDER.globalElementCount * sizeof(GLuint)
    );

    glDeleteBuffers(1, &R3D_MOD_RENDER.globalEbo);
    R3D_MOD_RENDER.globalEbo = newEbo;
    R3D_MOD_RENDER.globalElementCapacity = newCapacity;

    // Rebind the EBO into the global VAO
    glBindVertexArray(R3D_MOD_RENDER.globalVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R3D_MOD_RENDER.globalEbo);
    glBindVertexArray(0);

    return true;
}

/*
 * Copies 'count' vertex slots from 'srcOffset' to 'dstOffset' within
 * the global VBO. Ranges must not overlap.
 */
static void vbo_relocate(int dstOffset, int srcOffset, int count)
{
    GLsizeiptr size = count * sizeof(R3D_Vertex);

    glBindBuffer(GL_COPY_READ_BUFFER, R3D_MOD_RENDER.globalVbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, R3D_MOD_RENDER.globalVbo);
    glCopyBufferSubData(
        GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
        srcOffset * sizeof(R3D_Vertex),
        dstOffset * sizeof(R3D_Vertex),
        size
    );
}

/*
 * Copies 'count' index slots from 'srcOffset' to 'dstOffset' within
 * the global EBO. Ranges must not overlap.
 */
static void ebo_relocate(int dstOffset, int srcOffset, int count)
{
    GLsizeiptr size = count * sizeof(GLuint);

    glBindBuffer(GL_COPY_READ_BUFFER, R3D_MOD_RENDER.globalEbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, R3D_MOD_RENDER.globalEbo);
    glCopyBufferSubData(
        GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
        srcOffset * sizeof(GLuint),
        dstOffset * sizeof(GLuint),
        size
    );
}

// ========================================
// INTERNAL FREE LIST FUNCTIONS
// ========================================

/*
 * After inserting a range into a free list, sort by offset and merge
 * adjacent/overlapping blocks. Keeps the list compact and prevents
 * fragmentation from accumulating across many alloc/free cycles.
 */
static void free_list_coalesce(r3d_list_t* list)
{
    size_t count = R3D_LIST_LENGTH(list);
    if (count < 2) return;

    r3d_render_range_t* data = (r3d_render_range_t*)list->elements;

    // Offset sorting (insertion sort, the list is almost sorted in practice)
    for (size_t i = 1; i < count; i++)
    {
        r3d_render_range_t key = data[i];
        size_t j = i;
        while (j > 0 && data[j - 1].offset > key.offset)
        {
            data[j] = data[j - 1];
            j--;
        }
        data[j] = key;
    }

    // Merging of adjacent blocks
    size_t write = 0;
    for (size_t i = 1; i < count; i++)
    {
        if (data[write].offset + data[write].count >= data[i].offset)
        {
            // Contiguous or overlapping blocks: we extend
            int end = data[write].offset + data[write].count;
            int end_i = data[i].offset + data[i].count;
            if (end_i > end) data[write].count = end_i - data[write].offset;
        }
        else
        {
            data[++write] = data[i];
        }
    }
    list->elemCount = write + 1;
}

/*
 * Pushes a range onto a free list; the list grows automatically if needed.
 */
static void free_list_push_range(r3d_list_t** list, int offset, int rangeCount)
{
    r3d_render_range_t range = { .offset = offset, .count = rangeCount };
    R3D_LIST_PUSH(*list, range);
}

/*
 * First-fit search in a free list.
 * If a block large enough is found:
 *   - it is split if strictly larger than needed (remainder stays in the list)
 *   - it is removed entirely if it matches exactly
 * Returns the offset on success, -1 if nothing fits.
 */
static int free_list_pop_range(r3d_list_t* list, int needed)
{
    size_t count = R3D_LIST_LENGTH(list);

    for (size_t i = 0; i < count; i++)
    {
        r3d_render_range_t* range = &R3D_LIST_GET(list, r3d_render_range_t, i);

        if (range->count >= needed)
        {
            int offset = range->offset;
            if (range->count > needed)
            {
                // Cut: we keep the rest in the list
                range->offset += needed;
                range->count  -= needed;
            }
            else
            {
                // Exact match: remove the entry
                R3D_LIST_UNORDERED_REMOVE(list, i);
            }
            return offset;
        }
    }
    return -1;
}

/*
 * Searches the free list for a block starting exactly at 'afterOffset'
 * with at least 'needed' slots. If found, consumes 'needed' slots from it
 * (splitting the remainder back) and returns true.
 */
static bool free_list_try_extend_in_place(r3d_list_t* list, int afterOffset, int needed)
{
    size_t count = R3D_LIST_LENGTH(list);

    for (size_t i = 0; i < count; i++)
    {
        r3d_render_range_t* range = &R3D_LIST_GET(list, r3d_render_range_t, i);

        if (range->offset != afterOffset) continue;

        if (range->count < needed)
        {
            // Adjacent block but too small
            return false;
        }

        if (range->count == needed)
        {
            // Exact match: remove the entry
            R3D_LIST_UNORDERED_REMOVE(list, i);
        }
        else
        {
            // We consume 'needed' since the beginning of the block
            range->offset += needed;
            range->count  -= needed;
        }

        return true;
    }

    return false;
}

// ========================================
// INTERNAL SHAPE FUNCTIONS
// ========================================

typedef void (*shape_loader_func)(r3d_render_shape_t*);

static void shape_load_dummy(r3d_render_shape_t* dummy);
static void shape_load_quad(r3d_render_shape_t* quad);
static void shape_load_cube(r3d_render_shape_t* cube);

static const shape_loader_func SHAPE_LOADERS[] = {
    [R3D_RENDER_SHAPE_DUMMY] = shape_load_dummy,
    [R3D_RENDER_SHAPE_QUAD] = shape_load_quad,
    [R3D_RENDER_SHAPE_CUBE] = shape_load_cube,
};

void shape_load_dummy(r3d_render_shape_t* shape)
{
    shape->vertices.count = 3;
    shape->elements.count = 0;
}

void shape_load_quad(r3d_render_shape_t* shape)
{
    const R3D_Vertex vertices[] = {
        R3D_MakeVertex((Vector3){-0.5f,  0.5f, 0}, (Vector2){0, 1}, (Vector3){0, 0, 1}, (Vector4){1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f, -0.5f, 0}, (Vector2){0, 0}, (Vector3){0, 0, 1}, (Vector4){1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f,  0.5f, 0}, (Vector2){1, 1}, (Vector3){0, 0, 1}, (Vector4){1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f, -0.5f, 0}, (Vector2){1, 0}, (Vector3){0, 0, 1}, (Vector4){1, 0, 0, 1}, WHITE),
    };

    static const uint32_t INDICES[] = {
        0, 1, 2,
        1, 3, 2
    };

    r3d_render_alloc_vertices(R3D_ARRAY_SIZE(vertices), &shape->vertices.offset);
    r3d_render_alloc_elements(R3D_ARRAY_SIZE(INDICES), &shape->elements.offset);

    r3d_render_upload_vertices(shape->vertices.offset, vertices, R3D_ARRAY_SIZE(vertices));
    r3d_render_upload_elements(shape->elements.offset, INDICES, R3D_ARRAY_SIZE(INDICES));

    shape->vertices.count = R3D_ARRAY_SIZE(vertices);
    shape->elements.count = R3D_ARRAY_SIZE(INDICES);
}

void shape_load_cube(r3d_render_shape_t* shape)
{
    const R3D_Vertex vertices[] = {
        // Front (Z+)
        R3D_MakeVertex((Vector3){-0.5f,  0.5f,  0.5f}, (Vector2){0, 1}, (Vector3){ 0, 0, 1}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f, -0.5f,  0.5f}, (Vector2){0, 0}, (Vector3){ 0, 0, 1}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f,  0.5f,  0.5f}, (Vector2){1, 1}, (Vector3){ 0, 0, 1}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f, -0.5f,  0.5f}, (Vector2){1, 0}, (Vector3){ 0, 0, 1}, (Vector4){ 1, 0, 0, 1}, WHITE),
        // Back (Z-)
        R3D_MakeVertex((Vector3){-0.5f,  0.5f, -0.5f}, (Vector2){1, 1}, (Vector3){ 0, 0,-1}, (Vector4){-1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f, -0.5f, -0.5f}, (Vector2){1, 0}, (Vector3){ 0, 0,-1}, (Vector4){-1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f,  0.5f, -0.5f}, (Vector2){0, 1}, (Vector3){ 0, 0,-1}, (Vector4){-1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f, -0.5f, -0.5f}, (Vector2){0, 0}, (Vector3){ 0, 0,-1}, (Vector4){-1, 0, 0, 1}, WHITE),
        // Left (X-)
        R3D_MakeVertex((Vector3){-0.5f,  0.5f, -0.5f}, (Vector2){0, 1}, (Vector3){-1, 0, 0}, (Vector4){ 0, 0,-1, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f, -0.5f, -0.5f}, (Vector2){0, 0}, (Vector3){-1, 0, 0}, (Vector4){ 0, 0,-1, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f,  0.5f,  0.5f}, (Vector2){1, 1}, (Vector3){-1, 0, 0}, (Vector4){ 0, 0,-1, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f, -0.5f,  0.5f}, (Vector2){1, 0}, (Vector3){-1, 0, 0}, (Vector4){ 0, 0,-1, 1}, WHITE),
        // Right (X+)
        R3D_MakeVertex((Vector3){ 0.5f,  0.5f,  0.5f}, (Vector2){0, 1}, (Vector3){ 1, 0, 0}, (Vector4){ 0, 0, 1, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f, -0.5f,  0.5f}, (Vector2){0, 0}, (Vector3){ 1, 0, 0}, (Vector4){ 0, 0, 1, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f,  0.5f, -0.5f}, (Vector2){1, 1}, (Vector3){ 1, 0, 0}, (Vector4){ 0, 0, 1, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f, -0.5f, -0.5f}, (Vector2){1, 0}, (Vector3){ 1, 0, 0}, (Vector4){ 0, 0, 1, 1}, WHITE),
        // Top (Y+)
        R3D_MakeVertex((Vector3){-0.5f,  0.5f, -0.5f}, (Vector2){0, 0}, (Vector3){ 0, 1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f,  0.5f,  0.5f}, (Vector2){0, 1}, (Vector3){ 0, 1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f,  0.5f, -0.5f}, (Vector2){1, 0}, (Vector3){ 0, 1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f,  0.5f,  0.5f}, (Vector2){1, 1}, (Vector3){ 0, 1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
        // Bottom (Y-)
        R3D_MakeVertex((Vector3){-0.5f, -0.5f,  0.5f}, (Vector2){0, 0}, (Vector3){ 0,-1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){-0.5f, -0.5f, -0.5f}, (Vector2){0, 1}, (Vector3){ 0,-1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f, -0.5f,  0.5f}, (Vector2){1, 0}, (Vector3){ 0,-1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
        R3D_MakeVertex((Vector3){ 0.5f, -0.5f, -0.5f}, (Vector2){1, 1}, (Vector3){ 0,-1, 0}, (Vector4){ 1, 0, 0, 1}, WHITE),
    };

    static const uint32_t INDICES[] = {
        0,1,2, 2,1,3,
        6,5,4, 7,5,6,
        8,9,10, 10,9,11,
        12,13,14, 14,13,15,
        16,17,18, 18,17,19,
        20,21,22, 22,21,23
    };

    r3d_render_alloc_vertices(R3D_ARRAY_SIZE(vertices), &shape->vertices.offset);
    r3d_render_alloc_elements(R3D_ARRAY_SIZE(INDICES), &shape->elements.offset);

    r3d_render_upload_vertices(shape->vertices.offset, vertices, R3D_ARRAY_SIZE(vertices));
    r3d_render_upload_elements(shape->elements.offset, INDICES, R3D_ARRAY_SIZE(INDICES));

    shape->vertices.count = R3D_ARRAY_SIZE(vertices);
    shape->elements.count = R3D_ARRAY_SIZE(INDICES);
}

// ========================================
// INTERNAL INSTANCES FUNCTIONS
// ========================================

static void instances_enable(const GLuint buffers[R3D_INSTANCE_ATTRIBUTE_COUNT], R3D_InstanceLayout layout, int offset)
{
    r3d_render_instance_state_t* state = &R3D_MOD_RENDER.instanceState;

    if (offset == state->offset &&
        memcmp(buffers, state->buffers, R3D_INSTANCE_ATTRIBUTE_COUNT * sizeof(GLuint)) == 0 &&
        memcmp(&layout, &state->layout, sizeof(R3D_InstanceLayout)) == 0)
    {
        return;
    }

    memcpy(state->buffers, buffers, R3D_INSTANCE_ATTRIBUTE_COUNT * sizeof(GLuint));
    state->layout = layout;
    state->offset = offset;

    for (int i = 0; i < R3D_INSTANCE_ATTRIBUTE_COUNT; i++)
    {
        GLuint attrib = 10 + i;
        R3D_InstanceFlags flag = 1u << i;

        if (!R3D_BIT_ANY(layout.flags, flag))
        {
            glDisableVertexAttribArray(attrib);
            continue;
        }

        R3D_InstanceFormat format = layout.formats[i];

        if (format < 0 || format >= R3D_INSTANCE_FORMAT_COUNT)
        {
            glDisableVertexAttribArray(attrib);
            R3D_TRACELOG(LOG_WARNING, "instances_enable -> invalid instance format (attribute=%d, format=%d)", i, format);
            continue;
        }

        int components = INSTANCE_ATTRIBUTE_COMPONENTS[i];
        int stride = components * INSTANCE_FORMAT_SIZE[format];

        glBindBuffer(GL_ARRAY_BUFFER, buffers[i]);
        glEnableVertexAttribArray(attrib);
        glVertexAttribPointer(
            attrib,
            components,
            INSTANCE_FORMAT_GL_TYPE[format],
            INSTANCE_FORMAT_NORMALIZED[format],
            stride,
            (void*)((size_t)offset * stride)
        );
    }
}

/*
static void instances_disable(R3D_InstanceFlags flags)
{
    memset(&R3D_MOD_RENDER.instanceState, 0, sizeof(R3D_MOD_RENDER.instanceState));

    if (R3D_BIT_ANY(flags, R3D_INSTANCE_POSITION))
    {
        glDisableVertexAttribArray(10);
    }

    if (R3D_BIT_ANY(flags, R3D_INSTANCE_ROTATION))
    {
        glDisableVertexAttribArray(11);
    }

    if (R3D_BIT_ANY(flags, R3D_INSTANCE_SCALE))
    {
        glDisableVertexAttribArray(12);
    }

    if (R3D_BIT_ANY(flags, R3D_INSTANCE_COLOR))
    {
        glDisableVertexAttribArray(13);
    }

    if (R3D_BIT_ANY(flags, R3D_INSTANCE_CUSTOM))
    {
        glDisableVertexAttribArray(14);
    }
}
*/

// ========================================
// INTERNAL ARRAY FUNCTIONS
// ========================================

static inline int array_get_call_index(const r3d_render_call_t* call)
{
    r3d_render_call_t* base = (r3d_render_call_t*)R3D_MOD_RENDER.calls->elements;
    R3D_ASSERT(call >= base);
    return (int)(call - base);
}

static inline int array_get_last_group_index(void)
{
    int groupIndex = (int)R3D_LIST_LENGTH(R3D_MOD_RENDER.groups) - 1;
    R3D_ASSERT(groupIndex >= 0);
    return groupIndex;
}

//static inline r3d_render_group_t* array_get_last_group(void)
//{
//    int groupIndex = array_get_last_group_index();
//    return &R3D_LIST_GET(R3D_MOD_RENDER.groups, r3d_render_group_t, groupIndex);
//}

// ========================================
// INTERNAL BINDING FUNCTIONS
// ========================================

static inline GLenum get_opengl_primitive(R3D_PrimitiveType primitive)
{
    switch (primitive)
    {
    case R3D_PRIMITIVE_POINTS:          return GL_POINTS;
    case R3D_PRIMITIVE_LINES:           return GL_LINES;
    case R3D_PRIMITIVE_LINE_STRIP:      return GL_LINE_STRIP;
    case R3D_PRIMITIVE_LINE_LOOP:       return GL_LINE_LOOP;
    case R3D_PRIMITIVE_TRIANGLES:       return GL_TRIANGLES;
    case R3D_PRIMITIVE_TRIANGLE_STRIP:  return GL_TRIANGLE_STRIP;
    case R3D_PRIMITIVE_TRIANGLE_FAN:    return GL_TRIANGLE_FAN;
    default: break;
    }

    return GL_TRIANGLES; // consider an error...
}

static void get_draw_call_info(const r3d_render_call_t* call, GLenum* primitive, r3d_render_range_t* vertexRange, r3d_render_range_t* indexRange)
{
    R3D_ASSERT(primitive && vertexRange && indexRange);

    *primitive = GL_NONE;
    *vertexRange = (r3d_render_range_t) {0};
    *indexRange = (r3d_render_range_t) {0};

    switch (call->type)
    {
    case R3D_RENDER_CALL_MESH:
        {
            const R3D_Mesh* mesh = &call->mesh.instance;

            *primitive = get_opengl_primitive(mesh->primitiveType);
            vertexRange->offset = mesh->vertexOffset;
            vertexRange->count = mesh->vertexCount;
            indexRange->offset = mesh->indexOffset;
            indexRange->count = mesh->indexCount;
        }
        break;
    case R3D_RENDER_CALL_DECAL:
        {
            r3d_render_shape_t* shape = &R3D_MOD_RENDER.shapes[R3D_RENDER_SHAPE_CUBE];
            if (shape->vertices.count == 0)
            {
                SHAPE_LOADERS[R3D_RENDER_SHAPE_CUBE](shape);
                glBindVertexArray(R3D_MOD_RENDER.globalVao);
            }

            *primitive = GL_TRIANGLES;
            vertexRange->offset = shape->vertices.offset;
            vertexRange->count = shape->vertices.count;
            indexRange->offset = shape->elements.offset;
            indexRange->count = shape->elements.count;
        }
        break;
    default:
        R3D_ASSERT(false);
        break;
    }
}

// ========================================
// INTERNAL CULLING FUNCTIONS
// ========================================

static inline bool is_aabb_visible(const R3D_Frustum* frustum, BoundingBox aabb)
{
    if (memcmp(&aabb, &(BoundingBox){0}, sizeof(BoundingBox)) == 0)
    {
        return true;
    }

    return R3D_FrustumIntersectsBoundingBox(frustum, aabb);
}

static inline bool is_obb_visible(const R3D_Frustum* frustum, R3D_OrientedBox obb)
{
    if (memcmp(&obb, &(R3D_OrientedBox){0}, sizeof(R3D_OrientedBox)) == 0)
    {
        return true;
    }

    return R3D_FrustumIntersectsOrientedBox(frustum, obb);
}

static inline bool is_transformed_aabb_visible(const R3D_Frustum* frustum, BoundingBox aabb, Matrix transform)
{
    if (memcmp(&aabb, &(BoundingBox){0}, sizeof(BoundingBox)) == 0)
    {
        return true;
    }

    if (r3d_matrix_is_identity(transform))
    {
        return R3D_FrustumIntersectsBoundingBox(frustum, aabb);
    }

    return R3D_FrustumIntersectsOrientedBox(frustum, R3D_GetOrientedBox(aabb, transform));
}

static inline bool is_draw_call_visible(const R3D_Frustum* frustum, const r3d_render_call_t* call, Matrix transform)
{
    switch (call->type) {
    case R3D_RENDER_CALL_MESH:
        return is_transformed_aabb_visible(frustum, call->mesh.instance.aabb, transform);
    case R3D_RENDER_CALL_DECAL:
        return is_transformed_aabb_visible(frustum, (BoundingBox) {
            .min.x = -0.5f, .min.y = -0.5f, .min.z = -0.5f,
            .max.x = +0.5f, .max.y = +0.5f, .max.z = +0.5f
        }, transform);
    default:
        R3D_ASSERT(false);
        break;
    }

    return false;
}

// ========================================
// INTERNAL SORTING FUNCTIONS
// ========================================

static Vector3 G_sortViewPosition = {0};

static inline float calculate_center_distance_to_camera(const BoundingBox* aabb, const Matrix* transform)
{
    Vector3 center = {
        (aabb->min.x + aabb->max.x) * 0.5f,
        (aabb->min.y + aabb->max.y) * 0.5f,
        (aabb->min.z + aabb->max.z) * 0.5f
    };
    center = Vector3Transform(center, *transform);

    return Vector3DistanceSqr(G_sortViewPosition, center);
}

static inline float calculate_max_distance_to_camera(const BoundingBox* aabb, const Matrix* transform)
{
    float maxDistSq = 0.0f;

    for (int i = 0; i < 8; ++i)
    {
        Vector3 corner = {
            (i & 1) ? aabb->max.x : aabb->min.x,
            (i & 2) ? aabb->max.y : aabb->min.y,
            (i & 4) ? aabb->max.z : aabb->min.z
        };

        corner = Vector3Transform(corner, *transform);
        float distSq = Vector3DistanceSqr(G_sortViewPosition, corner);
        maxDistSq = (distSq > maxDistSq) ? distSq : maxDistSq;
    }

    return maxDistSq;
}

static inline void sort_fill_state_data(r3d_render_sort_state_t* state, const r3d_render_call_t* call)
{
    memset(state, 0, sizeof(*state));

    switch (call->type)
    {
    case R3D_RENDER_CALL_MESH:
        state->priority     = call->mesh.material.priority;
        state->shader       = (uintptr_t)call->mesh.material.shader;
        state->shading      = call->mesh.material.unlit;
        state->albedo       = call->mesh.material.albedo.texture.id;
        state->normal       = call->mesh.material.normal.texture.id;
        state->orm          = call->mesh.material.orm.texture.id;
        state->emission     = call->mesh.material.emission.texture.id;
        state->stencil      = r3d_hash_fnv1a_32(&call->mesh.material.stencil, sizeof(call->mesh.material.stencil));
        state->depth        = r3d_hash_fnv1a_32(&call->mesh.material.depth, sizeof(call->mesh.material.depth));
        state->blend        = call->mesh.material.blendMode;
        state->cull         = call->mesh.material.cullMode;
        state->transparency = call->mesh.material.transparencyMode;
        state->billboard    = call->mesh.material.billboardMode;
        break;

    case R3D_RENDER_CALL_DECAL:
        state->shader   = (uintptr_t)call->decal.instance.shader;
        state->albedo   = call->decal.instance.albedo.texture.id;
        state->normal   = call->decal.instance.normal.texture.id;
        state->orm      = call->decal.instance.orm.texture.id;
        state->emission = call->decal.instance.emission.texture.id;
        break;
    }
}

static void sort_fill_cache_front_to_back(r3d_render_list_enum_t list)
{
    R3D_ASSERT(list < R3D_RENDER_LIST_OPAQUE_INST && "Instantiated render lists should not be sorted by distance");
    R3D_ASSERT(list != R3D_RENDER_LIST_DECAL && "Decal render list should not be sorted by distance");

    r3d_list_t* drawList = R3D_MOD_RENDER.list[list];
    size_t count = R3D_LIST_LENGTH(drawList);

    for (size_t i = 0; i < count; i++)
    {
        int callIndex = R3D_LIST_GET(drawList, int, i);
        const r3d_render_call_t* call = &R3D_LIST_GET(R3D_MOD_RENDER.calls, r3d_render_call_t, callIndex);
        const r3d_render_group_t* group = r3d_render_get_call_group(call);
        r3d_render_sort_t* sortData = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, callIndex);

        sortData->distance = calculate_center_distance_to_camera(
            &call->mesh.instance.aabb, &group->transform
        );

        sort_fill_state_data(&sortData->state, call);
    }
}

static void sort_fill_cache_back_to_front(r3d_render_list_enum_t list)
{
    R3D_ASSERT(list < R3D_RENDER_LIST_OPAQUE_INST && "Instantiated render lists should not be sorted by distance");
    R3D_ASSERT(list != R3D_RENDER_LIST_DECAL && "Decal render list should not be sorted by distance");

    r3d_list_t* drawList = R3D_MOD_RENDER.list[list];
    size_t count = R3D_LIST_LENGTH(drawList);

    for (size_t i = 0; i < count; i++)
    {
        int callIndex = R3D_LIST_GET(drawList, int, i);
        const r3d_render_call_t* call = &R3D_LIST_GET(R3D_MOD_RENDER.calls, r3d_render_call_t, callIndex);
        const r3d_render_group_t* group = r3d_render_get_call_group(call);
        r3d_render_sort_t* sortData = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, callIndex);

        sortData->distance = calculate_max_distance_to_camera(
            &call->mesh.instance.aabb, &group->transform
        );

        // For back-to-front sorting we just need the priority for the meshes
        memset(&sortData->state, 0, sizeof(sortData->state));
        if (call->type == R3D_RENDER_CALL_MESH)
        {
            sortData->state.priority = call->mesh.material.priority;
        }
    }
}

static void sort_fill_cache_by_material(r3d_render_list_enum_t list)
{
    r3d_list_t* drawList = R3D_MOD_RENDER.list[list];
    size_t count = R3D_LIST_LENGTH(drawList);

    for (size_t i = 0; i < count; i++)
    {
        int callIndex = R3D_LIST_GET(drawList, int, i);
        const r3d_render_call_t* call = &R3D_LIST_GET(R3D_MOD_RENDER.calls, r3d_render_call_t, callIndex);
        r3d_render_sort_t* sortData = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, callIndex);

        sortData->distance = 0.0f;
        sort_fill_state_data(&sortData->state, call);
    }
}

static inline int compare_i32(int32_t a, int32_t b)
{
    return (a > b) - (a < b);
}

static inline int compare_f32(float a, float b)
{
    return (a > b) - (a < b);
}

static inline int compare_material(const r3d_render_sort_t* a, const r3d_render_sort_t* b)
{
    // User priority first (signed)
    if (a->state.priority != b->state.priority)
    {
        return compare_i32(a->state.priority, b->state.priority);
    }

    // Remaining fields via memcmp (must be all unsigned, zero-padded)
    size_t n = sizeof(a->state) - offsetof(r3d_render_sort_state_t, shader);
    return memcmp(&a->state.shader, &b->state.shader, n);
}

static int compare_front_to_back(const void* a, const void* b)
{
    const r3d_render_sort_t* aEntry = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, *(const int*)(a));
    const r3d_render_sort_t* bEntry = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, *(const int*)(b));

    int cmp = compare_material(aEntry, bEntry);
    if (cmp != 0) return cmp;

    return compare_f32(aEntry->distance, bEntry->distance);
}

static int compare_back_to_front(const void* a, const void* b)
{
    const r3d_render_sort_t* aEntry = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, *(const int*)(a));
    const r3d_render_sort_t* bEntry = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, *(const int*)(b));

    int cmp = compare_i32(aEntry->state.priority, bEntry->state.priority);
    if (cmp != 0) return cmp;

    return compare_f32(bEntry->distance, aEntry->distance);
}

static int compare_materials_only(const void* a, const void* b)
{
    const r3d_render_sort_t* aEntry = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, *(const int*)(a));
    const r3d_render_sort_t* bEntry = &R3D_LIST_GET(R3D_MOD_RENDER.sortCache, r3d_render_sort_t, *(const int*)(b));

    return compare_material(aEntry, bEntry);
}

// ========================================
// MODULE FUNCTIONS
// ========================================

bool r3d_render_init(void)
{
    memset(&R3D_MOD_RENDER, 0, sizeof(R3D_MOD_RENDER));

    /* --- CPU array allocation (draw calls, groups, etc) --- */

    int cap = R3D_HINT(R3D_HINT_DRAW_CALL_CAPACITY);

    R3D_MOD_RENDER.clusters        = R3D_LIST_CREATE(r3d_render_cluster_t, cap);
    R3D_MOD_RENDER.groupVisibility = R3D_LIST_CREATE(r3d_render_group_visibility_t, cap);
    R3D_MOD_RENDER.callIndices     = R3D_LIST_CREATE(r3d_render_indices_t, cap);
    R3D_MOD_RENDER.groups          = R3D_LIST_CREATE(r3d_render_group_t, cap);

    for (int i = 0; i < R3D_RENDER_LIST_COUNT; i++)
    {
        R3D_MOD_RENDER.list[i] = R3D_LIST_CREATE(int, cap);
    }

    R3D_MOD_RENDER.calls        = R3D_LIST_CREATE(r3d_render_call_t, cap);
    R3D_MOD_RENDER.groupIndices = R3D_LIST_CREATE(int, cap);
    R3D_MOD_RENDER.sortCache    = R3D_LIST_CREATE(r3d_render_sort_t, cap);

    R3D_MOD_RENDER.activeCluster = -1;

    /* --- CPU free list allocation --- */

    R3D_MOD_RENDER.freeVertices = R3D_LIST_CREATE(r3d_render_range_t, R3D_HINT(R3D_HINT_MESH_STREAMING_CAPACITY));
    R3D_MOD_RENDER.freeElements = R3D_LIST_CREATE(r3d_render_range_t, R3D_HINT(R3D_HINT_MESH_STREAMING_CAPACITY));

    /* --- Creation of the global VAO/VBO/EBO --- */

    glGenVertexArrays(1, &R3D_MOD_RENDER.globalVao);
    glBindVertexArray(R3D_MOD_RENDER.globalVao);

    glGenBuffers(1, &R3D_MOD_RENDER.globalVbo);
    glBindBuffer(GL_ARRAY_BUFFER, R3D_MOD_RENDER.globalVbo);
    glBufferData(GL_ARRAY_BUFFER,
        R3D_HINT(R3D_HINT_MESH_VERTEX_BUFFER_CAPACITY) * sizeof(R3D_Vertex),
        NULL, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &R3D_MOD_RENDER.globalEbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R3D_MOD_RENDER.globalEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        R3D_HINT(R3D_HINT_MESH_INDEX_BUFFER_CAPACITY) * sizeof(GLuint),
        NULL, GL_DYNAMIC_DRAW);

    R3D_MOD_RENDER.globalVertexCapacity  = R3D_HINT(R3D_HINT_MESH_VERTEX_BUFFER_CAPACITY);
    R3D_MOD_RENDER.globalElementCapacity = R3D_HINT(R3D_HINT_MESH_INDEX_BUFFER_CAPACITY);

    /* --- Configuring vertex attributes --- */

    vao_configure(false, true);
    glBindVertexArray(0);

    return true;
}

void r3d_render_quit(void)
{
    /* --- Delete global GL buffers --- */

    if (R3D_MOD_RENDER.globalVao) glDeleteVertexArrays(1, &R3D_MOD_RENDER.globalVao);
    if (R3D_MOD_RENDER.globalVbo) glDeleteBuffers(1, &R3D_MOD_RENDER.globalVbo);
    if (R3D_MOD_RENDER.globalEbo) glDeleteBuffers(1, &R3D_MOD_RENDER.globalEbo);

    /* --- Release CPU arrays --- */

    for (int i = 0; i < R3D_RENDER_LIST_COUNT; i++)
    {
        R3D_LIST_DESTROY(R3D_MOD_RENDER.list[i]);
    }

    R3D_LIST_DESTROY(R3D_MOD_RENDER.groupVisibility);
    R3D_LIST_DESTROY(R3D_MOD_RENDER.groupIndices);
    R3D_LIST_DESTROY(R3D_MOD_RENDER.callIndices);
    R3D_LIST_DESTROY(R3D_MOD_RENDER.sortCache);
    R3D_LIST_DESTROY(R3D_MOD_RENDER.clusters);
    R3D_LIST_DESTROY(R3D_MOD_RENDER.groups);
    R3D_LIST_DESTROY(R3D_MOD_RENDER.calls);

    /* --- Realease free lists --- */

    R3D_LIST_DESTROY(R3D_MOD_RENDER.freeVertices);
    R3D_LIST_DESTROY(R3D_MOD_RENDER.freeElements);
}

bool r3d_render_alloc_vertices(int count, int* outOffset)
{
    R3D_ASSERT(outOffset != NULL);
    R3D_ASSERT(count > 0);

    // First search the free list
    int offset = free_list_pop_range(R3D_MOD_RENDER.freeVertices, count);

    if (offset >= 0)
    {
        *outOffset = offset;
        return true;
    }

    // No free block, we extend from the end of the buffer
    int needed = R3D_MOD_RENDER.globalVertexCount + count;
    if (needed > R3D_MOD_RENDER.globalVertexCapacity)
    {
        if (!vbo_grow(needed))
        {
            R3D_TRACELOG(LOG_FATAL, "r3d_render_alloc_vertices: VBO resize failed");
            return false;
        }
    }

    *outOffset = R3D_MOD_RENDER.globalVertexCount;
    R3D_MOD_RENDER.globalVertexCount += count;
    return true;
}

bool r3d_render_alloc_elements(int count, int* outOffset)
{
    R3D_ASSERT(outOffset != NULL);
    R3D_ASSERT(count > 0);

    int offset = free_list_pop_range(R3D_MOD_RENDER.freeElements, count);

    if (offset >= 0)
    {
        *outOffset = offset;
        return true;
    }

    int needed = R3D_MOD_RENDER.globalElementCount + count;
    if (needed > R3D_MOD_RENDER.globalElementCapacity)
    {
        if (!ebo_grow(needed))
        {
            R3D_TRACELOG(LOG_FATAL, "r3d_render_alloc_elements: EBO resize failed");
            return false;
        }
    }

    *outOffset = R3D_MOD_RENDER.globalElementCount;
    R3D_MOD_RENDER.globalElementCount += count;
    return true;
}

bool r3d_render_realloc_vertices(int* offset, int* count, int newCount, bool keepData)
{
    R3D_ASSERT(offset != NULL && count != NULL);
    R3D_ASSERT(*offset >= 0 && *count >= 0 && newCount > 0);

    if (newCount == *count)
    {
        return true;
    }

    // Reduction
    // The queue is released: the (count - newCount) end slots
    // are returned to the free list. No GPU copy is necessary
    if (newCount < *count)
    {
        r3d_render_free_vertices(*offset + newCount, *count - newCount);
        *count = newCount;
        return true;
    }

    // Enlargement
    int extra = newCount - *count;

    // Case 1: Extension in place if the free list
    // has a contiguous block immediately after
    if (free_list_try_extend_in_place(R3D_MOD_RENDER.freeVertices, *offset + *count, extra))
    {
        *count = newCount;
        return true;
    }

    // Case 2: Extension in place if we are at the end of
    // the buffer and there is still uncommitted capacity
    if (*offset + *count == R3D_MOD_RENDER.globalVertexCount)
    {
        int needed = R3D_MOD_RENDER.globalVertexCount + extra;
        if (needed > R3D_MOD_RENDER.globalVertexCapacity)
        {
            if (!vbo_grow(needed))
            {
                R3D_TRACELOG(LOG_FATAL, "r3d_render_realloc_vertices: VBO resize failed");
                return false;
            }
        }
        R3D_MOD_RENDER.globalVertexCount += extra;
        *count = newCount;
        return true;
    }

    // Case 3: no contiguous space, we look for a new larger block,
    // copy the existing data into it, then free the old one
    int newOffset;
    if (!r3d_render_alloc_vertices(newCount, &newOffset))
    {
        return false;
    }

    if (keepData)
    {
        vbo_relocate(newOffset, *offset, *count);
    }

    r3d_render_free_vertices(*offset, *count);

    *offset = newOffset;
    *count  = newCount;
    return true;
}

bool r3d_render_realloc_elements(int* offset, int* count, int newCount, bool keepData)
{
    R3D_ASSERT(offset != NULL && count != NULL);
    R3D_ASSERT(*offset >= 0 && *count >= 0 && newCount > 0);

    if (newCount == *count)
    {
        return true;
    }

    // Reduction
    // The queue is released: the (count - newCount) end slots
    // are returned to the free list. No GPU copy is necessary
    if (newCount < *count)
    {
        r3d_render_free_elements(*offset + newCount, *count - newCount);
        *count = newCount;
        return true;
    }

    // Enlargement
    int extra = newCount - *count;

    // Case 1: Extension in place if the free list
    // has a contiguous block immediately after
    if (free_list_try_extend_in_place(R3D_MOD_RENDER.freeElements, *offset + *count, extra))
    {
        *count = newCount;
        return true;
    }

    // Case 2: Extension in place if we are at the end of
    // the buffer and there is still uncommitted capacity
    if (*offset + *count == R3D_MOD_RENDER.globalElementCount)
    {
        int needed = R3D_MOD_RENDER.globalElementCount + extra;
        if (needed > R3D_MOD_RENDER.globalElementCapacity)
        {
            if (!ebo_grow(needed))
            {
                R3D_TRACELOG(LOG_FATAL, "r3d_render_realloc_elements: EBO resize failed");
                return false;
            }
        }
        R3D_MOD_RENDER.globalElementCount += extra;
        *count = newCount;
        return true;
    }

    // Case 3: no contiguous space, we look for a new larger block,
    // copy the existing data into it, then free the old one
    int newOffset;
    if (!r3d_render_alloc_elements(newCount, &newOffset))
    {
        return false;
    }

    if (keepData)
    {
        ebo_relocate(newOffset, *offset, *count);
    }

    r3d_render_free_elements(*offset, *count);

    *offset = newOffset;
    *count  = newCount;
    return true;
}

void r3d_render_free_vertices(int offset, int count)
{
    R3D_ASSERT(offset >= 0 && count > 0);

    free_list_push_range(&R3D_MOD_RENDER.freeVertices, offset, count);
    free_list_coalesce(R3D_MOD_RENDER.freeVertices);
}

void r3d_render_free_elements(int offset, int count)
{
    R3D_ASSERT(offset >= 0 && count > 0);

    free_list_push_range(&R3D_MOD_RENDER.freeElements, offset, count);
    free_list_coalesce(R3D_MOD_RENDER.freeElements);
}

void r3d_render_upload_vertices(int offset, const R3D_Vertex* verts, int count)
{
    R3D_ASSERT(offset >= 0 && verts != NULL && count > 0);
    R3D_ASSERT(offset + count <= R3D_MOD_RENDER.globalVertexCapacity);

    glBindBuffer(GL_ARRAY_BUFFER, R3D_MOD_RENDER.globalVbo);
    glBufferSubData(
        GL_ARRAY_BUFFER,
        offset * sizeof(R3D_Vertex),
        count * sizeof(R3D_Vertex),
        verts
    );
}

void r3d_render_upload_elements(int offset, const GLuint* indices, int count)
{
    R3D_ASSERT(offset >= 0 && indices != NULL && count > 0);
    R3D_ASSERT(offset + count <= R3D_MOD_RENDER.globalElementCapacity);

    glBindVertexArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R3D_MOD_RENDER.globalEbo);
    glBufferSubData(
        GL_ELEMENT_ARRAY_BUFFER,
        offset * sizeof(GLuint),
        count * sizeof(GLuint),
        indices
    );
}

void r3d_render_clear(void)
{
    for (int i = 0; i < R3D_RENDER_LIST_COUNT; i++)
    {
        R3D_LIST_CLEAR(R3D_MOD_RENDER.list[i]);
    }

    R3D_LIST_CLEAR(R3D_MOD_RENDER.clusters);
    R3D_LIST_CLEAR(R3D_MOD_RENDER.groupVisibility);
    R3D_LIST_CLEAR(R3D_MOD_RENDER.callIndices);
    R3D_LIST_CLEAR(R3D_MOD_RENDER.groups);
    R3D_LIST_CLEAR(R3D_MOD_RENDER.calls);
    R3D_LIST_CLEAR(R3D_MOD_RENDER.groupIndices);
    R3D_LIST_CLEAR(R3D_MOD_RENDER.sortCache);

    R3D_MOD_RENDER.groupCulled = false;
}

bool r3d_render_cluster_begin(BoundingBox aabb)
{
    if (R3D_MOD_RENDER.activeCluster >= 0)
    {
        return false;
    }

    r3d_render_cluster_t cluster = {
        .aabb = aabb,
        .visible = R3D_RENDER_VISBILITY_UNKNOWN
    };

    R3D_MOD_RENDER.activeCluster = (int)R3D_LIST_LENGTH(R3D_MOD_RENDER.clusters);
    R3D_LIST_PUSH(R3D_MOD_RENDER.clusters, cluster);

    return true;
}

bool r3d_render_cluster_end(void)
{
    if (R3D_MOD_RENDER.activeCluster < 0) return false;
    R3D_MOD_RENDER.activeCluster = -1;
    return true;
}

void r3d_render_group_push(const r3d_render_group_t* group)
{
    r3d_render_group_visibility_t visibility = {
        .clusterIndex = R3D_MOD_RENDER.activeCluster,
        .visible = R3D_RENDER_VISBILITY_UNKNOWN
    };

    r3d_render_indices_t indices = {0};

    R3D_LIST_PUSH(R3D_MOD_RENDER.groupVisibility, visibility);
    R3D_LIST_PUSH(R3D_MOD_RENDER.callIndices, indices);
    R3D_LIST_PUSH(R3D_MOD_RENDER.groups, *group);
}

void r3d_render_call_push(const r3d_render_call_t* call)
{
    // Get group and their call indices
    int groupIndex = array_get_last_group_index();
    r3d_render_group_t* group = &R3D_LIST_GET(R3D_MOD_RENDER.groups, r3d_render_group_t, groupIndex);
    r3d_render_indices_t* indices = &R3D_LIST_GET(R3D_MOD_RENDER.callIndices, r3d_render_indices_t, groupIndex);

    // Get call index and set call group indices
    int callIndex = (int)R3D_LIST_LENGTH(R3D_MOD_RENDER.calls);
    if (indices->numCall == 0) indices->firstCall = callIndex;
    indices->numCall += 1;

    // Push this draw call and its group index
    R3D_LIST_PUSH(R3D_MOD_RENDER.groupIndices, groupIndex);
    R3D_LIST_PUSH(R3D_MOD_RENDER.calls, *call);

    // Push draw call index in the passes lists
    bool instanced = r3d_render_has_instances(group);

    if (call->type == R3D_RENDER_CALL_DECAL)
    {
        r3d_render_list_enum_t list = instanced ? R3D_RENDER_LIST_DECAL_INST : R3D_RENDER_LIST_DECAL;
        R3D_LIST_PUSH(R3D_MOD_RENDER.list[list], callIndex);
    }
    else
    {
        r3d_render_list_enum_t opaqueList = instanced ? R3D_RENDER_LIST_OPAQUE_INST : R3D_RENDER_LIST_OPAQUE;
        r3d_render_list_enum_t blendList  = instanced ? R3D_RENDER_LIST_BLEND_INST  : R3D_RENDER_LIST_BLEND;

        switch (call->mesh.material.transparencyMode)
        {
        case R3D_TRANSPARENCY_OPAQUE:
            R3D_LIST_PUSH(R3D_MOD_RENDER.list[opaqueList], callIndex);
            break;
        case R3D_TRANSPARENCY_HYBRID:
            R3D_LIST_PUSH(R3D_MOD_RENDER.list[opaqueList], callIndex);
            R3D_LIST_PUSH(R3D_MOD_RENDER.list[blendList], callIndex);
            break;
        case R3D_TRANSPARENCY_BLEND:
            R3D_LIST_PUSH(R3D_MOD_RENDER.list[blendList], callIndex);
            break;
        }
    }

    // Reserve a matching slot in the sort cache (filled in later by r3d_render_sort_list)
    R3D_LIST_PUSH(R3D_MOD_RENDER.sortCache, (r3d_render_sort_t) {0});
}

r3d_render_group_t* r3d_render_get_call_group(const r3d_render_call_t* call)
{
    int callIndex = array_get_call_index(call);
    int groupIndex = R3D_LIST_GET(R3D_MOD_RENDER.groupIndices, int, callIndex);
    r3d_render_group_t* group = &R3D_LIST_GET(R3D_MOD_RENDER.groups, r3d_render_group_t, groupIndex);

    return group;
}

void r3d_render_cull_groups(const R3D_Frustum* frustum)
{
    size_t numGroups = R3D_LIST_LENGTH(R3D_MOD_RENDER.groups);
    size_t numClusters = R3D_LIST_LENGTH(R3D_MOD_RENDER.clusters);

    // Reset visibility states if groups were already culled in a previous pass
    if (R3D_MOD_RENDER.groupCulled)
    {
        for (size_t i = 0; i < numGroups; i++)
        {
            R3D_LIST_GET(R3D_MOD_RENDER.groupVisibility, r3d_render_group_visibility_t, i).visible = R3D_RENDER_VISBILITY_UNKNOWN;
        }
        for (size_t i = 0; i < numClusters; i++)
        {
            R3D_LIST_GET(R3D_MOD_RENDER.clusters, r3d_render_cluster_t, i).visible = R3D_RENDER_VISBILITY_UNKNOWN;
        }
    }
    R3D_MOD_RENDER.groupCulled = true;

    // Perform frustum culling for each group
    for (size_t i = 0; i < numGroups; i++)
    {
        r3d_render_group_visibility_t* visibility = &R3D_LIST_GET(R3D_MOD_RENDER.groupVisibility, r3d_render_group_visibility_t, i);
        const r3d_render_group_t* group = &R3D_LIST_GET(R3D_MOD_RENDER.groups, r3d_render_group_t, i);

        // Branch 1: Group belongs to a cluster
        if (visibility->clusterIndex >= 0)
        {
            r3d_render_cluster_t* cluster = &R3D_LIST_GET(R3D_MOD_RENDER.clusters, r3d_render_cluster_t, visibility->clusterIndex);

            // Test cluster once (shared by multiple groups)
            if (cluster->visible == R3D_RENDER_VISBILITY_UNKNOWN)
            {
                cluster->visible = is_aabb_visible(frustum, cluster->aabb);
            }

            // If cluster is visible, test the group
            if (cluster->visible == R3D_RENDER_VISBILITY_TRUE)
            {
                // For instanced: trust cluster visibility
                // For others: test group AABB individually
                if (r3d_render_has_instances(group)) visibility->visible = R3D_RENDER_VISBILITY_TRUE;
                else visibility->visible = is_obb_visible(frustum, group->obb);
            }
            else
            {
                visibility->visible = R3D_RENDER_VISBILITY_FALSE;
            }
        }
        // Branch 2: Group without cluster
        else
        {
            // For instanced: always visible
            // For others: test group AABB
            if (r3d_render_has_instances(group)) visibility->visible = R3D_RENDER_VISBILITY_TRUE;
            else visibility->visible = is_obb_visible(frustum, group->obb);
        }
    }
}

bool r3d_render_call_is_visible(const r3d_render_call_t* call, const R3D_Frustum* frustum)
{
    // Get the draw call's parent group and its visibility state
    int callIndex = array_get_call_index(call);
    int groupIndex = R3D_LIST_GET(R3D_MOD_RENDER.groupIndices, int, callIndex);
    const r3d_render_group_t* group = &R3D_LIST_GET(R3D_MOD_RENDER.groups, r3d_render_group_t, groupIndex);
    r3d_render_visibility_enum_t groupVisibility = R3D_LIST_GET(R3D_MOD_RENDER.groupVisibility, r3d_render_group_visibility_t, groupIndex).visible;

    // If the group was already culled, reject immediately
    if (groupVisibility == R3D_RENDER_VISBILITY_FALSE)
    {
        return false;
    }

    // If the group passed culling, check if we can skip per-call testing
    if (groupVisibility == R3D_RENDER_VISBILITY_TRUE)
    {
        // Single-call groups were already tested at the group level
        if (R3D_LIST_GET(R3D_MOD_RENDER.callIndices, r3d_render_indices_t, groupIndex).numCall == 1)
        {
            return true;
        }
        // Instanced/skinned groups: trust the group-level test
        if (r3d_render_has_instances(group) || group->skinTexture > 0)
        {
            return true;
        }
        // Multi-call group: fall through to individual call testing
    }
    // If the group hasn't been tested yet, check instanced/skinned groups now
    else if (groupVisibility == R3D_RENDER_VISBILITY_UNKNOWN)
    {
        if (r3d_render_has_instances(group) || group->skinTexture > 0)
        {
            return is_obb_visible(frustum, group->obb);
        }
        // Regular multi-call group: fall through to individual call testing
    }

    // Test this specific draw call against the frustum
    return is_draw_call_visible(frustum, call, group->transform);
}

void r3d_render_sort_list(r3d_render_list_enum_t list, Vector3 viewPosition, r3d_render_sort_enum_t mode)
{
    G_sortViewPosition = viewPosition;

    int (*compareFunc)(const void *a, const void *b) = NULL;
    r3d_list_t* drawListCalls = R3D_MOD_RENDER.list[list];

    switch (mode)
    {
    case R3D_RENDER_SORT_FRONT_TO_BACK:
        compareFunc = compare_front_to_back;
        sort_fill_cache_front_to_back(list);
        break;
    case R3D_RENDER_SORT_BACK_TO_FRONT:
        compareFunc = compare_back_to_front;
        sort_fill_cache_back_to_front(list);
        break;
    case R3D_RENDER_SORT_MATERIAL_ONLY:
        compareFunc = compare_materials_only;
        sort_fill_cache_by_material(list);
        break;
    }

    qsort(
        drawListCalls->elements,
        drawListCalls->elemCount,
        drawListCalls->elemSize,
        compareFunc
    );
}

void r3d_render_prepare_drawing(void)
{
    glBindVertexArray(R3D_MOD_RENDER.globalVao);
}

void r3d_render_draw(const r3d_render_call_t* call)
{
    GLenum primitive;
    r3d_render_range_t vertexRange;
    r3d_render_range_t indexRange;

    get_draw_call_info(call, &primitive, &vertexRange, &indexRange);

    if (indexRange.count == 0)
    {
        glDrawArrays(primitive, vertexRange.offset, vertexRange.count);
    }
    else
    {
        glDrawElementsBaseVertex(
            primitive,
            indexRange.count,
            GL_UNSIGNED_INT,
            (void*)(indexRange.offset * sizeof(GLuint)),
            vertexRange.offset
        );
    }
}

void r3d_render_draw_instanced(const r3d_render_call_t* call)
{
    GLenum primitive;
    r3d_render_range_t vertexRange;
    r3d_render_range_t indexRange;

    get_draw_call_info(call, &primitive, &vertexRange, &indexRange);

    const r3d_render_group_t* group = r3d_render_get_call_group(call);

    instances_enable(group->instances.buffers, group->instances.layout, group->instanceOffset);

    if (indexRange.count == 0)
    {
        glDrawArraysInstanced(primitive, vertexRange.offset, vertexRange.count, group->instanceCount);
    }
    else
    {
        glDrawElementsInstancedBaseVertex(
            primitive,
            indexRange.count,
            GL_UNSIGNED_INT,
            (void*)(indexRange.offset * sizeof(GLuint)),
            group->instanceCount,
            vertexRange.offset
        );
    }

    // Instance attributes (locations 10-14) are intentionally left enabled after
    // the draw. This is safe as long as non-instanced vertex shaders
    // never read those locations.

    //instances_disable(group->instances.layout.flags);
}

void r3d_render_draw_shape(r3d_render_shape_enum_t shape)
{
    r3d_render_shape_t* s = &R3D_MOD_RENDER.shapes[shape];
    if (s->vertices.count == 0)
    {
        SHAPE_LOADERS[shape](s);
        glBindVertexArray(R3D_MOD_RENDER.globalVao);
    }

    if (s->elements.count == 0)
    {
        glDrawArrays(GL_TRIANGLES, s->vertices.offset, s->vertices.count);
    }
    else
    {
        glDrawElementsBaseVertex(
            GL_TRIANGLES,
            s->elements.count,
            GL_UNSIGNED_INT,
            (void*)(s->elements.offset * sizeof(GLuint)),
            s->vertices.offset
        );
    }
}
