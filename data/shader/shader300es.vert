#version 300 es

precision highp float;

in vec2 texcoord;
in vec2 position;
in vec4 diffuse;

out vec2 texcoord_var;
out vec4 diffuse_var;

uniform mat4 modelviewprojection;

void main(void)
{
  texcoord_var = texcoord;
  diffuse_var = diffuse;
  gl_Position = modelviewprojection * vec4(position, 0.0, 1.0);
}

/* EOF */
