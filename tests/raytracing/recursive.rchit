#version 460
#extension GL_EXT_ray_tracing : require
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(location = 0) rayPayloadInEXT uint value;
layout(location = 1) rayPayloadEXT uint secondary;
hitAttributeEXT vec2 barycentrics;
void main() {
    secondary = 0;
    traceRayEXT(scene, gl_RayFlagsOpaqueEXT, 255, 0, 0, 0,
        vec3(2, 0, 1), 0.001, vec3(0, 0, -1), 10.0, 1);
    value = 11 + secondary;
}
