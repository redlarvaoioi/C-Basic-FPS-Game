/*
 Minimal sandbox FPS in C++ targeting Emscripten/WebGL2.

 Build (in Codespaces devcontainer after emsdk is installed):
   emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j

 Serve build/ (python3 -m http.server 8000) and open sandbox_fps.html in browser preview.

 No external assets required.
*/

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>

#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

// ----------------- Minimal math (vec3, mat4) -----------------
struct Vec3 {
    float x,y,z;
    Vec3():x(0),y(0),z(0){}
    Vec3(float X,float Y,float Z):x(X),y(Y),z(Z){}
    Vec3 operator+(const Vec3& o) const { return Vec3(x+o.x,y+o.y,z+o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x-o.x,y-o.y,z-o.z); }
    Vec3 operator*(float s) const { return Vec3(x*s,y*s,z*s); }
};
inline float dot(const Vec3& a, const Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 cross(const Vec3& a,const Vec3& b){ return Vec3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
inline float length(const Vec3& v){ return sqrtf(dot(v,v)); }
inline Vec3 normalize(const Vec3& v){ float l=length(v); return l>0? v*(1.0f/l) : Vec3(0,0,0); }

struct Mat4 {
    float m[16];
    static Mat4 identity() {
        Mat4 r; memset(r.m,0,sizeof(r.m));
        r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.0f;
        return r;
    }
};

// column-major helpers
Mat4 perspective(float fovy, float aspect, float nearv, float farv){
    Mat4 o; memset(o.m,0,sizeof(o.m));
    float f = 1.0f / tanf(fovy*0.5f);
    o.m[0] = f / aspect;
    o.m[5] = f;
    o.m[10] = (farv + nearv) / (nearv - farv);
    o.m[11] = -1.0f;
    o.m[14] = (2.0f * farv * nearv) / (nearv - farv);
    return o;
}

Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up){
    Vec3 f = normalize(center - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 m = Mat4::identity();
    m.m[0] = s.x; m.m[4] = s.y; m.m[8]  = s.z;
    m.m[1] = u.x; m.m[5] = u.y; m.m[9]  = u.z;
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z;
    m.m[12] = -dot(s, eye);
    m.m[13] = -dot(u, eye);
    m.m[14] = dot(f, eye);
    return m;
}

// ----------------- Simple GL helpers -----------------
GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar buf[1024]; glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        printf("Shader compile error: %s\n", buf);
    }
    return s;
}
GLuint linkProgram(GLuint v, GLuint f) {
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar buf[1024]; glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        printf("Program link error: %s\n", buf);
    }
    return p;
}

// Simple vertex + fragment shader (colored)
const char* vertexSrc = R"(
attribute vec3 aPos;
attribute vec3 aColor;
uniform mat4 uMVP;
varying vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* fragSrc = R"(
precision mediump float;
varying vec3 vColor;
void main(){
    gl_FragColor = vec4(vColor, 1.0);
}
)";

// ----------------- Cube mesh (one unit cube centered on origin) -----------------
static const float cubeVerts[] = {
    // positions        // color per-vertex (we will ignore per-vertex and use per-instance color via attribute)
    -0.5f,-0.5f,-0.5f,  0.8f,0.8f,0.8f,
     0.5f,-0.5f,-0.5f,  0.8f,0.8f,0.8f,
     0.5f, 0.5f,-0.5f,  0.8f,0.8f,0.8f,
    -0.5f, 0.5f,-0.5f,  0.8f,0.8f,0.8f,
    -0.5f,-0.5f, 0.5f,  0.6f,0.6f,0.9f,
     0.5f,-0.5f, 0.5f,  0.6f,0.6f,0.9f,
     0.5f, 0.5f, 0.5f,  0.6f,0.6f,0.9f,
    -0.5f, 0.5f, 0.5f,  0.6f,0.6f,0.9f
};
static const unsigned short cubeIdx[] = {
    0,1,2, 2,3,0, // back
    4,5,6, 6,7,4, // front
    0,4,7, 7,3,0, // left
    1,5,6, 6,2,1, // right
    3,2,6, 6,7,3, // top
    0,1,5, 5,4,0  // bottom
};

// ----------------- World (grid of cubes) -----------------
struct Block {
    int gx, gz; // grid coordinates
    int h;      // height (stacked cubes)
};

std::vector<Block> blocks;
int GRID_W = 32;
int GRID_H = 32;
int MAX_STACK = 4;
float BLOCK_SIZE = 1.0f;

