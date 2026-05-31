attribute vec3 in_position;
uniform mat4 modelViewProj;

void main()
{
    gl_Position = modelViewProj * vec4(in_position, 1.0);
}
