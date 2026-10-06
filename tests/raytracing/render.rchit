#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT uint value;
hitAttributeEXT vec2 barycentrics;
void main() {
    value = packUnorm4x8(vec4(1.0 - barycentrics.x - barycentrics.y,
        barycentrics.x, barycentrics.y, 1.0));
}
