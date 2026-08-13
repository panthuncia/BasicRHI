#ifndef BASICRHI_C_H
#define BASICRHI_C_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define BASICRHI_C_CALL __cdecl
#else
#define BASICRHI_C_CALL
#endif

#define BASICRHI_C_ABI_VERSION_1 1u
#define BASICRHI_C_INVALID_INDEX UINT32_MAX

typedef enum basicrhi_c_result {
    BASICRHI_C_OK = 0,
    BASICRHI_C_INVALID_ARGUMENT = 1,
    BASICRHI_C_STALE_BORROW = 2,
    BASICRHI_C_UNSUPPORTED = 3,
    BASICRHI_C_BACKEND_ERROR = 4
} basicrhi_c_result;

typedef struct basicrhi_c_handle {
    uint32_t index;
    uint32_t generation;
} basicrhi_c_handle;

typedef basicrhi_c_handle basicrhi_c_resource;
typedef basicrhi_c_handle basicrhi_c_view;
typedef basicrhi_c_handle basicrhi_c_sampler;
typedef basicrhi_c_handle basicrhi_c_pipeline;
typedef basicrhi_c_handle basicrhi_c_pipeline_layout;
typedef basicrhi_c_handle basicrhi_c_command_signature;
typedef basicrhi_c_handle basicrhi_c_descriptor_heap;

typedef struct basicrhi_c_byte_span {
    const void* data;
    size_t size_bytes;
} basicrhi_c_byte_span;

typedef enum basicrhi_c_resource_kind {
    BASICRHI_C_RESOURCE_BUFFER = 0,
    BASICRHI_C_RESOURCE_TEXTURE_2D = 1
} basicrhi_c_resource_kind;

typedef enum basicrhi_c_heap_kind {
    BASICRHI_C_HEAP_DEVICE_LOCAL = 0,
    BASICRHI_C_HEAP_UPLOAD = 1,
    BASICRHI_C_HEAP_READBACK = 2
} basicrhi_c_heap_kind;

typedef struct basicrhi_c_resource_desc_v1 {
    uint32_t structure_size;
    uint32_t kind;
    uint32_t heap_kind;
    uint32_t format;
    uint64_t size_bytes;
    uint32_t element_stride;
    uint32_t usage_flags;
    uint32_t width;
    uint32_t height;
    uint32_t depth_or_array_size;
    uint32_t mip_levels;
    uint32_t sample_count;
    uint32_t reserved[3];
} basicrhi_c_resource_desc_v1;

typedef enum basicrhi_c_view_kind {
    BASICRHI_C_VIEW_SRV = 0,
    BASICRHI_C_VIEW_UAV = 1,
    BASICRHI_C_VIEW_RTV = 2,
    BASICRHI_C_VIEW_DSV = 3
} basicrhi_c_view_kind;

typedef struct basicrhi_c_view_desc_v1 {
    uint32_t structure_size;
    uint32_t kind;
    uint32_t format;
    uint32_t first_mip;
    uint32_t mip_count;
    uint32_t first_array_slice;
    uint32_t array_size;
    uint32_t first_element;
    uint32_t element_count;
    uint32_t element_stride;
} basicrhi_c_view_desc_v1;

typedef struct basicrhi_c_sampler_desc_v1 {
    uint32_t structure_size;
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t mip_filter;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    uint32_t comparison;
    float mip_lod_bias;
    float min_lod;
    float max_lod;
    float max_anisotropy;
    float border_color[4];
} basicrhi_c_sampler_desc_v1;

typedef struct basicrhi_c_layout_binding_v1 {
    uint32_t set;
    uint32_t binding;
    uint32_t descriptor_type;
    uint32_t descriptor_count;
    uint32_t shader_stages;
} basicrhi_c_layout_binding_v1;

typedef struct basicrhi_c_pipeline_layout_desc_v1 {
    uint32_t structure_size;
    const basicrhi_c_layout_binding_v1* bindings;
    size_t binding_count;
    uint32_t push_constant_size;
    uint32_t push_constant_stages;
} basicrhi_c_pipeline_layout_desc_v1;

typedef enum basicrhi_c_pipeline_kind {
    BASICRHI_C_PIPELINE_RASTER = 0,
    BASICRHI_C_PIPELINE_COMPUTE = 1,
    BASICRHI_C_PIPELINE_MESH = 2
} basicrhi_c_pipeline_kind;

