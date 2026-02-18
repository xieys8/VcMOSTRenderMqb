// vnc_qnx_pipelined.cpp

#include <cstdlib>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/keycodes.h>
#include <time.h>
#include "stb_easyfont.hh"
#include <regex.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "miniz.h"

#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <errno.h>

#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/param.h>
#include <netinet/tcp_var.h>

#ifndef TCP_USER_TIMEOUT
#define TCP_USER_TIMEOUT 18  // how long for loss retry before timeout [ms]
#endif

// ---------------- Timing (C++98-friendly) ----------------
struct FrameTimings {
    double recv_ms;
    double inflate_ms;
    double parse_ms;
    double texture_upload_ms;
    double total_frame_ms;
    FrameTimings() : recv_ms(0.0), inflate_ms(0.0), parse_ms(0.0), texture_upload_ms(0.0), total_frame_ms(0.0) {}
};

static uint64_t now_us()
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
    }
#endif
    // Fallback
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static double us_to_ms(uint64_t us) { return (double)us / 1000.0; }

static ssize_t recv_timed(int sockfd, void* buf, size_t len, int flags, FrameTimings* timings)
{
    uint64_t t0 = now_us();
    ssize_t r = recv(sockfd, buf, len, flags);
    uint64_t t1 = now_us();
    if (timings) timings->recv_ms += us_to_ms(t1 - t0);
    return r;
}

