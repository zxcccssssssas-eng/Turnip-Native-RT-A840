#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT uint value;
hitAttributeEXT vec2 barycentrics;
void main() { value = 11; }
