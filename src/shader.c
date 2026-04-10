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

char *shader_preprocess(const char *source, const char *include_dir)
{
    /* Estimate output size (includes expand it) */
    size_t cap = strlen(source) * 2 + 4096;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t len = 0;

    const char *p = source;
    while (*p) {
        /* Find the next line */
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p + 1) : strlen(p);

        /* Check for #include "filename" */
        const char *inc = p;
        while (*inc == ' ' || *inc == '\t') inc++;
        if (strncmp(inc, "#include", 8) == 0) {
            const char *q = inc + 8;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"') {
                const char *name_start = q + 1;
                const char *name_end = strchr(name_start, '"');
                if (name_end) {
                    char inc_path[512];
                    snprintf(inc_path, sizeof(inc_path), "%s/%.*s",
                             include_dir, (int)(name_end - name_start), name_start);
                    char *inc_src = shader_load_file(inc_path);
                    if (!inc_src) {
                        LOG_ERROR("Cannot resolve #include \"%.*s\" (%s)",
                                  (int)(name_end - name_start), name_start, inc_path);
                        free(out);
                        return NULL;
                    }
                    /* Recursively preprocess the included file */
                    char *inc_expanded = shader_preprocess(inc_src, include_dir);
                    free(inc_src);
                    if (!inc_expanded) { free(out); return NULL; }

                    size_t inc_len = strlen(inc_expanded);
                    while (len + inc_len + 1 >= cap) {
                        cap *= 2;
                        char *tmp = realloc(out, cap);
                        if (!tmp) { free(inc_expanded); free(out); return NULL; }
                        out = tmp;
                    }
                    memcpy(out + len, inc_expanded, inc_len);
                    len += inc_len;
                    free(inc_expanded);

                    p += line_len;
                    continue;
                }
            }
        }

        /* Regular line — copy through */
        while (len + line_len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        memcpy(out + len, p, line_len);
        len += line_len;
        p += line_len;
    }

    out[len] = '\0';
    return out;
}

GLuint shader_load_program_with_includes(const char *vert_path, const char *frag_path,
                                          const char *include_dir)
{
    char *vert_src = shader_load_file(vert_path);
    char *frag_src = shader_load_file(frag_path);
    if (!vert_src || !frag_src) {
        free(vert_src);
        free(frag_src);
        return 0;
    }

    /* Preprocess includes (only frag typically needs it, but do both) */
    char *vert_pp = shader_preprocess(vert_src, include_dir);
    char *frag_pp = shader_preprocess(frag_src, include_dir);
    free(vert_src);
    free(frag_src);
    if (!vert_pp || !frag_pp) {
        free(vert_pp);
        free(frag_pp);
        return 0;
    }

    GLuint vert = shader_compile(GL_VERTEX_SHADER, vert_pp);
    GLuint frag = shader_compile(GL_FRAGMENT_SHADER, frag_pp);
    free(vert_pp);
    free(frag_pp);

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