// recv until exactly len bytes read, or fail (timeout / disconnect / error)
static int recv_exact(int sockfd, void* buf, size_t len, FrameTimings* timings)
{
    size_t off = 0;
    char* p = (char*)buf;

    while (off < len) {
        ssize_t r = recv_timed(sockfd, p + off, len - off, 0, timings);
        if (r == 0) {
            // peer closed
            errno = ECONNRESET;
            return -1;
        }
        if (r < 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

// ---------------- GLES setup ----------------
GLuint programObject;
GLuint programObjectTextRender;
EGLDisplay eglDisplay;
EGLConfig eglConfig;
EGLSurface eglSurface;
EGLContext eglContext;

// ---------------- VNC shaders ----------------
const char* vertexShaderSource =
    "attribute vec2 position;    \n"
    "attribute vec2 texCoord;     \n"
    "varying vec2 v_texCoord;     \n"
    "void main()                  \n"
    "{                            \n"
    "   gl_Position = vec4(position, 0.0, 1.0); \n"
    "   v_texCoord = texCoord;   \n"
    "   gl_PointSize = 4.0;      \n"
    "}                            \n";

const char* fragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D texture;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = texture2D(texture, v_texCoord);\n"
    "}\n";

// Text Rendering shaders
const char* vertexShaderSourceText =
    "attribute vec2 position;    \n"
    "void main()                  \n"
    "{                            \n"
    "   gl_Position = vec4(position, 0.0, 1.0); \n"
    "   gl_PointSize = 1.0;      \n"
    "}                            \n";

const char* fragmentShaderSourceText =
    "precision mediump float;\n"
    "void main()               \n"
    "{                         \n"
    "  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); \n"
    "}                         \n";

// Geometry
GLfloat landscapeVertices[] = {
    -0.8f,  0.73f, 0.0f,
     0.8f,  0.73f, 0.0f,
     0.8f, -0.63f, 0.0f,
    -0.8f, -0.63f, 0.0f
};
GLfloat portraitVertices[] = {
    -0.8f,  1.0f, 0.0f,
     0.8f,  1.0f, 0.0f,
     0.8f, -0.67f,0.0f,
    -0.8f, -0.67f,0.0f
};
GLfloat landscapeTexCoords[] = {
    0.0f, 0.07f,
    0.90f,0.07f,
    0.90f,1.0f,
    0.0f, 1.0f
};
GLfloat portraitTexCoords[] = {
    0.0f, 0.0f,
    0.63f,0.0f,
    0.63f,0.3f,
    0.0f, 0.3f
};

// Constants for VNC protocol
const char* PROTOCOL_VERSION = "RFB 003.003\n";
const char FRAMEBUFFER_UPDATE_REQUEST[] = { 3, 0, 0, 0, 0, 0, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF };
const char CLIENT_INIT[] = { 1 };
const char ZLIB_ENCODING[] = { 2, 0, 0, 2, 0, 0, 0, 6, 0, 0, 0, 0 };

// SETUP
int windowWidth  = 800;
int windowHeight = 480;

const char* VNC_SERVER_IP_ADDRESS = "10.173.189.62";
const int   VNC_SERVER_PORT       = 5900;

const char* EXLAP_SERVER_IP_ADDRESS = "127.0.0.1";
const int   EXLAP_SERVER_PORT       = 25010;

// ---------------- QNX EGL helper ----------------
static EGLenum checkErrorEGL(const char* msg) {
    static const char* errmsg[] = {
        "EGL function succeeded",
        "EGL is not initialized, or could not be initialized, for the specified display",
        "EGL cannot access a requested resource",
        "EGL failed to allocate resources for the requested operation",
        "EGL fail to access an unrecognized attribute or attribute value was passed in an attribute list",
        "EGLConfig argument does not name a valid EGLConfig",
        "EGLContext argument does not name a valid EGLContext",
        "EGL current surface of the calling thread is no longer valid",
        "EGLDisplay argument does not name a valid EGLDisplay",
        "EGL arguments are inconsistent",
        "EGLNativePixmapType argument does not refer to a valid native pixmap",
        "EGLNativeWindowType argument does not refer to a valid native window",
        "EGL one or more argument values are invalid",
        "EGLSurface argument does not name a valid surface configured for rendering",
        "EGL power management event has occurred",
    };
    EGLenum error = eglGetError();
    if (error >= EGL_SUCCESS && error <= EGL_CONTEXT_LOST) {
        fprintf(stderr, "%s: %s\n", msg, errmsg[error - EGL_SUCCESS]);
    } else {
        fprintf(stderr, "%s: EGL error 0x%x\n", msg, (unsigned)error);
    }
    return error;
}

struct Command {
    const char* command;
    const char* error_message;
};

void execute_initial_commands() {
    struct Command commands[] = {
        { "/eso/bin/apps/dmdt dc 70 3",  "Create new display table with context 3 failed with error" },
        { "/eso/bin/apps/dmdt sc 4 70",  "Set display 4 (VC) to display table 99 failed with error" }
    };
    size_t num_commands = sizeof(commands) / sizeof(commands[0]);

    for (size_t i = 0; i < num_commands; ++i) {
        printf("Executing '%s'\n", commands[i].command);
        int ret = system(commands[i].command);
        if (ret != 0) fprintf(stderr, "%s: %d\n", commands[i].error_message, ret);
    }
}

void execute_final_commands() {
    struct Command commands[] = {
        { "/eso/bin/apps/dmdt dc 70 33", "Create new display table with context 3 failed with error" },
        { "/eso/bin/apps/dmdt sc 4 70",  "Set display 4 (VC) to display table 99 failed with error" }
    };
    size_t num_commands = sizeof(commands) / sizeof(commands[0]);

    for (size_t i = 0; i < num_commands; ++i) {
        printf("Executing '%s'\n", commands[i].command);
        int ret = system(commands[i].command);
        if (ret != 0) fprintf(stderr, "%s: %d\n", commands[i].error_message, ret);
    }
}

int16_t byteArrayToInt16(const char* byteArray) {
    return (int16_t)(((unsigned char)byteArray[0] << 8) | (unsigned char)byteArray[1]);
}

int32_t byteArrayToInt32(const char* byteArray) {
    return (int32_t)(
        ((uint32_t)(unsigned char)byteArray[0] << 24) |
        ((uint32_t)(unsigned char)byteArray[1] << 16) |
        ((uint32_t)(unsigned char)byteArray[2] <<  8) |
        ((uint32_t)(unsigned char)byteArray[3]      )
    );
}

// ---------------- GL init ----------------
static void compile_check(GLuint shader, const char* tag)
{
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        printf("%s compile failed: %s\n", tag, infoLog);
    }
}

static void link_check(GLuint prog, const char* tag)
{
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char infoLog[512];
        glGetProgramInfoLog(prog, 512, NULL, infoLog);
        printf("%s link failed: %s\n", tag, infoLog);
    }
}

void Init() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexShaderSource, NULL);
    glCompileShader(vs);
    compile_check(vs, "VNC VS");

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentShaderSource, NULL);
    glCompileShader(fs);
    compile_check(fs, "VNC FS");

    GLuint vsT = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vsT, 1, &vertexShaderSourceText, NULL);
    glCompileShader(vsT);
    compile_check(vsT, "TXT VS");

    GLuint fsT = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fsT, 1, &fragmentShaderSourceText, NULL);
    glCompileShader(fsT);
    compile_check(fsT, "TXT FS");

    programObject = glCreateProgram();
    glAttachShader(programObject, vs);
    glAttachShader(programObject, fs);
    glLinkProgram(programObject);
    link_check(programObject, "VNC PROG");

    programObjectTextRender = glCreateProgram();
    glAttachShader(programObjectTextRender, vsT);
    glAttachShader(programObjectTextRender, fsT);
    glLinkProgram(programObjectTextRender);
    link_check(programObjectTextRender, "TXT PROG");

    glClearColor(0.f, 0.f, 0.f, 1.f);
}

