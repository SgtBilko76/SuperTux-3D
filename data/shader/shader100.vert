#version 100

attribute vec2 texcoord;
attribute vec2 position;
attribute vec4 diffuse;

varying mediump vec2 texcoord_var;
varying lowp vec4 diffuse_var;

uniform mat4 modelviewprojection;

void main(void)
{
  texcoord_var = texcoord;
  diffuse_var = diffuse;
  gl_Position = modelviewprojection * vec4(position, 0.0, 1.0);
}

/* EOF */
