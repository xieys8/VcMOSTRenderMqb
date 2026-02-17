#include <algorithm> // For std::min
#include <codecvt>
#include <EGL/egl.h>
#include <filesystem>
#include <GLES2/gl2.h>
#include <iostream>
#include <locale>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <chrono> // Added for timing
#include <vector>
#include <iomanip>

#define STB_TRUETYPE_IMPLEMENTATION  // force following include to generate implementation
#include "stb_easyfont.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <fcntl.h>
#include "miniz.h"
#include <time.h>

// --- Timing Structures ---
using Clock = std::chrono::high_resolution_clock;

struct FrameTimings {
    double recv_ms = 0.0;       // Time waiting for network data
    double inflate_ms = 0.0;    // Time spent in zlib inflate
    double parse_ms = 0.0;      // Total time in parseFramebufferUpdate
    double texture_upload_ms = 0.0; // Time to upload to GPU
    double total_frame_ms = 0.0; // End-to-end frame loop time
};

static double GetDurationMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Wrapper to measure recv time
int recv_timed(SOCKET s, char* buf, int len, int flags, FrameTimings& timings) {
    auto start = Clock::now();
    int result = recv(s, buf, len, flags);
    auto end = Clock::now();
    timings.recv_ms += GetDurationMs(start, end);
    return result;
}

// --- GLES setup ---
GLuint programObject;
GLuint programObjectTextRender;
EGLDisplay eglDisplay;
EGLConfig eglConfig;
EGLSurface eglSurface;
EGLContext eglContext;

// --- VNC shaders ---
const char* vertexShaderSource =
"#version 100\n"
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
"#version 100\n"
"precision highp float;\n"
"varying vec2 v_texCoord;\n"
"uniform sampler2D texture;\n"
"void main()\n"
"{\n"
"    gl_FragColor = texture2D(texture, v_texCoord);\n"
"}\n";

// --- Text Rendering shaders ---
const char* vertexShaderSourceText =
"attribute vec2 position;    \n"
"void main()                  \n"
"{                            \n"
"   gl_Position = vec4(position, 0.0, 1.0); \n"
"   gl_PointSize = 4.0;      \n"
"}                            \n";

const char* fragmentShaderSourceText =
"void main()               \n"
"{                         \n"
"  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0); \n"
"}                         \n";

// --- Geometry ---
GLfloat landscapeVertices[] = {
   -0.8f,  0.73, 0.0f,  // Top Left
    0.8f,  0.73f, 0.0f, // Top Right
    0.8f, -0.63f, 0.0f, // Bottom Right
   -0.8f, -0.63f, 0.0f  // Bottom Left
};
GLfloat portraitVertices[] = {
   -0.8f,  1.0f, 0.0f,  // Top Left
    0.8f,  1.0f, 0.0f,  // Top Right
    0.8f, -0.67f, 0.0f, // Bottom Right
   -0.8f, -0.67f, 0.0f  // Bottom Left
};
GLfloat landscapeTexCoords[] = {
    0.0f, 0.07f, // Bottom Left
    0.90f, 0.07f,// Bottom Right
    0.90f, 1.0f, // Top Right
    0.0f, 1.0f   // Top Left
};
GLfloat portraitTexCoords[] = {
    0.0f, 0.0f,  // Bottom Left
    0.63f, 0.0f, // Bottom Right
    0.63f, 0.2f, // Top Right
    0.0f, 0.2f   // Top Left
};

// --- Constants ---
const char* PROTOCOL_VERSION = "RFB 003.003\n";
const char FRAMEBUFFER_UPDATE_REQUEST[] = { 3,0,0,0,0,0,255,255,255,255 };
const char CLIENT_INIT[] = { 1 };
const char ZLIB_ENCODING[] = { 2,0,0,2,0,0,0,6,0,0,0,0 };

// --- Setup ---
int windowWidth = 800;
int windowHeight = 480;