// ---------------- Text render (unchanged logic, minor fix for attrib program) ----------------
void print_string(float x, float y, const char* text, float r, float g, float b, float size) {
    (void)r; (void)g; (void)b; // shader is constant color in this example
    char inputBuffer[2000] = { 0 };
    GLfloat triangleBuffer[2000] = { 0 };
    stb_easy_font_print(0, 0, (char*)text, NULL, inputBuffer, sizeof(inputBuffer));

    float ndcMovementX = (2.0f * x) / windowWidth;
    float ndcMovementY = (2.0f * y) / windowHeight;

    int triangleIndex = 0;
    for (int i = 0; i < (int)(sizeof(inputBuffer) / sizeof(GLfloat)); i += 8) {
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[i * sizeof(GLfloat)]) / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 1) * sizeof(GLfloat)]) / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 2) * sizeof(GLfloat)]) / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 3) * sizeof(GLfloat)]) / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 4) * sizeof(GLfloat)]) / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 5) * sizeof(GLfloat)]) / size * -1 + ndcMovementY;

        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[i * sizeof(GLfloat)]) / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 1) * sizeof(GLfloat)]) / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 4) * sizeof(GLfloat)]) / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 5) * sizeof(GLfloat)]) / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 6) * sizeof(GLfloat)]) / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = *reinterpret_cast<GLfloat*>(&inputBuffer[(i + 7) * sizeof(GLfloat)]) / size * -1 + ndcMovementY;
    }

    glUseProgram(programObjectTextRender);

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangleBuffer), triangleBuffer, GL_STATIC_DRAW);

    // FIX: query attribute from the TEXT program, not the VNC program
    GLint positionAttribute = glGetAttribLocation(programObjectTextRender, "position");
    glEnableVertexAttribArray(positionAttribute);
    glVertexAttribPointer(positionAttribute, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    glDrawArrays(GL_TRIANGLES, 0, triangleIndex);

    glDeleteBuffers(1, &vbo);
}

// ---------------- Config file helpers (unchanged) ----------------
void parseLineArray(char *line, const char *key, GLfloat *dest, int count) {
    if (strncmp(line, key, strlen(key)) == 0) {
        char *values = strchr(line, '=');
        if (values) {
            values++;
            for (int i = 0; i < count; i++) dest[i] = strtof(values, &values);
        }
    }
}
void parseLineInt(char *line, const char *key, int *dest) {
    if (strncmp(line, key, strlen(key)) == 0) {
        char *value = strchr(line, '=');
        if (value) *dest = atoi(value + 1);
    }
}
void loadConfig(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Config file not found. Using defaults.\n");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        parseLineArray(line, "landscapeVertices", landscapeVertices, 12);
        parseLineArray(line, "portraitVertices", portraitVertices, 12);
        parseLineArray(line, "landscapeTexCoords", landscapeTexCoords, 8);
        parseLineArray(line, "portraitTexCoords", portraitTexCoords, 8);
        parseLineInt(line, "windowWidth", &windowWidth);
        parseLineInt(line, "windowHeight", &windowHeight);
    }
    fclose(file);
}
void printArray(const char *label, GLfloat *array, int count, int elementsPerLine) {
    printf("%s:\n", label);
    for (int i = 0; i < count; i++) {
        printf("%f ", array[i]);
        if ((i + 1) % elementsPerLine == 0) printf("\n");
    }
    printf("\n");
}