// Procedural generation: simple island-like falloff and noise
static float pseudoNoise(int x, int z) {
    int n = x + z * 57;
    n = (n<<13) ^ n;
    return (1.0f - float((n*(n*n*15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f);
}
void generateWorld() {
    blocks.clear();
    int cx = GRID_W/2, cz = GRID_H/2;
    float radius = std::min(GRID_W, GRID_H) * 0.45f;
    for (int z=0; z<GRID_H; ++z) for (int x=0; x<GRID_W; ++x) {
        float dx = (x - cx), dz = (z - cz);
        float d = sqrtf(dx*dx + dz*dz);
        float mask = 1.0f - (d / radius);
        if (mask <= 0.0f) continue;
        float n = pseudoNoise(x*3, z*3) * 0.6f + pseudoNoise(x*7, z*7) * 0.4f;
        float v = mask * (0.5f + n*0.5f);
        int h = int(floorf(v * MAX_STACK + 0.001f));
        if (h > 0) blocks.push_back({x,z,h});
    }
}

// ----------------- Player -----------------
Vec3 playerPos(0.0f, 1.8f, 0.0f); // x,z,y where y is up (we'll use x,z for plane coords and y for height)
float yaw = 0.0f; // rotation around up
float pitch = 0.0f;
Vec3 playerVel(0,0,0);
bool onGround = false;

// Input state
bool keyW=false,keyA=false,keyS=false,keyD=false, keySpace=false;
double mouseX=0, mouseY=0;
bool pointerLocked = false;
int canvasWidth=1280, canvasHeight=720;

// Shooting / world interaction
void raycastShoot() {
    // Ray origin at eye
    Vec3 eye(playerPos.x, playerPos.y, playerPos.z);
    Vec3 forward(cosf(yaw)*cosf(pitch), sinf(pitch), sinf(yaw)*cosf(pitch));
    forward = normalize(forward);
    // Step along ray and detect block intersection
    float maxDist = 30.0f;
    float step = 0.1f;
    for (float t=0.0f; t<maxDist; t += step) {
        Vec3 p = eye + forward * t;
        // convert world x,z to grid coords
        int gx = int(roundf(p.x / BLOCK_SIZE)) + GRID_W/2;
        int gz = int(roundf(p.z / BLOCK_SIZE)) + GRID_H/2;
        if (gx < 0 || gx >= GRID_W || gz < 0 || gz >= GRID_H) continue;
        // find a block at (gx,gz) with any height hitting p.y
        for (auto it = blocks.begin(); it != blocks.end(); ++it) {
            if (it->gx == gx && it->gz == gz) {
                // block base height is 0; block top is it->h * BLOCK_SIZE
                float hTop = it->h * BLOCK_SIZE;
                if (p.y >= 0.0f && p.y <= hTop + 0.5f) {
                    // remove block entirely
                    blocks.erase(it);
                    return;
                }
            }
        }
    }
}

// ----------------- Collision (capsule vs stacked cubes) -----------------
void resolveCollisions(float dt) {
    // simple: for each block, for each cube level, treat AABB and push player out if overlapping in x/z and y.
    float radius = 0.25f;
    for (const Block& b : blocks) {
        for (int level=0; level < b.h; ++level) {
            Vec3 minp( (b.gx - GRID_W/2 - 0.5f) * BLOCK_SIZE,
                       level * BLOCK_SIZE,
                       (b.gz - GRID_H/2 - 0.5f) * BLOCK_SIZE );
            Vec3 maxp = minp + Vec3(BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
            // player capsule center
            Vec3 pc(playerPos.x, playerPos.y-0.9f, playerPos.z); // approximate foot-level
            // clamp pc to AABB
            float cx = std::max(minp.x, std::min(pc.x, maxp.x));
            float cy = std::max(minp.y, std::min(pc.y, maxp.y));
            float cz = std::max(minp.z, std::min(pc.z, maxp.z));
            Vec3 closest(cx,cy,cz);
            Vec3 diff = pc - closest;
            float dist = length(diff);
            if (dist < radius) {
                // push player out along horizontal plane mostly
                Vec3 push = normalize(Vec3(diff.x, 0.0f, diff.z)) * (radius - dist + 0.001f);
                if (isnan(push.x) || isnan(push.y) || isnan(push.z)) {
                    // degenerate; separate vertically
                    push = Vec3(0, (radius-dist)+0.001f, 0);
                }
                playerPos.x += push.x;
                playerPos.z += push.z;
                // if push y positive and small, set onGround
                if (playerPos.y <= maxp.y + 0.01f) {
                    onGround = true;
                    playerVel.y = 0.0f;
                    playerPos.y = maxp.y + 1.8f; // stand on top of cube
                }
            }
        }
    }
}

// ----------------- GL program and buffers -----------------
GLuint prog=0;
GLint locMVP= -1;
GLuint vbo=0, ibo=0, vao=0;
void setupGL() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    prog = linkProgram(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);
    locMVP = glGetUniformLocation(prog, "uMVP");

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVerts), cubeVerts, GL_STATIC_DRAW);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(cubeIdx), cubeIdx, GL_STATIC_DRAW);
}

