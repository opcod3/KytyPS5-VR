#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source;
layout(binding = 1) uniform sampler2D fsr_horizontal;
layout(binding = 2) uniform sampler2D fsr_vertical;
layout(push_constant) uniform Layer {
	// Normalized offset and size of the eye's region in the source texture.
	vec4 region;
	uint fsr;
};

void main() {
	vec2 position = uv;
	if (fsr != 0u) {
		// Each table maps an output pixel to its normalized position in FSR space.
		const ivec2 size  = ivec2(textureSize(fsr_horizontal, 0).x, textureSize(fsr_vertical, 0).x);
		const ivec2 pixel = min(ivec2(uv * vec2(size)), size - 1);
		position = vec2(texelFetch(fsr_horizontal, ivec2(pixel.x, 0), 0).r,
		                texelFetch(fsr_vertical, ivec2(pixel.y, 0), 0).r);
	}
	color = texture(source, region.xy + position * region.zw);
}
