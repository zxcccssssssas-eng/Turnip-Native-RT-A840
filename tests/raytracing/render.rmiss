#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT uint value;
void main() { value = 0xff201810; }
