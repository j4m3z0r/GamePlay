#ifdef EMSCRIPTEN

#include <emscripten/emscripten.h>

#ifdef USE_VAO
// Emscripten tests use SDL's proxy to eglGetProcAddress for totally mysterious
// reasons. We duplicate the pattern here, so need SDL's include file.
#include "SDL/SDL.h"
#endif

#include <sys/time.h>

#include "Base.h"
#include "Platform.h"
#include "FileSystem.h"
#include "Game.h"
#include "Form.h"
#include "ScriptController.h"
#include <unistd.h>

// Emscripten's GLUT includes have definitions that conflict with the OpenGL
// ones. Instead, just add the declarations we need to use GLUT's input
// controls. Note that Emscripten's GLUT implementation just pulls
// Module.canvas and window references directly, so there is no need for us to
// ensure that GLUT is correctly wired up to the OpenGL contexts, etc. Method
// definitions taken from opengl.org docs.
extern "C" {
void glutInit(int *argcp, char **argv);
void glutMouseFunc(void (*func)(int button, int state, int x, int y));
void glutMotionFunc(void (*func)(int x, int y));
void glutPassiveMotionFunc(void (*func)(int x, int y));
void glutKeyboardFunc(void (*func)(unsigned char key, int x, int y));

void mouseCB(int button, int state, int x, int y);
void motionCB(int x, int y);
void keyboardCB(unsigned char key, int x, int y);
}

// Constants for mouse events (inferred from experiment)
static const int glutLeftButton = 0;
static const int glutMouseDown = 0;
static const int glutMouseUp = 1;
// Externally referenced global variables.

static bool __initialized;
static bool __suspended;
static EGLDisplay __eglDisplay = EGL_NO_DISPLAY;
static EGLContext __eglContext = EGL_NO_CONTEXT;
static EGLSurface __eglSurface = EGL_NO_SURFACE;
static EGLConfig __eglConfig = 0;

// XXX: Hard-code display size to 800x600 since Emscripten does not implement querying EGL_WIDTH/EGL_HEIGHT
static int __width = 800;
static int __height = 600;

static double __timeStart;
static double __timeAbsolute;
static bool __vsync = WINDOW_VSYNC;
static int __orientationAngle = 90;
static bool __multiTouch = false;
static int __primaryTouchId = -1;
static bool __displayKeyboard = false;

// OpenGL VAO functions.
static const char* __glExtensions;
PFNGLBINDVERTEXARRAYOESPROC glBindVertexArray = NULL;
PFNGLDELETEVERTEXARRAYSOESPROC glDeleteVertexArrays = NULL;
PFNGLGENVERTEXARRAYSOESPROC glGenVertexArrays = NULL;
PFNGLISVERTEXARRAYOESPROC glIsVertexArray = NULL;

#define GESTURE_TAP_DURATION_MAX    200
#define GESTURE_SWIPE_DURATION_MAX  400
#define GESTURE_SWIPE_DISTANCE_MIN  50

static std::bitset<3> __gestureEventsProcessed;

struct TouchPointerData
{
    size_t pointerId;
    bool pressed;
    double time;
    int x;
    int y;
};

TouchPointerData __pointer0;
TouchPointerData __pointer1;