// Utility to draw a box at world position with scale and color
void drawCubeInstance(const Mat4& vp, const Vec3& pos, float scale, const Vec3& color) {
    glUseProgram(prog);
    // Build model matrix (translate then scale)
    Mat4 model = Mat4::identity();
    // column-major manual
    model.m[0] = scale; model.m[5] = scale; model.m[10] = scale;
    model.m[12] = pos.x;
    model.m[13] = pos.y;
    model.m[14] = pos.z;
    // mvp = vp * model
    Mat4 mvp; memset(mvp.m,0,sizeof(mvp.m));
    for (int r=0;r<4;++r) for(int c=0;c<4;++c) {
        float sum = 0.0f;
        for (int k=0;k<4;++k) sum += vp.m[k*4 + r] * model.m[c*4 + k];
        mvp.m[c*4 + r] = sum;
    }
    glUniformMatrix4fv(locMVP, 1, GL_FALSE, mvp.m);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    GLint aPos = glGetAttribLocation(prog, "aPos");
    GLint aColor = glGetAttribLocation(prog, "aColor");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, sizeof(float)*6, (void*)(0));
    glEnableVertexAttribArray(aColor);
    glVertexAttribPointer(aColor, 3, GL_FLOAT, GL_FALSE, sizeof(float)*6, (void*)(sizeof(float)*3));
    // Draw
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
    glDisableVertexAttribArray(aPos);
    glDisableVertexAttribArray(aColor);
}

// ----------------- Main loop -----------------
double lastTime = 0.0;
void main_loop();

EM_BOOL mouse_move_cb(int eventType, const EmscriptenMouseEvent* e, void* userData) {
    if (!pointerLocked) return EM_TRUE;
    // relative movement
    double dx = e->movementX;
    double dy = e->movementY;
    const float sensitivity = 0.0025f;
    yaw += dx * sensitivity;
    pitch -= dy * sensitivity;
    if (pitch > 1.4f) pitch = 1.4f;
    if (pitch < -1.4f) pitch = -1.4f;
    return EM_TRUE;
}
EM_BOOL mouse_click_cb(int eventType, const EmscriptenMouseEvent* e, void* userData) {
    // Left click shoots (button 0)
    if (e->button == 0) {
        raycastShoot();
    }
    // Request pointer lock on canvas
    if (!pointerLocked) {
        emscripten_request_pointerlock("#canvas", EM_FALSE);
    }
    return EM_TRUE;
}
EM_BOOL pointerlockchange(int eventType, const EmscriptenPointerlockChangeEvent* e, void* userData) {
    pointerLocked = e->isActive;
    return EM_TRUE;
}
EM_BOOL key_cb(int eventType, const EmscriptenKeyboardEvent* e, void* userData) {
    bool down = (eventType == EMSCRIPTEN_EVENT_KEYDOWN);
    if (strcmp(e->key, "w")==0 || strcmp(e->key, "W")==0) keyW = down;
    if (strcmp(e->key, "a")==0 || strcmp(e->key, "A")==0) keyA = down;
    if (strcmp(e->key, "s")==0 || strcmp(e->key, "S")==0) keyS = down;
    if (strcmp(e->key, "d")==0 || strcmp(e->key, "D")==0) keyD = down;
    if (strcmp(e->key, " " )==0) keySpace = down;
    if (strcmp(e->key, "r")==0 || strcmp(e->key, "R")==0) {
        if (down) {
            // respawn
            generateWorld();
            playerPos = Vec3(0.0f, 1.8f, 0.0f);
            playerVel = Vec3(0,0,0);
        }
    }
    return EM_TRUE;
}