const char* VNC_SERVER_IP_ADDRESS = "192.168.1.199";
const int VNC_SERVER_PORT = 5900;

const char* EXLAP_SERVER_IP_ADDRESS = "127.0.0.1";
const int EXLAP_SERVER_PORT = 25010;

// --- Windows Helpers ---
static void usleep(__int64 usec) {
    HANDLE timer;
    LARGE_INTEGER ft;
    ft.QuadPart = -(10 * usec);
    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

static void BuildSockAddr(SOCKADDR* const SockAddr, char const* const IPAddress, WORD const Port) {
    SOCKADDR_IN* const SIn = (SOCKADDR_IN*)SockAddr;
    SIn->sin_family = AF_INET;
    inet_pton(AF_INET, IPAddress, &SIn->sin_addr);
    SIn->sin_port = htons(Port);
}

SOCKET MySocketOpen(int const Type, WORD const Port) {
    int Protocol = (Type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
    SOCKET Socket = WSASocket(AF_INET, Type, Protocol, NULL, 0, 0);
    if (Socket != INVALID_SOCKET) {
        SOCKADDR SockAddr = { 0 };
        BuildSockAddr(&SockAddr, NULL, Port);
        if (bind(Socket, &SockAddr, sizeof(SockAddr)) != 0) {
            closesocket(Socket);
            Socket = INVALID_SOCKET;
        }
    }
    return Socket;
}

// --- Shader Compiler ---
GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "Shader compilation error: " << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

std::string readPersistanceData(const std::string& position) {
#ifdef _WIN32
    return "0"; // Mock data on Windows to avoid hanging if pipe fails
#else
    std::string command = "on -f mmx /net/mmx/mnt/app/eso/bin/apps/pc " + position;
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) return "0";
    char buffer[128];
    std::string result = "";
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != NULL) result += buffer;
    }
    _pclose(pipe);
    return result;
#endif
}

int16_t byteArrayToInt16(const char* byteArray) {
    return ((int16_t)(byteArray[0] & 0xFF) << 8) | (byteArray[1] & 0xFF);
}

int32_t byteArrayToInt32(const char* byteArray) {
    return ((int32_t)(byteArray[0] & 0xFF) << 24) | ((int32_t)(byteArray[1] & 0xFF) << 16) | ((int32_t)(byteArray[2] & 0xFF) << 8) | (byteArray[3] & 0xFF);
}

void Init() {
    // Compile VNC Shaders
    GLuint vsVnc = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fsVnc = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    programObject = glCreateProgram();
    glAttachShader(programObject, vsVnc);
    glAttachShader(programObject, fsVnc);
    glLinkProgram(programObject);

    // Compile Text Shaders
    GLuint vsText = compileShader(GL_VERTEX_SHADER, vertexShaderSourceText);
    GLuint fsText = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSourceText);
    programObjectTextRender = glCreateProgram();
    glAttachShader(programObjectTextRender, vsText);
    glAttachShader(programObjectTextRender, fsText);
    glLinkProgram(programObjectTextRender);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