typedef struct basicrhi_c_pipeline_desc_v1 {
    uint32_t structure_size;
    uint32_t kind;
    basicrhi_c_pipeline_layout layout;
    basicrhi_c_byte_span vertex_shader;
    basicrhi_c_byte_span pixel_shader;
    basicrhi_c_byte_span compute_shader;
    basicrhi_c_byte_span amplification_shader;
    basicrhi_c_byte_span mesh_shader;
    uint32_t primitive_topology;
    uint32_t raster_flags;
    uint32_t depth_format;
    uint32_t render_target_formats[8];
    uint32_t render_target_count;
    uint32_t sample_count;
} basicrhi_c_pipeline_desc_v1;

typedef struct basicrhi_c_command_argument_v1 {
    uint32_t kind;
    uint32_t slot;
    uint32_t byte_offset;
    uint32_t byte_size;
} basicrhi_c_command_argument_v1;

typedef struct basicrhi_c_command_signature_desc_v1 {
    uint32_t structure_size;
    uint32_t byte_stride;
    basicrhi_c_pipeline_layout layout;
    const basicrhi_c_command_argument_v1* arguments;
    size_t argument_count;
} basicrhi_c_command_signature_desc_v1;

typedef struct basicrhi_c_texture_copy_region_v1 {
    basicrhi_c_resource texture;
    uint32_t mip;
    uint32_t array_slice;
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} basicrhi_c_texture_copy_region_v1;

/* Valid only during one contributor execute callback. No end/reset/submit entry points exist. */
typedef struct basicrhi_c_borrowed_recorder_v1 {
    uint32_t structure_size;
    uint32_t abi_version;
    void* context;
    uint64_t borrow_token;
    basicrhi_c_result (BASICRHI_C_CALL *bind_layout)(void*, uint64_t, basicrhi_c_pipeline_layout);
    basicrhi_c_result (BASICRHI_C_CALL *bind_pipeline)(void*, uint64_t, basicrhi_c_pipeline);
    basicrhi_c_result (BASICRHI_C_CALL *set_descriptor_heaps)(void*, uint64_t, basicrhi_c_descriptor_heap, basicrhi_c_descriptor_heap);
    basicrhi_c_result (BASICRHI_C_CALL *push_constants)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, basicrhi_c_byte_span);
    basicrhi_c_result (BASICRHI_C_CALL *draw)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *draw_indexed)(void*, uint64_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *dispatch)(void*, uint64_t, uint32_t, uint32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *dispatch_mesh)(void*, uint64_t, uint32_t, uint32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *execute_indirect)(void*, uint64_t, basicrhi_c_command_signature,
        basicrhi_c_resource, uint64_t, basicrhi_c_resource, uint64_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *copy_buffer)(void*, uint64_t, basicrhi_c_resource, uint64_t,
        basicrhi_c_resource, uint64_t, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *copy_texture)(void*, uint64_t,
        const basicrhi_c_texture_copy_region_v1*, const basicrhi_c_texture_copy_region_v1*);
} basicrhi_c_borrowed_recorder_v1;

typedef struct basicrhi_c_device_api_v1 {
    uint32_t structure_size;
    uint32_t abi_version;
    void* context;
    basicrhi_c_result (BASICRHI_C_CALL *query_capability)(void*, uint32_t, void*, size_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_resource)(void*, const basicrhi_c_resource_desc_v1*, basicrhi_c_resource*);
    void (BASICRHI_C_CALL *destroy_resource)(void*, basicrhi_c_resource, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_view)(void*, basicrhi_c_resource, const basicrhi_c_view_desc_v1*, basicrhi_c_view*);
    void (BASICRHI_C_CALL *destroy_view)(void*, basicrhi_c_view, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_sampler)(void*, const basicrhi_c_sampler_desc_v1*, basicrhi_c_sampler*);
    void (BASICRHI_C_CALL *destroy_sampler)(void*, basicrhi_c_sampler, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_pipeline_layout)(void*, const basicrhi_c_pipeline_layout_desc_v1*, basicrhi_c_pipeline_layout*);
    void (BASICRHI_C_CALL *destroy_pipeline_layout)(void*, basicrhi_c_pipeline_layout, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_pipeline)(void*, const basicrhi_c_pipeline_desc_v1*, basicrhi_c_pipeline*);
    void (BASICRHI_C_CALL *destroy_pipeline)(void*, basicrhi_c_pipeline, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_command_signature)(void*, const basicrhi_c_command_signature_desc_v1*, basicrhi_c_command_signature*);
    void (BASICRHI_C_CALL *destroy_command_signature)(void*, basicrhi_c_command_signature, uint64_t);
} basicrhi_c_device_api_v1;

#if defined(__cplusplus)
static_assert(sizeof(basicrhi_c_handle) == 8, "BasicRHI C handle ABI drift");
#endif

#endif