// ---------------- VNC framebuffer update parser (PIPELINED) ----------------
char* parseFramebufferUpdate_pipelined(
    int socket_fd,
    int* frameBufferWidth,
    int* frameBufferHeight,
    z_stream* strm,
    int* finalHeight,
    FrameTimings* timings)
{
    uint64_t parseStart = now_us();

    // Server->client message header: type(1), pad(1), rectcount(2)
    char msgHdr[4];
    if (recv_exact(socket_fd, msgHdr, 4, timings) != 0) {
        return NULL;
    }

    unsigned char messageType = (unsigned char)msgHdr[0];
    int rectCount = byteArrayToInt16(msgHdr + 2);

    // --- PIPELINING: request NEXT update ASAP (after we know this is a framebuffer update) ---
    if (messageType == 0) {
        // best-effort; if it fails, we still try to decode current frame
        (void)send(socket_fd, FRAMEBUFFER_UPDATE_REQUEST, sizeof(FRAMEBUFFER_UPDATE_REQUEST), 0);
    }

    int totalLoadedSize = 0;
    char* finalFrameBuffer = (char*)malloc(1);
    if (!finalFrameBuffer) return NULL;

    int offset = 0;

    for (int i = 0; i < rectCount; i++) {
        // Rect header: x(2), y(2), w(2), h(2), encoding(4)
        char rectHdr[12];
        if (recv_exact(socket_fd, rectHdr, 12, timings) != 0) {
            free(finalFrameBuffer);
            return NULL;
        }

        int w = byteArrayToInt16(rectHdr + 4);
        int h = byteArrayToInt16(rectHdr + 6);
        int32_t encoding = byteArrayToInt32(rectHdr + 8);

        *frameBufferWidth = w;
        *frameBufferHeight = h;
        *finalHeight += h;

        if (encoding == 6) { // ZLIB encoding
            char sizeBuf[4];
            if (recv_exact(socket_fd, sizeBuf, 4, timings) != 0) {
                free(finalFrameBuffer);
                return NULL;
            }

            int compressedSize = byteArrayToInt32(sizeBuf);
            if (compressedSize <= 0) {
                free(finalFrameBuffer);
                return NULL;
            }

            char* compressedData = (char*)malloc((size_t)compressedSize);
            if (!compressedData) {
                free(finalFrameBuffer);
                return NULL;
            }

            if (recv_exact(socket_fd, compressedData, (size_t)compressedSize, timings) != 0) {
                free(compressedData);
                free(finalFrameBuffer);
                return NULL;
            }

            size_t outSize = (size_t)w * (size_t)h * 4u;
            char* decompressedData = (char*)malloc(outSize);
            if (!decompressedData) {
                free(compressedData);
                free(finalFrameBuffer);
                return NULL;
            }

            totalLoadedSize += (int)outSize;
            char* tmp = (char*)realloc(finalFrameBuffer, (size_t)totalLoadedSize);
            if (!tmp) {
                free(decompressedData);
                free(compressedData);
                free(finalFrameBuffer);
                return NULL;
            }
            finalFrameBuffer = tmp;

            strm->avail_in  = (uInt)compressedSize;
            strm->next_in   = (Bytef*)compressedData;
            strm->avail_out = (uInt)outSize;
            strm->next_out  = (Bytef*)decompressedData;

            uint64_t infStart = now_us();
            int ret = inflate(strm, Z_NO_FLUSH);
            uint64_t infEnd = now_us();
            if (timings) timings->inflate_ms += us_to_ms(infEnd - infStart);

            if (ret < 0 && ret != Z_BUF_ERROR) {
                free(decompressedData);
                free(compressedData);
                free(finalFrameBuffer);
                return NULL;
            }

            memcpy(finalFrameBuffer + offset, decompressedData, outSize);
            offset += (int)outSize;

            free(decompressedData);
            free(compressedData);
        } else {
            // Unsupported encoding in this minimal example
            // You could skip/handle RAW etc. here.
        }
    }

    uint64_t parseEnd = now_us();
    if (timings) timings->parse_ms = us_to_ms(parseEnd - parseStart);
    return finalFrameBuffer;
}