// --- Modified Parser with Timing & Pipelining ---
char* parseFramebufferUpdate(SOCKET socket_fd, int* frameBufferWidth, int* frameBufferHeight, z_stream* strm, int* finalHeight, FrameTimings& timings) {
    auto parseStart = Clock::now();

    // Read message-type (1 byte)
    char messageType[1];
    if (recv_timed(socket_fd, messageType, 1, MSG_WAITALL, timings) <= 0) return NULL;

    // Read padding (1 byte)
    char padding[1];
    if (recv_timed(socket_fd, padding, 1, MSG_WAITALL, timings) <= 0) return NULL;

    // Read number-of-rectangles (2 bytes)
    char numberOfRectangles[2];
    if (recv_timed(socket_fd, numberOfRectangles, 2, MSG_WAITALL, timings) <= 0) return NULL;

    // --- PIPELINING OPTIMIZATION ---
    // Send request for the NEXT frame immediately after receiving the header for this one.
    // Server processes next frame while we are busy decoding this one.
    // Note: Protocol allows multiple outstanding requests.
    if (messageType[0] == 0) { // Only for FramebufferUpdate (type 0)
        // Request full screen update (0,0, w,h) or whatever strategy you use
        // Using the const char array defined globally
        // NOTE: Make sure FRAMEBUFFER_UPDATE_REQUEST is accessible here or pass it in.
        // It's a global constant in your code so it works.
        const char REQ[] = { 3,0,0,0,0,0,255,255,255,255 };
        send(socket_fd, REQ, sizeof(REQ), 0);
    }
    // -------------------------------

    int totalLoadedSize = 0;
    char* finalFrameBuffer = (char*)malloc(1);
    int offset = 0;
    int rectCount = byteArrayToInt16(numberOfRectangles);

    for (int i = 0; i < rectCount; i++) {
        char header[12]; // x(2), y(2), w(2), h(2), enc(4)
        if (recv_timed(socket_fd, header, 12, MSG_WAITALL, timings) != 12) {
            free(finalFrameBuffer);
            return NULL;
        }

        *frameBufferWidth = byteArrayToInt16(header + 4);
        *frameBufferHeight = byteArrayToInt16(header + 6);
        *finalHeight = *finalHeight + *frameBufferHeight;
        char encodingType = header[11]; // 4th byte of encoding S32

        if (encodingType == 6) { // ZLIB encoding
            char compressedSizeBuf[4];
            if (recv_timed(socket_fd, compressedSizeBuf, 4, MSG_WAITALL, timings) != 4) {
                free(finalFrameBuffer);
                return NULL;
            }

            int compressedSize = byteArrayToInt32(compressedSizeBuf);
            char* compressedData = (char*)malloc(compressedSize);

            if (recv_timed(socket_fd, compressedData, compressedSize, MSG_WAITALL, timings) != compressedSize) {
                free(compressedData);
                free(finalFrameBuffer);
                return NULL;
            }

            // Allocation for decompression
            char* decompressedData = (char*)malloc(*frameBufferWidth * *frameBufferHeight * 4);

            totalLoadedSize += (*frameBufferWidth * *frameBufferHeight * 4);
            char* temp = (char*)realloc(finalFrameBuffer, totalLoadedSize);
            if (!temp) { free(compressedData); free(decompressedData); free(finalFrameBuffer); return NULL; }
            finalFrameBuffer = temp;

            // Decompress
            // Use pointer strm* now
            strm->avail_in = compressedSize;
            strm->next_in = (Bytef*)compressedData;
            strm->avail_out = *frameBufferWidth * *frameBufferHeight * 4;
            strm->next_out = (Bytef*)decompressedData;

            auto infStart = Clock::now();
            int ret = inflate(strm, Z_NO_FLUSH);
            timings.inflate_ms += GetDurationMs(infStart, Clock::now());

            if (ret < 0 && ret != Z_BUF_ERROR) {
                inflateEnd(strm); // Use pointer
                free(decompressedData);
                free(compressedData);
                free(finalFrameBuffer);
                return NULL;
            }

            memcpy(finalFrameBuffer + offset, decompressedData, static_cast<size_t>(*frameBufferWidth) * *frameBufferHeight * 4);
            offset += (*frameBufferWidth * *frameBufferHeight * 4);

            free(compressedData);
            free(decompressedData);
        }
    }

    timings.parse_ms = GetDurationMs(parseStart, Clock::now());
    return finalFrameBuffer;
}

