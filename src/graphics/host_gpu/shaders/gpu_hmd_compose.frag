#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source;
layout(push_constant) uniform Layer {
	// Normalized offset and size of the eye's region in the source texture.
	vec4 region;
};

void main() {
	color = texture(source, region.xy + uv * region.zw);
}