void main_loop() {
    double now = emscripten_get_now() * 0.001;
    float dt = float(now - lastTime);
    if (dt <= 0 || dt > 0.05f) dt = 1.0f/60.0f;
    lastTime = now;

    // Physics
    Vec3 forward(cosf(yaw)*cosf(pitch), sinf(pitch), sinf(yaw)*cosf(pitch));
    Vec3 right(-sinf(yaw), 0.0f, cosf(yaw));
    forward = normalize(forward);
    right = normalize(right);
    Vec3 moveDir(0,0,0);
    if (keyW) moveDir = moveDir + forward;
    if (keyS) moveDir = moveDir - forward;
    if (keyA) moveDir = moveDir - right;
    if (keyD) moveDir = moveDir + right;
    moveDir.y = 0.0f;
    if (length(moveDir) > 0.01f) moveDir = normalize(moveDir);

    float speed = 5.0f;
    playerVel.x = moveDir.x * speed;
    playerVel.z = moveDir.z * speed;

    // gravity
    playerVel.y += -9.8f * dt;
    if (keySpace && onGround) { playerVel.y = 6.0f; onGround=false; }

    // integrate
    playerPos.x += playerVel.x * dt;
    playerPos.y += playerVel.y * dt;
    playerPos.z += playerVel.z * dt;

    // collisions
    onGround = false;
    resolveCollisions(dt);

    // ground plane
    if (playerPos.y < 1.0f) { playerPos.y = 1.0f; playerVel.y = 0.0f; onGround = true; }

    // Rendering
    glViewport(0,0,canvasWidth,canvasHeight);
    glClearColor(0.53f, 0.81f, 0.92f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    Mat4 proj = perspective(60.0f * (3.14159265f/180.0f), float(canvasWidth)/float(canvasHeight), 0.1f, 200.0f);
    Vec3 eye(playerPos.x, playerPos.y+0.5f, playerPos.z);
    Vec3 center = eye + forward;
    Mat4 view = lookAt(eye, center, Vec3(0,1,0));
    // vp = proj * view
    Mat4 vp; memset(vp.m,0,sizeof(vp.m));
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
        float sum=0.0f;
        for (int k=0;k<4;++k) sum += proj.m[k*4 + r] * view.m[c*4 + k];
        vp.m[c*4 + r] = sum;
    }

    // draw blocks
    for (const Block& b : blocks) {
        for (int level=0; level < b.h; ++level) {
            Vec3 pos( (b.gx - GRID_W/2) * BLOCK_SIZE,
                      level * BLOCK_SIZE + BLOCK_SIZE*0.5f,
                      (b.gz - GRID_H/2) * BLOCK_SIZE );
            // color varies with height
            Vec3 color(0.2f + 0.08f*level, 0.6f - 0.05f*level, 0.2f);
            drawCubeInstance(vp, pos, BLOCK_SIZE, color);
        }
    }

    // simple crosshair - use HTML overlay via JS
    EM_ASM({
        let el = document.getElementById('crosshair');
        if (!el) {
            el = document.createElement('div');
            el.id = 'crosshair';
            el.style.position = 'absolute';
            el.style.left = '50%';
            el.style.top = '50%';
            el.style.width = '10px';
            el.style.height = '10px';
            el.style.marginLeft = '-5px';
            el.style.marginTop = '-5px';
            el.style.borderLeft = '2px solid rgba(0,0,0,0.8)';
            el.style.borderTop = '2px solid rgba(0,0,0,0.8)';
            el.style.pointerEvents = 'none';
            document.body.appendChild(el);
        }
    });
}

// ----------------- Initialization -----------------
int main() {
    srand((unsigned)time(NULL));
    generateWorld();
    // create GL context on default canvas (#canvas)
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.alpha = EM_FALSE;
    attr.depth = EM_TRUE;
    attr.stencil = EM_FALSE;
    attr.antialias = EM_TRUE;
    attr.majorVersion = 2; // WebGL2
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &attr);
    if (ctx <= 0) {
        printf("Failed to create WebGL context\n");
        return 1;
    }
    EMSCRIPTEN_RESULT r = emscripten_webgl_make_context_current(ctx);
    if (r != EMSCRIPTEN_RESULT_SUCCESS) {
        printf("Failed to make context current\n");
        return 1;
    }

    // get canvas size
    emscripten_get_canvas_element_size("#canvas", &canvasWidth, &canvasHeight);

    setupGL();

    // set up input callbacks
    emscripten_set_mousemove_callback("#canvas", nullptr, EM_TRUE, mouse_move_cb);
    emscripten_set_mousedown_callback("#canvas", nullptr, EM_TRUE, mouse_click_cb);
    emscripten_set_pointerlockchange_callback(nullptr, nullptr, EM_TRUE, (EM_BOOL (*)(int,const void*,void*))pointerlockchange);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_TRUE, key_cb);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_TRUE, key_cb);

    lastTime = emscripten_get_now() * 0.001;
    // request animation frame loop (emscripten will call main_loop ~60fps)
    emscripten_set_main_loop(main_loop, 0, EM_TRUE);

    return 0;
}
