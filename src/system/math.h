#pragma once

typedef struct wc_float2 {
    float x, y;
} wc_float2;

typedef struct wc_float3 {
    float x, y, z;
} wc_float3;

typedef struct wc_float4 {
    float x, y, z, w;
} wc_float4;

typedef struct wc_aabb {
    wc_float3 min, max;
} wc_aabb;