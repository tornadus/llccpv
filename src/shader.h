#ifndef LLCCPV_SHADER_H
#define LLCCPV_SHADER_H

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

/* Compile a shader from source. Returns 0 on failure. */
GLuint shader_compile(GLenum type, const char *source);

/* Link a vertex and fragment shader into a program. Returns 0 on failure. */
GLuint shader_link(GLuint vert, GLuint frag);

/* Read an entire file into a malloc'd string. Returns NULL on failure. */
char *shader_load_file(const char *path);

/* Compile and link shaders from file paths. Returns 0 on failure. */
GLuint shader_load_program(const char *vert_path, const char *frag_path);

/* Resolve #include "file" directives in shader source.
 * Included paths are relative to include_dir.
 * Returns malloc'd string with includes expanded, or NULL on error. */
char *shader_preprocess(const char *source, const char *include_dir);

/* Like shader_load_program but preprocesses #include directives first.
 * include_dir is the base path for resolving #include "file" directives. */
GLuint shader_load_program_with_includes(const char *vert_path, const char *frag_path,
                                          const char *include_dir);

#endif
