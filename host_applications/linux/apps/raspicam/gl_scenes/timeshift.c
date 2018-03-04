/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, Tim Gover
All rights reserved.


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "timeshift.h"
#include "RaspiTex.h"
#include "RaspiTexUtil.h"
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* \file timeshift.c
 * Example code for implementing Sobel filter as GLSL shaders.
 * The input image is a greyscale texture from the MMAL buffer Y plane.
 */

#define TIMESHIFT_VSHADER_SOURCE \
    "attribute vec2 vertex;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "   texcoord = 0.5 * (vertex + 1.0);\n" \
    "   gl_Position = vec4(vertex, 0.0, 1.0);\n" \
    "}\n"

/* Example Sobel edge detct shader. The texture format for
 * EGL_IMAGE_BRCM_MULTIMEDIA_Y is a one byte per pixel greyscale GL_LUMINANCE.
 * If the output is to be fed into another image processing shader then it may
 * be worth changing this code to take 4 input Y pixels and pack the result
 * into a 32bpp RGBA pixel.
 */
#define TIMESHIFT_FSHADER_SOURCE \
    "#extension GL_OES_EGL_image_external : require\n" \
    "uniform samplerExternalOES tex;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    float x = texcoord.x;\n" \
    "    float y = texcoord.y;\n" \
    "    gl_FragColor = texture2D(tex, vec2(x, y));\n" \
    "}\n"
  
static GLfloat quad_varray[] = {
   -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f,
   -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f,
};

static GLuint quad_vbo;

static RASPITEXUTIL_SHADER_PROGRAM_T timeshift_shader =
{
    .vertex_source = TIMESHIFT_VSHADER_SOURCE,
    .fragment_source = TIMESHIFT_FSHADER_SOURCE,
    .uniform_names = {"tex"},
    .attribute_names = {"vertex"},
};

static const EGLint timeshift_egl_config_attribs[] =
{
   EGL_RED_SIZE,   8,
   EGL_GREEN_SIZE, 8,
   EGL_BLUE_SIZE,  8,
   EGL_ALPHA_SIZE, 8,
   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
   EGL_NONE
};


/**
 * Initialisation of shader uniforms.
 *
 * @param width Width of the EGL image.
 * @param width Height of the EGL image.
 */
static int shader_set_uniforms(RASPITEXUTIL_SHADER_PROGRAM_T *shader,
      int width, int height)
{
   GLCHK(glUseProgram(shader->program));
   GLCHK(glUniform1i(shader->uniform_locations[0], 0)); // Texture unit

   /* Dimensions of a single pixel in texture co-ordinates */
   GLCHK(glUniform2f(shader->uniform_locations[1],
            1.0 / (float) width, 1.0 / (float) height));

   /* Enable attrib 0 as vertex array */
   GLCHK(glEnableVertexAttribArray(shader->attribute_locations[0]));
   return 0;
}

/**
 * Creates the OpenGL ES 2.X context and builds the shaders.
 * @param raspitex_state A pointer to the GL preview state.
 * @return Zero if successful.
 */
static int timeshift_init(RASPITEX_STATE *raspitex_state)
{
    int rc = 0;
    int width = raspitex_state->width;
    int height = raspitex_state->height;

    vcos_log_trace("%s", VCOS_FUNCTION);
    raspitex_state->egl_config_attribs = timeshift_egl_config_attribs;
    rc = raspitexutil_gl_init_2_0(raspitex_state);
    if (rc != 0)
       goto end;

    rc = raspitexutil_build_shader_program(&timeshift_shader);
    if (rc != 0)
       goto end;

    rc = shader_set_uniforms(&timeshift_shader, width, height);
    if (rc != 0)
       goto end;

   GLCHK(glGenBuffers(1, &quad_vbo));
   GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
   GLCHK(glBufferData(GL_ARRAY_BUFFER, sizeof(quad_varray), quad_varray, GL_STATIC_DRAW));
   GLCHK(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));

end:
    return rc;
}

/* Redraws the scene with the latest luma buffer.
 *
 * @param raspitex_state A pointer to the GL preview state.
 * @return Zero if successful.
 */
static int timeshift_redraw(RASPITEX_STATE* state)
{
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   GLCHK(glUseProgram(timeshift_shader.program));

   int time_index;
   switch (state->timeshift_mode) {
   case RASPITEX_TIMESHIFT_POLARI: {
     time_index = (state->texture_count - 1) - state->texture_index;
   } break;
   case RASPITEX_TIMESHIFT_RANDOM: {
     time_index = rand() % state->texture_count;
   } break;
   case RASPITEX_TIMESHIFT_PING_PONG: {
     time_index = (state->texture_count - 1) - (state->texture_index * 2);
     if (time_index < 0) {
       time_index = -time_index;
     }
   } break;
   case RASPITEX_TIMESHIFT_SLUR: {
     static float angle = 0.0f;
     const int delay = fabsf(sinf(angle) * state->texture_count);
     angle += 0.05f;
     time_index = (state->texture_index + state->texture_count - delay) % state->texture_count;
   } break;
   default: {
     time_index = state->texture_index;
   } break;
   }
   
   /* Bind the Y plane texture */
   GLCHK(glActiveTexture(GL_TEXTURE0));
   GLCHK(glBindTexture(GL_TEXTURE_EXTERNAL_OES, state->textures[time_index]));
   GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
   GLCHK(glEnableVertexAttribArray(timeshift_shader.attribute_locations[0]));
   GLCHK(glVertexAttribPointer(timeshift_shader.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, 0));
   GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));

   return 0;
}

int timeshift_open(RASPITEX_STATE *state)
{
   state->ops.gl_init = timeshift_init;
   state->ops.redraw = timeshift_redraw;
   state->ops.update_texture = raspitexutil_update_texture;
   return 0;
}
