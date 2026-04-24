#ifndef LLCCPV_SHADER_H
#define LLCCPV_SHADER_H

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

/* Compile and link shaders from file paths. Returns 0 on failure. */
GLuint shader_load_program(const char *vert_path, const char *frag_path);

/* Like shader_load_program but preprocesses #include directives first.
 * include_dir is the base path for resolving #include "file" directives. */
GLuint shader_load_program_with_includes(const char *vert_path, const char *frag_path,
                                          const char *include_dir);

#endif