namespace gameplay
{

static double timeval2millis(struct timeval *t)
{
    GP_ASSERT(t);
    return (1000.0 * t->tv_sec) + (0.001 * t->tv_usec);
}

extern void print(const char* format, ...)
{
    GP_ASSERT(format);
    va_list argptr;
    va_start(argptr, format);
    printf(format, argptr);
    va_end(argptr);
}

static EGLenum checkErrorEGL(const char* msg)
{
    GP_ASSERT(msg);
    static const char* errmsg[] =
    {
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
    print("%s: %s.", msg, errmsg[error - EGL_SUCCESS]);
    return error;
}

static int getRotation()
{
    return 0;
}


// Initialized EGL resources.
static bool initEGL()
{
    // Use GLUT for mouse and key events

    // Construct a fake argv array for GLUT. LLVM seems extra picky about what
    // it will accept here, so we allocate a "real" argv array on the heap, and
    // tear it down after init.
    char *arg1 = (char*)malloc(1);
    char **dummyArgv = (char**)malloc(sizeof(char*));
    dummyArgv[0] = arg1;
    glutInit(0, dummyArgv);
    free(dummyArgv[0]);
    free(dummyArgv);

    glutMouseFunc(&mouseCB);
    glutMotionFunc(&motionCB);
    glutPassiveMotionFunc(&motionCB);
    glutKeyboardFunc(&keyboardCB);

    int samples = 0;
    Properties* config = Game::getInstance()->getConfig()->getNamespace("window", true);
    if (config)
    {
        samples = std::max(config->getInt("samples"), 0);
    }

    // Hard-coded to 32-bit/OpenGL ES 2.0.
    // NOTE: EGL_SAMPLE_BUFFERS, EGL_SAMPLES and EGL_DEPTH_SIZE MUST remain at the beginning of the attribute list
    // since they are expected to be at indices 0-5 in config fallback code later.
    // EGL_DEPTH_SIZE is also expected to
    EGLint eglConfigAttrs[] =
    {
        EGL_SAMPLE_BUFFERS,     samples > 0 ? 1 : 0,
        EGL_SAMPLES,            samples,
        EGL_DEPTH_SIZE,         24,
        EGL_RED_SIZE,           8,
        EGL_GREEN_SIZE,         8,
        EGL_BLUE_SIZE,          8,
        EGL_ALPHA_SIZE,         8,
        EGL_STENCIL_SIZE,       8,
        EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    
    EGLint eglConfigCount;
    const EGLint eglContextAttrs[] =
    {
        EGL_CONTEXT_CLIENT_VERSION,    2,
        EGL_NONE
    };

    const EGLint eglSurfaceAttrs[] =
    {
        EGL_RENDER_BUFFER,    EGL_BACK_BUFFER,
        EGL_NONE
    };

    if (__eglDisplay == EGL_NO_DISPLAY && __eglContext == EGL_NO_CONTEXT)
    {
        // Get the EGL display and initialize.
        __eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (__eglDisplay == EGL_NO_DISPLAY)
        {
            checkErrorEGL("eglGetDisplay");
            goto error;
        }

        if (eglInitialize(__eglDisplay, NULL, NULL) != EGL_TRUE)
        {
            checkErrorEGL("eglInitialize");
            goto error;
        }

        // Try both 24 and 16-bit depth sizes since some hardware (i.e. Tegra) does not support 24-bit depth
        bool validConfig = false;
        EGLint depthSizes[] = { 24, 16 };
        for (unsigned int i = 0; i < 2; ++i)
        {
            eglConfigAttrs[1] = samples > 0 ? 1 : 0;
            eglConfigAttrs[3] = samples;
            eglConfigAttrs[5] = depthSizes[i];

            if (eglChooseConfig(__eglDisplay, eglConfigAttrs, &__eglConfig, 1, &eglConfigCount) == EGL_TRUE && eglConfigCount > 0)
            {
                validConfig = true;
                break;
            }

            if (samples)
            {
                // Try lowering the MSAA sample size until we find a supported config
                int sampleCount = samples;
                while (sampleCount)
                {
                    GP_WARN("No EGL config found for depth_size=%d and samples=%d. Trying samples=%d instead.", depthSizes[i], sampleCount, sampleCount / 2);
                    sampleCount /= 2;
                    eglConfigAttrs[1] = sampleCount > 0 ? 1 : 0;
                    eglConfigAttrs[3] = sampleCount;
                    if (eglChooseConfig(__eglDisplay, eglConfigAttrs, &__eglConfig, 1, &eglConfigCount) == EGL_TRUE && eglConfigCount > 0)
                    {
                        validConfig = true;
                        break;
                    }
                }
                if (validConfig)
                    break;
            }
            else
            {
                GP_WARN("No EGL config found for depth_size=%d.", depthSizes[i]);
            }
        }

        if (!validConfig)
        {
            checkErrorEGL("eglChooseConfig");
            goto error;
        }

        __eglContext = eglCreateContext(__eglDisplay, __eglConfig, EGL_NO_CONTEXT, eglContextAttrs);
        if (__eglContext == EGL_NO_CONTEXT)
        {
            checkErrorEGL("eglCreateContext");
            goto error;
        }
    }
    
    // EGL_NATIVE_VISUAL_ID is an attribute of the EGLConfig that is
    // guaranteed to be accepted by ANativeWindow_setBuffersGeometry().
    // As soon as we picked a EGLConfig, we can safely reconfigure the
    // ANativeWindow buffers to match, using EGL_NATIVE_VISUAL_ID.
    EGLint format;
    eglGetConfigAttrib(__eglDisplay, __eglConfig, EGL_NATIVE_VISUAL_ID, &format);

    // Emscripten's GLES2 emulation layer ignores this argument. Just pass in a
    // dummy value so it compiles.
    EGLNativeWindowType dummyWindow;
    __eglSurface = eglCreateWindowSurface(__eglDisplay, __eglConfig, dummyWindow, eglSurfaceAttrs);
    if (__eglSurface == EGL_NO_SURFACE)
    {
        checkErrorEGL("eglCreateWindowSurface");
        goto error;
    }
    
    if (eglMakeCurrent(__eglDisplay, __eglSurface, __eglSurface, __eglContext) != EGL_TRUE)
    {
        checkErrorEGL("eglMakeCurrent");
        goto error;
    }
    
    __orientationAngle = getRotation() * 90;
    
    // Set vsync.
    eglSwapInterval(__eglDisplay, WINDOW_VSYNC ? 1 : 0);
    
    // Initialize OpenGL ES extensions.
    __glExtensions = (const char*)glGetString(GL_EXTENSIONS);

#ifdef USE_VAO
    if (strstr(__glExtensions, "OES_vertex_array_object"))
    {
        // Disable VAO extension for now.
        glBindVertexArray = (PFNGLBINDVERTEXARRAYOESPROC)SDL_GL_GetProcAddress("glBindVertexArray");
        glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSOESPROC)SDL_GL_GetProcAddress("glDeleteVertexArrays");
        glGenVertexArrays = (PFNGLGENVERTEXARRAYSOESPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
        //glIsVertexArray = (PFNGLISVERTEXARRAYOESPROC)eglGetProcAddress("glIsVertexArrayOES");
    }
#endif // USE_VAO

    return true;
    
error:
    return false;
}

extern "C" void motionCB(int x, int y)
{
    // FIXME: Get width and height properly.
    if(x < 0 || y < 0 || x > __width || y > __height)
    {
        return;
    }

    // FIXME: Something weird here with touch handling.
    if(!gameplay::Platform::mouseEventInternal(gameplay::Mouse::MOUSE_MOVE, x, y, 0))
    {
        if(__pointer0.pressed)
        {
            gameplay::Platform::touchEventInternal(gameplay::Touch::TOUCH_MOVE, x, y, 0);
        }
    }
}

extern "C" void mouseCB(int button, int state, int x, int y)
{
    gameplay::Mouse::MouseEvent evt;
    if(button != glutLeftButton)
    {
        return;
    }

    // FIXME: Get width and height properly.
    if(x < 0 || y < 0 || x > __width || y > __height)
    {
        return;
    }

    if(state == glutMouseDown)
    {
        __pointer0.pressed = true;
        evt = gameplay::Mouse::MOUSE_PRESS_LEFT_BUTTON;
    }
    else if(state == glutMouseUp)
    {
        __pointer0.pressed = false;
        evt = gameplay::Mouse::MOUSE_RELEASE_LEFT_BUTTON;
    }
    else
    {
        return;
    }

    if(!gameplay::Platform::mouseEventInternal(evt, x, y, 0))
    {
        if(state == glutMouseDown)
        {
            gameplay::Platform::touchEventInternal(gameplay::Touch::TOUCH_PRESS, x, y, 0);
        }
        else if(state == glutMouseUp)
        {
            gameplay::Platform::touchEventInternal(gameplay::Touch::TOUCH_RELEASE, x, y, 0);
        }
        else
        {
            return;
        }
    }

    // TODO: Handle touch events.
}

static void destroyEGLSurface()
{
    if (__eglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(__eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (__eglSurface != EGL_NO_SURFACE)
    {
        eglDestroySurface(__eglDisplay, __eglSurface);
        __eglSurface = EGL_NO_SURFACE;
    }
}

static void destroyEGLMain()
{
    destroyEGLSurface();

    if (__eglContext != EGL_NO_CONTEXT)
    {
        eglDestroyContext(__eglDisplay, __eglContext);
        __eglContext = EGL_NO_CONTEXT;
    }

    if (__eglDisplay != EGL_NO_DISPLAY)
    {
        eglTerminate(__eglDisplay);
        __eglDisplay = EGL_NO_DISPLAY;
    }
}

// Gets the Keyboard::Key enumeration constant that corresponds to the given GLUT key code.
static Keyboard::Key getKey(unsigned char keycode)
{
    switch(keycode) 
    {
        case 0x0A: return Keyboard::KEY_RETURN;
        case 0x20: return Keyboard::KEY_SPACE;
            
        case '0': return Keyboard::KEY_ZERO;
        case '1': return Keyboard::KEY_ONE;
        case '2': return Keyboard::KEY_TWO;
        case '3': return Keyboard::KEY_THREE;
        case '4': return Keyboard::KEY_FOUR;
        case '5': return Keyboard::KEY_FIVE;
        case '6': return Keyboard::KEY_SIX;
        case '7': return Keyboard::KEY_SEVEN;
        case '8': return Keyboard::KEY_EIGHT;
        case '9': return Keyboard::KEY_NINE;
            
        case 'A': return Keyboard::KEY_CAPITAL_A;
        case 'B': return Keyboard::KEY_CAPITAL_B;
        case 'C': return Keyboard::KEY_CAPITAL_C;
        case 'D': return Keyboard::KEY_CAPITAL_D;
        case 'E': return Keyboard::KEY_CAPITAL_E;
        case 'F': return Keyboard::KEY_CAPITAL_F;
        case 'G': return Keyboard::KEY_CAPITAL_G;
        case 'H': return Keyboard::KEY_CAPITAL_H;
        case 'I': return Keyboard::KEY_CAPITAL_I;
        case 'J': return Keyboard::KEY_CAPITAL_J;
        case 'K': return Keyboard::KEY_CAPITAL_K;
        case 'L': return Keyboard::KEY_CAPITAL_L;
        case 'M': return Keyboard::KEY_CAPITAL_M;
        case 'N': return Keyboard::KEY_CAPITAL_N;
        case 'O': return Keyboard::KEY_CAPITAL_O;
        case 'P': return Keyboard::KEY_CAPITAL_P;
        case 'Q': return Keyboard::KEY_CAPITAL_Q;
        case 'R': return Keyboard::KEY_CAPITAL_R;
        case 'S': return Keyboard::KEY_CAPITAL_S;
        case 'T': return Keyboard::KEY_CAPITAL_T;
        case 'U': return Keyboard::KEY_CAPITAL_U;
        case 'V': return Keyboard::KEY_CAPITAL_V;
        case 'W': return Keyboard::KEY_CAPITAL_W;
        case 'X': return Keyboard::KEY_CAPITAL_X;
        case 'Y': return Keyboard::KEY_CAPITAL_Y;
        case 'Z': return Keyboard::KEY_CAPITAL_Z;
            
            
        case 'a': return Keyboard::KEY_A;
        case 'b': return Keyboard::KEY_B;
        case 'c': return Keyboard::KEY_C;
        case 'd': return Keyboard::KEY_D;
        case 'e': return Keyboard::KEY_E;
        case 'f': return Keyboard::KEY_F;
        case 'g': return Keyboard::KEY_G;
        case 'h': return Keyboard::KEY_H;
        case 'i': return Keyboard::KEY_I;
        case 'j': return Keyboard::KEY_J;
        case 'k': return Keyboard::KEY_K;
        case 'l': return Keyboard::KEY_L;
        case 'm': return Keyboard::KEY_M;
        case 'n': return Keyboard::KEY_N;
        case 'o': return Keyboard::KEY_O;
        case 'p': return Keyboard::KEY_P;
        case 'q': return Keyboard::KEY_Q;
        case 'r': return Keyboard::KEY_R;
        case 's': return Keyboard::KEY_S;
        case 't': return Keyboard::KEY_T;
        case 'u': return Keyboard::KEY_U;
        case 'v': return Keyboard::KEY_V;
        case 'w': return Keyboard::KEY_W;
        case 'x': return Keyboard::KEY_X;
        case 'y': return Keyboard::KEY_Y;
        case 'z': return Keyboard::KEY_Z;
        default: break;
            
       // Symbol Row 3
        case '.': return Keyboard::KEY_PERIOD;
        case ',': return Keyboard::KEY_COMMA;
        case '?': return Keyboard::KEY_QUESTION;
        case '!': return Keyboard::KEY_EXCLAM;
        case '\'': return Keyboard::KEY_APOSTROPHE;
            
        // Symbols Row 2
        case '-': return Keyboard::KEY_MINUS;
        case '/': return Keyboard::KEY_SLASH;
        case ':': return Keyboard::KEY_COLON;
        case ';': return Keyboard::KEY_SEMICOLON;
        case '(': return Keyboard::KEY_LEFT_PARENTHESIS;
        case ')': return Keyboard::KEY_RIGHT_PARENTHESIS;
        case '$': return Keyboard::KEY_DOLLAR;
        case '&': return Keyboard::KEY_AMPERSAND;
        case '@': return Keyboard::KEY_AT;
        case '"': return Keyboard::KEY_QUOTE;
            
        // Numeric Symbols Row 1
        case 0x5B: return Keyboard::KEY_LEFT_BRACKET;
        case 0x5D: return Keyboard::KEY_RIGHT_BRACKET;
        case 0x7B: return Keyboard::KEY_LEFT_BRACE;
        case 0x7D: return Keyboard::KEY_RIGHT_BRACE;
        case 0x23: return Keyboard::KEY_NUMBER;
        case '%': return Keyboard::KEY_PERCENT;
        case 0x5E: return Keyboard::KEY_CIRCUMFLEX;
        case '*': return Keyboard::KEY_ASTERISK;
        case '+': return Keyboard::KEY_PLUS;
        case '=': return Keyboard::KEY_EQUAL;
            
        // Numeric Symbols Row 2
        case '_': return Keyboard::KEY_UNDERSCORE;
        case '\\': return Keyboard::KEY_BACK_SLASH;
        case 0x7C: return Keyboard::KEY_BAR;
        case '~': return Keyboard::KEY_TILDE;
        case 0x3C: return Keyboard::KEY_LESS_THAN;
        case 0x3E: return Keyboard::KEY_GREATER_THAN;
        case 0x80: return Keyboard::KEY_EURO;
        case 0xA3: return Keyboard::KEY_POUND;
        case 0xA5: return Keyboard::KEY_YEN;
        case 0xB7: return Keyboard::KEY_MIDDLE_DOT;
    }
    return Keyboard::KEY_NONE;
}

static Platform *self;
static Game *self_game;

// GLUT only gives keyboard events, without noting whether it is key up or
// down. We store some state here to try to simulate the release events. Define
// keys as being released if we haven't had any events for 1/5 second.


static double __lastKeyPressTime = 0;
unsigned char __lastKeyCode = 0;

extern "C" void keyboardCB(unsigned char key, int x, int y)
{
    Keyboard::Key gpkey = getKey(key);
    if(__lastKeyCode && key != __lastKeyCode)
    {
        gameplay::Platform::keyEventInternal(gameplay::Keyboard::KEY_RELEASE, gpkey);
    }
    __lastKeyCode = key;

    struct timeval t;
    gettimeofday(&t, NULL);
    __lastKeyPressTime = timeval2millis(&t);

    gameplay::Platform::keyEventInternal(gameplay::Keyboard::KEY_PRESS, gpkey);
}

Platform::Platform(Game* game)
    : _game(game)
{
}

Platform::~Platform()
{
}

Platform* Platform::create(Game* game, void* attachToWindow)
{
    FileSystem::setResourcePath("./");
    Platform* platform = new Platform(game);

    return platform;
}


extern "C" void main_loop_iter(void)
{
    if(__lastKeyCode)
    {
        struct timeval t;
        gettimeofday(&t, NULL);
        double curTime = timeval2millis(&t);

        if(curTime - __lastKeyPressTime > 200)
        {
            Keyboard::Key gpkey = getKey(__lastKeyCode);
            gameplay::Platform::keyEventInternal(gameplay::Keyboard::KEY_RELEASE, gpkey);
            __lastKeyCode = 0;
        }
    }

    self_game->frame();
}

int Platform::enterMessagePump()
{
    // Set nasty global pointers to this so that emscripten_set_main_loop has a
    // pointer to work with.
    self = this;
    self_game = this->_game;


    initEGL();
    this->_game->run();
    __initialized = true;

    //__initialized = false;
    __suspended = false;

    // Get the initial time.
    struct timeval t;
    gettimeofday(&t, NULL);
    __timeStart = timeval2millis(&t);
    __timeAbsolute = 0L;

    // Code after here won't ever run as emscripten main loops never end.
    emscripten_set_main_loop(&main_loop_iter, 0, 1);

    Game::getInstance()->exit();
    destroyEGLMain();
    __initialized = false;

    return true;
}

void Platform::signalShutdown() 
{
    // nothing to do  
}

bool Platform::canExit()
{
    return true;
}
   
unsigned int Platform::getDisplayWidth()
{
    return __width;
}
    
unsigned int Platform::getDisplayHeight()
{
    return __height;
}
    
double Platform::getAbsoluteTime()
{
    struct timeval t;
    gettimeofday(&t, NULL);
    double now = timeval2millis(&t);
    __timeAbsolute = now - __timeStart;

    return __timeAbsolute;
}

void Platform::setAbsoluteTime(double time)
{
    __timeAbsolute = time;
}

bool Platform::isVsync()
{
    return __vsync;
}

void Platform::setVsync(bool enable)
{
    eglSwapInterval(__eglDisplay, enable ? 1 : 0);
    __vsync = enable;
}


void Platform::swapBuffers()
{
    if (__eglDisplay && __eglSurface)
        eglSwapBuffers(__eglDisplay, __eglSurface);
}

void Platform::sleep(long ms)
{
    usleep(ms * 1000);
}

void Platform::setMultiTouch(bool enabled)
{
    __multiTouch = enabled;
}

bool Platform::isMultiTouch()
{
    return __multiTouch;
}

bool Platform::hasMouse()
{
    // not supported
    return true;
}

void Platform::setMouseCaptured(bool captured)
{
    // not supported
}

bool Platform::isMouseCaptured()
{
    // not supported
    return false;
}

void Platform::setCursorVisible(bool visible)
{
    // not supported
}

bool Platform::isCursorVisible()
{
    // not supported
    return false;
}

void Platform::displayKeyboard(bool display)
{
    return;
}

void Platform::touchEventInternal(Touch::TouchEvent evt, int x, int y, unsigned int contactIndex)
{
    if (!Form::touchEventInternal(evt, x, y, contactIndex))
    {
        Game::getInstance()->touchEvent(evt, x, y, contactIndex);
        Game::getInstance()->getScriptController()->touchEvent(evt, x, y, contactIndex);
    }
}

void Platform::keyEventInternal(Keyboard::KeyEvent evt, int key)
{
    if (!Form::keyEventInternal(evt, key))
    {
        Game::getInstance()->keyEvent(evt, key);
        Game::getInstance()->getScriptController()->keyEvent(evt, key);
    }
}

bool Platform::mouseEventInternal(Mouse::MouseEvent evt, int x, int y, int wheelDelta)
{
    if (Form::mouseEventInternal(evt, x, y, wheelDelta))
    {
        return true;
    }
    else if (Game::getInstance()->mouseEvent(evt, x, y, wheelDelta))
    {
        return true;
    }
    else
    {
        return Game::getInstance()->getScriptController()->mouseEvent(evt, x, y, wheelDelta);
    }
}

void Platform::gamepadEventConnectedInternal(GamepadHandle handle,  unsigned int buttonCount, unsigned int joystickCount, unsigned int triggerCount,
                                             unsigned int vendorId, unsigned int productId, const char* vendorString, const char* productString)
{
    Gamepad::add(handle, buttonCount, joystickCount, triggerCount, vendorId, productId, vendorString, productString);
}

void Platform::gamepadEventDisconnectedInternal(GamepadHandle handle)
{
    Gamepad::remove(handle);
}

void Platform::shutdownInternal()
{
    Game::getInstance()->shutdown();
}

bool Platform::isGestureSupported(Gesture::GestureEvent evt)
{
    // Pinch currently not implemented
    return evt == gameplay::Gesture::GESTURE_SWIPE || evt == gameplay::Gesture::GESTURE_TAP;
}

void Platform::registerGesture(Gesture::GestureEvent evt)
{
    switch(evt)
    {
    case Gesture::GESTURE_ANY_SUPPORTED:
        __gestureEventsProcessed.set();
        break;

    case Gesture::GESTURE_TAP:
    case Gesture::GESTURE_SWIPE:
        __gestureEventsProcessed.set(evt);
        break;

    default:
        break;
    }
}

void Platform::unregisterGesture(Gesture::GestureEvent evt)
{
    switch(evt)
    {
    case Gesture::GESTURE_ANY_SUPPORTED:
        __gestureEventsProcessed.reset();
        break;

    case Gesture::GESTURE_TAP:
    case Gesture::GESTURE_SWIPE:
        __gestureEventsProcessed.set(evt, 0);
        break;

    default:
        break;
    }
}
    
bool Platform::isGestureRegistered(Gesture::GestureEvent evt)
{
    return __gestureEventsProcessed.test(evt);
}

void Platform::pollGamepadState(Gamepad* gamepad)
{
}


}

#endif