// --- Text Rendering ---
void print_string(float x, float y, const char* text, float r, float g, float b, float size) {
    char inputBuffer[20000] = { 0 };
    GLfloat triangleBuffer[20000] = { 0 };

    int number = stb_easy_font_print(0, 0, text, NULL, inputBuffer, sizeof(inputBuffer));

    float ndcMovementX = (2.0f * x) / windowWidth;
    float ndcMovementY = (2.0f * y) / windowHeight;

    int triangleIndex = 0;
    for (int i = 0; i < sizeof(inputBuffer) / sizeof(GLfloat); i += 8) {
        GLfloat* ptr = reinterpret_cast<GLfloat*>(&inputBuffer[i * sizeof(GLfloat)]);
        if (ptr[0] == 0 && ptr[1] == 0 && ptr[2] == 0) break; // End of data

        triangleBuffer[triangleIndex++] = ptr[0] / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = ptr[1] / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = ptr[2] / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = ptr[3] / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = ptr[4] / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = ptr[5] / size * -1 + ndcMovementY;

        triangleBuffer[triangleIndex++] = ptr[0] / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = ptr[1] / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = ptr[4] / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = ptr[5] / size * -1 + ndcMovementY;
        triangleBuffer[triangleIndex++] = ptr[6] / size + ndcMovementX;
        triangleBuffer[triangleIndex++] = ptr[7] / size * -1 + ndcMovementY;
    }

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangleBuffer), triangleBuffer, GL_STATIC_DRAW);

    GLint positionAttribute = glGetAttribLocation(programObjectTextRender, "position");
    glEnableVertexAttribArray(positionAttribute);
    glVertexAttribPointer(positionAttribute, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    glDrawArrays(GL_TRIANGLES, 0, triangleIndex / 2); // 2 floats per vertex
    glDeleteBuffers(1, &vbo);
}

void loadConfig(const char* filename) {
    FILE* file = NULL;
    if (fopen_s(&file, filename, "r") != 0 || !file) return;
    fclose(file);
}