// ---------------- MAIN ----------------
int main(int argc, char* argv[])
{
    printf("QNX MOST VNC render 0.2.x (Pipelined)\n");
    printf("Loading config.txt\n");
    loadConfig("config.txt");

    printArray("Landscape vertices", landscapeVertices, 12, 3);
    printArray("Portrait vertices", portraitVertices, 12, 3);
    printArray("Landscape texture coordinates", landscapeTexCoords, 8, 2);
    printArray("Portrait texture coordinates", portraitTexCoords, 8, 2);
    printf("windowWidth = %d;\n", windowWidth);
    printf("windowHeight = %d;\n", windowHeight);

    // display_init
    void* func_handle = dlopen("libdisplayinit.so", RTLD_LAZY);
    if (!func_handle) {
        fprintf(stderr, "Error using libdisplayinit.so: %s\n", dlerror());
        return 1;
    }

    void (*display_init)(int, int) = (void (*)(int, int))dlsym(func_handle, "display_init");
    if (!display_init) {
        fprintf(stderr, "Error loading display_init: %s\n", dlerror());
        dlclose(func_handle);
        return 1;
    }

    printf("Calling display_init\n");
    display_init(0, 0);

    dlclose(func_handle);

    // EGL init
    printf("OpenGL ES2.0 initialization started\n");
    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDisplay, 0, 0);

    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    printf("Maximum OpenGL texture size supported: %d\n", maxSize);

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig* configs = new EGLConfig[5];
    EGLint num_configs = 0;
    EGLNativeWindowType windowEgl;
    int kdWindow = 0;

    if (!eglChooseConfig(eglDisplay, config_attribs, configs, 1, &num_configs)) {
        fprintf(stderr, "Error: Failed to choose EGL configuration\n");
        return 1;
    }
    eglConfig = configs[0];

    void* func_handle_display_create_window = dlopen("libdisplayinit.so", RTLD_LAZY);
    if (!func_handle_display_create_window) {
        fprintf(stderr, "Error: %s\n", dlerror());
        return 1;
    }

    void (*display_create_window)(EGLDisplay, EGLConfig, int, int, int, EGLNativeWindowType*, int*) =
        (void (*)(EGLDisplay, EGLConfig, int, int, int, EGLNativeWindowType*, int*))
        dlsym(func_handle_display_create_window, "display_create_window");

    if (!display_create_window) {
        fprintf(stderr, "Error loading display_create_window: %s\n", dlerror());
        dlclose(func_handle_display_create_window);
        return 1;
    }

    printf("display_create_window\n");
    display_create_window(eglDisplay, configs[0], windowWidth, windowHeight, 3, &windowEgl, &kdWindow);
    dlclose(func_handle_display_create_window);

    printf("eglCreateWindowSurface\n");
    eglSurface = eglCreateWindowSurface(eglDisplay, configs[0], windowEgl, 0);
    if (eglSurface == EGL_NO_SURFACE) {
        checkErrorEGL("eglCreateWindowSurface");
        fprintf(stderr, "Create surface failed\n");
        exit(EXIT_FAILURE);
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    printf("eglCreateContext\n");
    eglContext = eglCreateContext(eglDisplay, configs[0], EGL_NO_CONTEXT, context_attribs);
    if (eglContext == EGL_NO_CONTEXT) {
        checkErrorEGL("eglCreateContext");
        std::cerr << "Failed to create EGL context\n";
        return EXIT_FAILURE;
    }

    if (eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext) == EGL_FALSE) {
        std::cerr << "Failed to make EGL context current\n";
        return EXIT_FAILURE;
    }

    Init();

    // -------- Main reconnect loop --------
    for (;;) {
        printf("Main loop executed\n");
        execute_final_commands();

        int sockfd = -1;
        fd_set write_fds;
        int result = 0;
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        struct sockaddr_in serv_addr;

        int keepalive = 1;
        int keepidle  = 2;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("Error opening socket");
            usleep(200000);
            continue;
        }

        int mib[4];
        int ival = 0;

        mib[0] = CTL_NET; mib[1] = AF_INET; mib[2] = IPPROTO_TCP; mib[3] = TCPCTL_KEEPCNT;
        ival = 3;
        sysctl(mib, 4, NULL, NULL, &ival, sizeof(ival));

        mib[0] = CTL_NET; mib[1] = AF_INET; mib[2] = IPPROTO_TCP; mib[3] = TCPCTL_KEEPINTVL;
        ival = 2;
        sysctl(mib, 4, NULL, NULL, &ival, sizeof(ival));

        if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
            perror("setsockopt SO_KEEPALIVE");
            close(sockfd);
            usleep(200000);
            continue;
        }

        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPALIVE, &keepidle, sizeof(keepidle)) < 0) {
            perror("setsockopt TCP_KEEPALIVE");
            close(sockfd);
            usleep(200000);
            continue;
        }

        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags < 0) {
            perror("fcntl F_GETFL");
            close(sockfd);
            usleep(200000);
            continue;
        }
        if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("fcntl F_SETFL");
            close(sockfd);
            usleep(200000);
            continue;
        }

        memset((char*)&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        if (argc > 1) serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
        else          serv_addr.sin_addr.s_addr = inet_addr(VNC_SERVER_IP_ADDRESS);
        serv_addr.sin_port = htons(VNC_SERVER_PORT);

        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;

        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

        result = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (result < 0 && errno != EINPROGRESS) {
            perror("Error connecting to server");
            close(sockfd);
            usleep(200000);
            continue;
        }

        FD_ZERO(&write_fds);
        FD_SET(sockfd, &write_fds);
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        result = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
        if (result <= 0) {
            if (result < 0) perror("select failed");
            else printf("Connection timed out\n");
            close(sockfd);
            usleep(200000);
            continue;
        }

        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            printf("Connection failed: %s\n", strerror(so_error));
            close(sockfd);
            usleep(200000);
            continue;
        }

        // back to blocking mode
        if (fcntl(sockfd, F_SETFL, flags) < 0) {
            perror("fcntl F_SETFL");
            close(sockfd);
            usleep(200000);
            continue;
        }

        execute_initial_commands();

        // ---- VNC handshake (same behavior as your original) ----
        char serverInitMsg[12];
        if (recv_exact(sockfd, serverInitMsg, sizeof(serverInitMsg), NULL) != 0) {
            perror("recv serverInitMsg");
            close(sockfd);
            continue;
        }

        if (send(sockfd, PROTOCOL_VERSION, strlen(PROTOCOL_VERSION), 0) < 0) {
            perror("send PROTOCOL_VERSION");
            close(sockfd);
            continue;
        }

        char securityHandshake[4];
        if (recv(sockfd, securityHandshake, sizeof(securityHandshake), 0) <= 0) {
            perror("recv securityHandshake");
            close(sockfd);
            continue;
        }

        if (send(sockfd, "\x01", 1, 0) < 0) {
            perror("send ClientInit");
            close(sockfd);
            continue;
        }

        // Read framebuffer w/h (2+2), then pixel format(16) + name length(4) + name(nameLen)
        char fbWb[2], fbHb[2];
        if (recv_exact(sockfd, fbWb, 2, NULL) != 0 || recv_exact(sockfd, fbHb, 2, NULL) != 0) {
            perror("recv fb size");
            close(sockfd);
            continue;
        }

        char pixelFormat[16];
        char nameLength[4];
        if (recv_exact(sockfd, pixelFormat, 16, NULL) != 0 || recv_exact(sockfd, nameLength, 4, NULL) != 0) {
            perror("recv pixelFormat/nameLength");
            close(sockfd);
            continue;
        }

        uint32_t nameLengthInt =
            ((uint32_t)(unsigned char)nameLength[0] << 24) |
            ((uint32_t)(unsigned char)nameLength[1] << 16) |
            ((uint32_t)(unsigned char)nameLength[2] << 8)  |
            ((uint32_t)(unsigned char)nameLength[3]);

        if (nameLengthInt > 0) {
            char* name = (char*)malloc(nameLengthInt + 1);
            if (!name) { close(sockfd); continue; }
            if (recv_exact(sockfd, name, nameLengthInt, NULL) != 0) {
                free(name);
                perror("recv server name");
                close(sockfd);
                continue;
            }
            name[nameLengthInt] = 0;
            // printf("Server name: %s\n", name);
            free(name);
        }

        // Set encodings + initial update request
        if (send(sockfd, ZLIB_ENCODING, sizeof(ZLIB_ENCODING), 0) < 0) {
            perror("send ZLIB_ENCODING");
            close(sockfd);
            continue;
        }
        if (send(sockfd, FRAMEBUFFER_UPDATE_REQUEST, sizeof(FRAMEBUFFER_UPDATE_REQUEST), 0) < 0) {
            perror("send initial FRAMEBUFFER_UPDATE_REQUEST");
            close(sockfd);
            continue;
        }

        // ---- GL texture init ----
        GLuint textureID;
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // ---- zlib stream (persistent across frames) ----
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (inflateInit(&strm) != Z_OK) {
            fprintf(stderr, "inflateInit failed\n");
            close(sockfd);
            glDeleteTextures(1, &textureID);
            continue;
        }

        int framebufferWidthInt = 0;
        int framebufferHeightInt = 0;
        int finalHeight = 0;

        // FPS
        int frameCount = 0;
        double fps = 0.0;
        uint64_t lastFpsUs = now_us();

        // ---- render loop ----
        execute_initial_commands();

        for (;;) {
            uint64_t frameStartUs = now_us();
            FrameTimings timings;

            char* framebufferUpdate = parseFramebufferUpdate_pipelined(
                sockfd,
                &framebufferWidthInt,
                &framebufferHeightInt,
                &strm,
                &finalHeight,
                &timings);

            if (!framebufferUpdate) {
                perror("parseFramebufferUpdate_pipelined");
                close(sockfd);
                break;
            }

            // NOTE: no send() here anymore; it's pipelined inside the parser.

            // FPS update
            frameCount++;
            uint64_t nowUs = now_us();
            uint64_t dtUs = nowUs - lastFpsUs;
            if (dtUs >= 1000000ULL) {
                fps = (double)frameCount * 1000000.0 / (double)dtUs;
                frameCount = 0;
                lastFpsUs = nowUs;
            }

            // Render
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(programObject);

            uint64_t texStartUs = now_us();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, framebufferWidthInt, finalHeight,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, framebufferUpdate);
            uint64_t texEndUs = now_us();
            timings.texture_upload_ms = us_to_ms(texEndUs - texStartUs);

            GLint positionAttribute = glGetAttribLocation(programObject, "position");
            GLint texCoordAttrib    = glGetAttribLocation(programObject, "texCoord");

            if (framebufferWidthInt > finalHeight) {
                glVertexAttribPointer(positionAttribute, 3, GL_FLOAT, GL_FALSE, 0, landscapeVertices);
                glVertexAttribPointer(texCoordAttrib,    2, GL_FLOAT, GL_FALSE, 0, landscapeTexCoords);
            } else {
                glVertexAttribPointer(positionAttribute, 3, GL_FLOAT, GL_FALSE, 0, portraitVertices);
                glVertexAttribPointer(texCoordAttrib,    2, GL_FLOAT, GL_FALSE, 0, portraitTexCoords);
            }

            glEnableVertexAttribArray(positionAttribute);
            glEnableVertexAttribArray(texCoordAttrib);

            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

            glDisableVertexAttribArray(positionAttribute);
            glDisableVertexAttribArray(texCoordAttrib);

            // Optional on-screen stats
            timings.total_frame_ms = us_to_ms(now_us() - frameStartUs);
            char overlay[256];
            snprintf(overlay, sizeof(overlay),
                     "FPS %.1f\nFrame %.2fms\nRecv %.2fms\nInflate %.2fms\nParse %.2fms\nGPU %.2fms",
                     fps,
                     timings.total_frame_ms,
                     timings.recv_ms,
                     timings.inflate_ms,
                     timings.parse_ms,
                     timings.texture_upload_ms);
            print_string(-320, 220, overlay, 1, 1, 1, 64);

            eglSwapBuffers(eglDisplay, eglSurface);

            finalHeight = 0;
            free(framebufferUpdate);
        }

        inflateEnd(&strm);
        glDeleteTextures(1, &textureID);
        execute_final_commands();
    }

    // Cleanup (never reached with the infinite loop, kept for completeness)
    eglSwapBuffers(eglDisplay, eglSurface);
    eglDestroySurface(eglDisplay, eglSurface);
    eglDestroyContext(eglDisplay, eglContext);
    eglTerminate(eglDisplay);
    execute_final_commands();
    return EXIT_SUCCESS;
}
