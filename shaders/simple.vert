#version 460 core

layout(location = 0) in  vec3 pos;
layout(location = 1) in  vec3 normal;

layout(location = 0) out vec3 out_pos;
layout(location = 1) out vec3 out_normal;

layout(set = 0, binding = 0) uniform data
{
    mat4 model_view_proj;
    mat4 model_view;
};

void main()
{
    gl_Position = model_view_proj * vec4(pos, 1);
    out_pos     = (model_view * vec4(pos, 1)).xyz;
    out_normal  = mat3(model_view) * normal; // assume uniform scaling, so no inverse transpose
}