// --- Main ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OpenGL_VNC_Sim";
    wc.style = CS_OWNDC;
    RegisterClass(&wc);

    loadConfig("config.txt");

    HWND hWnd = CreateWindowEx(0, L"OpenGL_VNC_Sim", L"OpenGL VNC Render (Stats Enabled)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, windowHeight,
        nullptr, nullptr, hInstance, nullptr);

    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    eglInitialize(eglDisplay, &maj, &min);

    EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig eglCfg;
    EGLint numConfigs;
    eglChooseConfig(eglDisplay, config_attribs, &eglCfg, 1, &numConfigs);

    eglSurface = eglCreateWindowSurface(eglDisplay, eglCfg, hWnd, nullptr);
    EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    eglContext = eglCreateContext(eglDisplay, eglCfg, EGL_NO_CONTEXT, ctxAttribs);
    eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

    Init();

    while (true) {
        WSADATA WSAData;
        WSAStartup(0x202, &WSAData);
        SOCKET sockfd = MySocketOpen(SOCK_STREAM, 0);

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        inet_pton(AF_INET, VNC_SERVER_IP_ADDRESS, &serverAddr.sin_addr);
        serverAddr.sin_port = htons(VNC_SERVER_PORT);

        struct timeval timeout = { 5, 0 }; // 5s timeout
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

        if (connect(sockfd, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "Connecting..." << std::endl;
            closesocket(sockfd);
            WSACleanup();
            Sleep(500);
            continue;
        }

        // Handshake
        char buf[256];
        recv(sockfd, buf, 12, MSG_WAITALL);
        send(sockfd, PROTOCOL_VERSION, strlen(PROTOCOL_VERSION), 0);
        recv(sockfd, buf, 4, 0);
        send(sockfd, "\\x01", 1, 0);
        recv(sockfd, buf, 4, 0);
        recv(sockfd, buf, 16 + 4, MSG_WAITALL);
        uint32_t nameLen = (unsigned char)buf[16] << 24 | (unsigned char)buf[17] << 16 | (unsigned char)buf[18] << 8 | (unsigned char)buf[19];
        recv(sockfd, buf, nameLen, MSG_WAITALL);

        send(sockfd, ZLIB_ENCODING, sizeof(ZLIB_ENCODING), 0);

        // Initial Request
        send(sockfd, FRAMEBUFFER_UPDATE_REQUEST, sizeof(FRAMEBUFFER_UPDATE_REQUEST), 0);

        int fbW = 0, fbH = 0, finalH = 0;
        int frameCount = 0;
        double fps = 0.0;
        auto lastFpsTime = Clock::now();

        GLuint textureID;
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        z_stream strm = { 0 };
        inflateInit(&strm);

        MSG msg;
        bool running = true;

        while (running) {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running = false;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (!running) break;

            auto frameStart = Clock::now();
            FrameTimings timings = { 0 };

            // 1. Receive and Decode
            // Pass &strm now to use persistent pointer if you modified it, 
            // or pass strm by value if you didn't change the signature.
            // CAUTION: In your original code 'strm' was passed by value copy!
            // To make pipelining robust, you generally want to pass by pointer so internal state
            // tracks correctly if you process partials. 
            // I changed the signature to `z_stream* strm` in the function above.
            char* fbData = parseFramebufferUpdate(sockfd, &fbW, &fbH, &strm, &finalH, timings);

            if (!fbData) {
                closesocket(sockfd);
                break;
            }

            // REMOVED THE SEND() CALL FROM HERE (It's now inside parseFramebufferUpdate)
            // if (send(sockfd, FRAMEBUFFER_UPDATE_REQUEST, ...) < 0) { ... }

            // 3. Clear Screen
            glClear(GL_COLOR_BUFFER_BIT);

            // 4. Upload Texture
            glUseProgram(programObject);
            auto texStart = Clock::now();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbW, finalH, 0, GL_RGBA, GL_UNSIGNED_BYTE, fbData);
            timings.texture_upload_ms = GetDurationMs(texStart, Clock::now());

            // 5. Draw VNC Quad
            GLint posAttr = glGetAttribLocation(programObject, "position");
            GLint texAttr = glGetAttribLocation(programObject, "texCoord");

            if (fbW > finalH) {
                glVertexAttribPointer(posAttr, 3, GL_FLOAT, GL_FALSE, 0, landscapeVertices);
                glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, 0, landscapeTexCoords);
            }
            else {
                glVertexAttribPointer(posAttr, 3, GL_FLOAT, GL_FALSE, 0, portraitVertices);
                glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, 0, portraitTexCoords);
            }
            glEnableVertexAttribArray(posAttr);
            glEnableVertexAttribArray(texAttr);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            glDisableVertexAttribArray(posAttr);
            glDisableVertexAttribArray(texAttr);

            // 6. Stats & Overlay
            frameCount++;
            auto now = Clock::now();
            timings.total_frame_ms = GetDurationMs(frameStart, now);

            if (GetDurationMs(lastFpsTime, now) >= 1000.0) {
                fps = frameCount * 1000.0 / GetDurationMs(lastFpsTime, now);
                frameCount = 0;
                lastFpsTime = now;
            }

            char overlayText[512];
            snprintf(overlayText, sizeof(overlayText),
                "FPS: %.1f\nFrame: %.2f ms\nRecv: %.2f ms\nInflate: %.2f ms\nParse: %.2f ms\nGPU Up: %.2f ms",
                fps, timings.total_frame_ms, timings.recv_ms, timings.inflate_ms, timings.parse_ms, timings.texture_upload_ms);

            glUseProgram(programObjectTextRender);
            print_string(-380, 220, overlayText, 1.0f, 1.0f, 0.0f, 80.0f);

            eglSwapBuffers(eglDisplay, eglSurface);

            finalH = 0;
            free(fbData);
        }

        closesocket(sockfd);
        WSACleanup();
        glDeleteTextures(1, &textureID);
    }
    return 0;
}
