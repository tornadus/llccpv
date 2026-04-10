#include "shader.h"
#include "util.h"

GLuint shader_compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOG_ERROR("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint shader_link(GLuint vert, GLuint frag)
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        LOG_ERROR("Shader link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

char *shader_load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_ERROR("Cannot open shader file '%s': %s", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, len, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

GLuint shader_load_program(const char *vert_path, const char *frag_path)
{
    char *vert_src = shader_load_file(vert_path);
    char *frag_src = shader_load_file(frag_path);
    if (!vert_src || !frag_src) {
        free(vert_src);
        free(frag_src);
        return 0;
    }

    GLuint vert = shader_compile(GL_VERTEX_SHADER, vert_src);
    GLuint frag = shader_compile(GL_FRAGMENT_SHADER, frag_src);
    free(vert_src);
    free(frag_src);

    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return 0;
    }

    GLuint prog = shader_link(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}
