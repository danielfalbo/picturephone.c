/*
 * Picturephone --- Text-based video-conferencing in the terminal.
 * -----------------------------------------------------------------------
 *
 * Credits and inspiration: antirez/kilo, ertdfgcvb/play.core, kubrick/2001.
 *
 * Author:                  Daniel Falbo <hi at danielfalbo dot com>.
 *
 * */

#include <time.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <termios.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/select.h>

/* --- WEBCAM INTERFACE -------------------------------------------
 * Generic interfaces and data structures.
 * Each OS/device will implement them in their own way. */

typedef struct {
  int width;
  int height;
  unsigned char *pixels; /* ARGB/BGRA buffer */
} frame;

typedef struct {
  int isRunning;

  frame currentFrame;

  /* Opaque pointer to OS-specific state */
  void *internal;

  /* OS will write camera data somewhere, and we will be reading it from the
   * same place: we use a lock so only one of us uses it at any given time. */
  pthread_mutex_t lock;

  /* Dummy State */
  struct {
    int x, y;
    int dx, dy;
  } bounce;
  unsigned int noise_seed;
} camera;

typedef struct {
  char name[128];
  char id[64];
} CameraInfo;

CameraInfo *enumerateCameras(void);
void freeCameraList(CameraInfo *list);

/* --- CONFIG --------------------------------------------------------------- */

#define MODE_MIRROR 0
#define MODE_NETWORK 1

#define NET_ROLE_SERVER 0
#define NET_ROLE_CLIENT 1

#define VIEW_PIP 0
#define VIEW_SPLIT 1

struct editorConfig {
  int screenrows; /* Number of rows that we can show */
  int screencols; /* Number of cols that we can show */
  int rawmode;    /* Is terminal raw mode enabled? */
  char statusmsg[80];
  time_t statusmsg_time;

  /* Configurable Parameters */
  int mode;       /* MODE_MIRROR or MODE_NETWORK */
  int view_mode;  /* VIEW_PIP or VIEW_SPLIT */
  int net_role;   /* NET_ROLE_SERVER or NET_ROLE_CLIENT */
  int net_port;
  char net_ip[64];
  char camera_target[64]; /* specific camera ID or "dummy-..." */
  int list_cameras;       /* Check if we should list cameras and exit */

  /* Density String Config */
  char density_arg[256];
  char **density_glyphs;
  int density_count;
};

#define DENSITY_ASCII_DEFAULT " .x?A@"
#define DENSITY_UNICODE_DEFAULT " .x?▂▄▆█"

static struct editorConfig E;

/* --- CONFIGURATION ENGINE ------------------------------------------------- */

enum config_type {
  CONF_INT,
  CONF_STRING,
  CONF_BOOL,   /* switch: 0 or 1 */
  CONF_ENUM    /* selection from a list of named integers */
};

struct config_enum_map {
  const char *name;
  int val;
};

struct config_option {
  const char *name;         /* CLI flag name (e.g., "port") */
  const char *description;  /* Human readable description */
  enum config_type type;
  void *ptr;                /* Pointer to the variable in E */
  struct config_enum_map *enum_map; /* For CONF_ENUM, NULL otherwise */
};

/* Enum Definitions */
struct config_enum_map mode_map[] = {
  {"mirror", MODE_MIRROR},
  {"network", MODE_NETWORK},
  {NULL, 0}
};

struct config_enum_map role_map[] = {
  {"server", NET_ROLE_SERVER},
  {"client", NET_ROLE_CLIENT},
  {NULL, 0}
};

/* The Global Configuration Table */
struct config_option config_table[] = {
  {"mode", "App Mode", CONF_ENUM, &E.mode, mode_map},
  {"role", "Network Role", CONF_ENUM, &E.net_role, role_map},
  {"port", "Port", CONF_INT, &E.net_port, NULL},
  {"ip", "Remote IP", CONF_STRING, E.net_ip, NULL},
  {"camera", "Camera ID", CONF_STRING, E.camera_target, NULL},
  {"list-cameras", "List Cameras", CONF_BOOL, &E.list_cameras, NULL},
  {"density-string", "Density String", CONF_STRING, E.density_arg, NULL},
  {NULL, NULL, 0, NULL, NULL}
};

/* Helper: Get string representation of current ENUM value */
const char *configGetEnumString(struct config_option *opt) {
  int current_val = *(int *)opt->ptr;
  for (int i = 0; opt->enum_map[i].name; i++) {
    if (opt->enum_map[i].val == current_val) return opt->enum_map[i].name;
  }
  return "?";
}

/* Helper: Set ENUM value from string */
int configSetEnum(struct config_option *opt, const char *val_str) {
  for (int i = 0; opt->enum_map[i].name; i++) {
    if (strcasecmp(opt->enum_map[i].name, val_str) == 0) {
      *(int *)opt->ptr = opt->enum_map[i].val;
      return 1;
    }
  }
  return 0;
}

void print_help(const char *program_name);

/* Parse CLI arguments using the config table. */
void parse_config_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    int consumed = 0;
    char *arg = argv[i];

    /* Handle Help */
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_help(argv[0]);
      exit(0);
    }

    /* Check for flag prefix */
    if (arg[0] != '-' || arg[1] != '-') {
      fprintf(stderr, "Unknown argument: %s\n", arg);
      exit(1);
    }
    char *flag = arg + 2; // Skip "--"

    /* Search table */
    for (int j = 0; config_table[j].name; j++) {
      struct config_option *opt = &config_table[j];
      if (strcmp(flag, opt->name) == 0) {
        // Matched!
        if (opt->type == CONF_BOOL) {
          *(int *)opt->ptr = 1;
          consumed = 1;
        } else {
          // Require next argument
          if (i + 1 >= argc) {
            fprintf(stderr, "Error: --%s requires an argument.\n", opt->name);
            exit(1);
          }
          char *val = argv[++i];

          if (opt->type == CONF_INT) {
            *(int *)opt->ptr = atoi(val);
          } else if (opt->type == CONF_STRING) {
            strncpy((char *)opt->ptr, val, 63); // Safety? assumes 64 len buffer
          } else if (opt->type == CONF_ENUM) {
            if (!configSetEnum(opt, val)) {
              fprintf(stderr, "Invalid value for --%s: %s\n", opt->name, val);
              fprintf(stderr, "Valid options: ");
              for(int k=0; opt->enum_map[k].name; k++)
                fprintf(stderr, "%s ", opt->enum_map[k].name);
              fprintf(stderr, "\n");
              exit(1);
            }
          }
          consumed = 1;
        }
        break;
      }
    }

    if (!consumed) {
      /* Legacy / Manual Fallback support could go here */
      fprintf(stderr, "Unknown argument: %s\n", arg);
      print_help(argv[0]);
      exit(1);
    }
  }
}

/* Print dynamic help message */
void print_help(const char *program_name) {
  fprintf(stderr, "Usage: %s [options]\n\n", program_name);
  fprintf(stderr, "Options:\n");
  for (int i = 0; config_table[i].name; i++) {
    struct config_option *opt = &config_table[i];
    char hint[32] = "";
    if (opt->type == CONF_INT) strcpy(hint, " <n>");
    else if (opt->type == CONF_STRING) strcpy(hint, " <s>");
    else if (opt->type == CONF_ENUM) strcpy(hint, " <val>");

    fprintf(stderr, "  --%-16s %s%s\n", opt->name, opt->description, hint);
    if (opt->type == CONF_ENUM) {
      fprintf(stderr, "                         Values: ");
      for(int k=0; opt->enum_map[k].name; k++)
        fprintf(stderr, "%s ", opt->enum_map[k].name);
      fprintf(stderr, "\n");
    }
  }
}

// Initialize the camera (select default device)
void cameraInit(camera *cam, int width, int height);

// Start capturing
void cameraStart(camera *cam);

// Copy the latest frame safely into the target buffer.
// Returns 1 if a new frame was available, 0 otherwise.
int cameraGetFrame(camera *cam, frame *outFrame);

/* --- DUMMY CAMERA IMPLEMENTATION ------------------------------------------ */

void appendDummyCameras(CameraInfo *list, int *idx) {
  strcpy(list[*idx].name, "Dummy Gradient");
  strcpy(list[*idx].id, "dummy-gradient");
  (*idx)++;

  strcpy(list[*idx].name, "Dummy Noise");
  strcpy(list[*idx].id, "dummy-noise");
  (*idx)++;

  strcpy(list[*idx].name, "Dummy Bouncing Ball");
  strcpy(list[*idx].id, "dummy-bounce");
  (*idx)++;
}

int isDummyCamera(void) {
  return strncmp(E.camera_target, "dummy-", 6) == 0;
}

int initDummyCamera(camera *cam) {
  cam->bounce.x = 100;
  cam->bounce.y = 100;
  cam->bounce.dx = 8;
  cam->bounce.dy = 8;
  cam->noise_seed = 12345;

  if (isDummyCamera()) {
    cam->currentFrame.width = 640;
    cam->currentFrame.height = 480;
    cam->currentFrame.pixels = malloc(640 * 480 * 4);
    cam->internal = NULL;
    pthread_mutex_init(&cam->lock, NULL);
    return 1;
  }
  return 0;
}

int startDummyCamera(camera *cam) {
  if (isDummyCamera()) {
    cam->isRunning = 1;
    return 1;
  }
  return 0;
}

int getDummyFrame(camera *cam, frame *outFrame) {
  if (!isDummyCamera()) return 0;

  int w = cam->currentFrame.width;
  int h = cam->currentFrame.height;
  unsigned char *p = cam->currentFrame.pixels;

  if (strcmp(E.camera_target, "dummy-noise") == 0) {
    for (int i = 0; i < w * h * 4; i+=4) {
      unsigned char val = rand_r(&cam->noise_seed) % 256;
      p[i+0] = val; p[i+1] = val; p[i+2] = val;
    }
  } else if (strcmp(E.camera_target, "dummy-bounce") == 0) {
    // Clear
    memset(p, 0, w * h * 4);

    // Update
    cam->bounce.x += cam->bounce.dx;
    cam->bounce.y += cam->bounce.dy;

    int box = 80;
    if (cam->bounce.x < 0 || cam->bounce.x + box >= w) {
      cam->bounce.dx = -cam->bounce.dx;
      cam->bounce.x += cam->bounce.dx;
    }
    if (cam->bounce.y < 0 || cam->bounce.y + box >= h) {
      cam->bounce.dy = -cam->bounce.dy;
      cam->bounce.y += cam->bounce.dy;
    }

    // Draw
    for (int y = cam->bounce.y; y < cam->bounce.y + box; y++) {
      for (int x = cam->bounce.x; x < cam->bounce.x + box; x++) {
        if (x < 0 || x >= w || y < 0 || y >= h) continue;
        int offset = (y * w + x) * 4;
        p[offset+0] = 255;
        p[offset+1] = 255;
        p[offset+2] = 255;
      }
    }
  } else {
    // Default: Gradient
    static int frame_counter = 0;
    frame_counter++;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int offset = (y * w + x) * 4;
        unsigned char val = (x + y + frame_counter) % 255;
        p[offset + 0] = val; // B
        p[offset + 1] = val; // G
        p[offset + 2] = val; // R
      }
    }
  }

  outFrame->width = w;
  outFrame->height = h;
  outFrame->pixels = p;
  return 1;
}

/* --- DEVICE/OS WEBCAM IMPLEMENTATIONS ------------------------------------- */

#ifdef __linux__
// TODO: Implement Linux/V4L2 support

CameraInfo *enumerateCameras(void) {
  CameraInfo *list = malloc(sizeof(CameraInfo) * 4);
  int idx = 0;

  appendDummyCameras(list, &idx);

  list[idx].name[0] = '\0';
  return list;
}

void freeCameraList(CameraInfo *list) {
  free(list);
}

void cameraInit(camera *cam, int width, int height) {
  (void)width; (void)height;
  if (initDummyCamera(cam)) return;
  // Stub
}

void cameraStart(camera *cam) {
  if (startDummyCamera(cam)) return;
  // Stub
}

int cameraGetFrame(camera *cam, frame *outFrame) {
  if (getDummyFrame(cam, outFrame)) return 1;
  // Stub
  return 0;
}
#endif

#ifdef __APPLE__
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>

// Typedefs for cleaner objc_msgSend calls
typedef id (*id_msg_t)(id, SEL);
typedef id (*id_msg_ptr_t)(id, SEL, void*);
typedef id (*id_msg_str_t)(id, SEL, const char*);
typedef void (*void_msg_ptr_t)(id, SEL, void*);
typedef void (*void_msg_id_id_t)(id, SEL, id, id);
typedef id (*id_msg_id_t)(id, SEL, id);
typedef id (*id_msg_id_ptr_t)(id, SEL, id, void*);
typedef unsigned long (*ulong_msg_t)(id, SEL);
typedef id (*id_msg_uint_t)(id, SEL, unsigned long);

// Context to pass into the delegate callback
static camera *global_cam_context = NULL;

// -----------------------------------------------------------------------
// Camera Enumeration
// -----------------------------------------------------------------------
CameraInfo *enumerateCameras(void) {
  id AVCaptureDevice = (id)objc_getClass("AVCaptureDevice");
  id NSString = (id)objc_getClass("NSString");

  SEL s_stringWithUTF8String = sel_registerName("stringWithUTF8String:");
  SEL s_devicesWithMediaType = sel_registerName("devicesWithMediaType:");
  SEL s_count = sel_registerName("count");
  SEL s_objectAtIndex = sel_registerName("objectAtIndex:");
  SEL s_localizedName = sel_registerName("localizedName");
  SEL s_uniqueID = sel_registerName("uniqueID");
  SEL s_UTF8String = sel_registerName("UTF8String");

  id mediaType = ((id_msg_str_t)objc_msgSend)(NSString,
      s_stringWithUTF8String, "vide");
  id devices = ((id_msg_id_t)objc_msgSend)(AVCaptureDevice,
      s_devicesWithMediaType, mediaType);

  unsigned long count = 0;
  if (devices) {
    count = ((ulong_msg_t)objc_msgSend)(devices, s_count);
  }

  // Allocate list: Real cameras + 3 dummies + 1 terminator
  CameraInfo *list = malloc(sizeof(CameraInfo) * (count + 4));

  int idx = 0;
  for (unsigned long i = 0; i < count; i++) {
    id dev = ((id_msg_uint_t)objc_msgSend)(devices, s_objectAtIndex, i);
    id name = ((id_msg_t)objc_msgSend)(dev, s_localizedName);
    id uid = ((id_msg_t)objc_msgSend)(dev, s_uniqueID);

    const char *nameStr = ((const char* (*)(id, SEL))objc_msgSend)(name,
        s_UTF8String);
    const char *uidStr = ((const char* (*)(id, SEL))objc_msgSend)(uid,
        s_UTF8String);

    strncpy(list[idx].name, nameStr, 127);
    strncpy(list[idx].id, uidStr, 63);
    idx++;
  }

  // Add Dummies
  appendDummyCameras(list, &idx);

  // Terminator
  list[idx].name[0] = '\0';

  return list;
}

void freeCameraList(CameraInfo *list) {
  free(list);
}

// -----------------------------------------------------------------------
// The Delegate: This C function acts as the Objective-C method:
// - (void)captureOutput:(AVCaptureOutput *)output
//   didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
//   fromConnection:(AVCaptureConnection *)connection;
// -----------------------------------------------------------------------
void captureOutput_didOutputSampleBuffer_fromConnection(id self, SEL _cmd,
    id output,
    CMSampleBufferRef sampleBuffer,
    id connection) {
  (void)self; (void)_cmd; (void)output; (void)connection;

  if (!global_cam_context) return;

  // Access the pixel buffer from the sample buffer
  CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);

  // Lock the base address to access pixels
  CVPixelBufferLockBaseAddress(imageBuffer, 0);

  void *baseAddress = CVPixelBufferGetBaseAddress(imageBuffer);
  size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
  // size_t width = CVPixelBufferGetWidth(imageBuffer);
  size_t height = CVPixelBufferGetHeight(imageBuffer);
  size_t width = CVPixelBufferGetWidth(imageBuffer);

  // Copy to our internal buffer
  pthread_mutex_lock(&global_cam_context->lock);

  // Basic resize/copy logic could go here.
  // Currently we just dump the raw buffer.
  // Note: AVFoundation gives BGRA.

  frame *f = &global_cam_context->currentFrame;
  if (f->pixels == NULL) {
    f->width = (int)width;
    f->height = (int)height;
    f->pixels = malloc(height * bytesPerRow);
  }

  memcpy(f->pixels, baseAddress, height * bytesPerRow);

  pthread_mutex_unlock(&global_cam_context->lock);

  CVPixelBufferUnlockBaseAddress(imageBuffer, 0);
}

void cameraInit(camera *cam, int width, int height) {
  (void)width; (void)height;

  if (initDummyCamera(cam)) return;

  // Setup global context for the static callback
  global_cam_context = cam;
  pthread_mutex_init(&cam->lock, NULL);
  cam->currentFrame.pixels = NULL;

  // Create the Delegate Class at Runtime
  // We define a class "CaptureDelegate" that inherits from NSObject
  Class CaptureDelegate = objc_allocateClassPair(objc_getClass("NSObject"),
      "CaptureDelegate", 0);

  // Add the delegate method
  // (signature "v@:@@@" means void return, id self, SEL cmd, id, id, id)
  class_addMethod(CaptureDelegate,
      sel_registerName("captureOutput:didOutputSampleBuffer:fromConnection:"),
      (IMP)captureOutput_didOutputSampleBuffer_fromConnection,
      "v@:@@@");

  objc_registerClassPair(CaptureDelegate);

  // Setup AVFoundation Classes
  id AVCaptureSession = ((id_msg_t)objc_msgSend)(
      (id)objc_getClass("AVCaptureSession"),
      sel_registerName("alloc")
      );
  id session = ((id_msg_t)objc_msgSend)(AVCaptureSession,
      sel_registerName("init"));

  // Set Preset (Low quality is better for ASCII)
  // NSString *sessionPreset = AVFoundation.AVCaptureSessionPreset640x480;
  id preset = ((id_msg_str_t)objc_msgSend)(
      (id)objc_getClass("NSString"),
      sel_registerName("stringWithUTF8String:"),
      "AVCaptureSessionPreset640x480"
      );
  ((void_msg_ptr_t)objc_msgSend)(session,
    sel_registerName("setSessionPreset:"),
    preset);

  // Get Input Device
  id AVCaptureDevice = (id)objc_getClass("AVCaptureDevice");
  id device = NULL;

  if (strlen(E.camera_target) == 0 || strcmp(E.camera_target, "default") == 0) {
    id mediaType = ((id_msg_str_t)objc_msgSend)(
        (id)objc_getClass("NSString"),
        sel_registerName("stringWithUTF8String:"),
        "vide" // "vide" = AVMediaTypeVideo
        );

    device = ((id_msg_id_t)objc_msgSend)(
        AVCaptureDevice,
        sel_registerName("defaultDeviceWithMediaType:"),
        mediaType
        );
  } else {
    // By ID
    id uid = ((id_msg_str_t)objc_msgSend)(
        (id)objc_getClass("NSString"),
        sel_registerName("stringWithUTF8String:"),
        E.camera_target
        );
    device = ((id_msg_id_t)objc_msgSend)(
        AVCaptureDevice,
        sel_registerName("deviceWithUniqueID:"),
        uid
        );
  }

  if (!device) {
    fprintf(stderr, "Error: Video device not found.\n");
    exit(1);
  }

  id AVCaptureDeviceInput = (id)objc_getClass("AVCaptureDeviceInput");
  id input = ((id_msg_id_ptr_t)objc_msgSend)(
      AVCaptureDeviceInput,
      sel_registerName("deviceInputWithDevice:error:"),
      device,
      NULL
      );
  ((void_msg_ptr_t)objc_msgSend)(
    session,
    sel_registerName("addInput:"),
    input
    );

  // Setup Output
  id AVCaptureVideoDataOutput = (id)objc_getClass("AVCaptureVideoDataOutput");
  id output = ((id_msg_t)objc_msgSend)(
      AVCaptureVideoDataOutput,
      sel_registerName("alloc")
      );
  output = ((id_msg_t)objc_msgSend)(output, sel_registerName("init"));

  // Set Pixel Format to BGRA (kCVPixelFormatType_32BGRA = 'BGRA' = 1111970369)
  id NSNumber = (id)objc_getClass("NSNumber");
  id pixelFormat = ((id_msg_ptr_t)objc_msgSend)(
      NSNumber,
      sel_registerName("numberWithInt:"),
      (void*)1111970369
      );
  id NSDictionary = (id)objc_getClass("NSDictionary");
  id const kCVPixelBufferPixelFormatTypeKey = ((id_msg_str_t)objc_msgSend)(
      (id)objc_getClass("NSString"),
      sel_registerName("stringWithUTF8String:"),
      "PixelFormatType"
      );

  id settings = ((id (*)(id, SEL, id, id))objc_msgSend)(
      NSDictionary,
      sel_registerName("dictionaryWithObject:forKey:"),
      pixelFormat,
      kCVPixelBufferPixelFormatTypeKey
      );

  ((void_msg_ptr_t)objc_msgSend)(output,
    sel_registerName("setVideoSettings:"),
    settings);

  // Connect Delegate
  id delegateInstance = ((id_msg_t)objc_msgSend)(
      (id)((id_msg_t)objc_msgSend)(
        (id)CaptureDelegate,
        sel_registerName("alloc")
        ),
      sel_registerName("init")
      );

  // We need a dispatch queue
  id dispatchQueue = (id)dispatch_queue_create("camera_queue", NULL);
  ((void_msg_id_id_t)objc_msgSend)(
    output,
    sel_registerName("setSampleBufferDelegate:queue:"),
    delegateInstance,
    dispatchQueue
    );

  ((void_msg_ptr_t)objc_msgSend)(
    session,
    sel_registerName("addOutput:"),
    output
    );

  // Save session
  cam->internal = session;
  return;
}

void cameraStart(camera *cam) {
  if (startDummyCamera(cam)) return;
  id session = (id)cam->internal;
  ((void_msg_ptr_t)objc_msgSend)(session,
    sel_registerName("startRunning"),
    NULL);
  cam->isRunning = 1;
}

int cameraGetFrame(camera *cam, frame *outFrame) {
  if (getDummyFrame(cam, outFrame)) return 1;

  pthread_mutex_lock(&cam->lock);
  if (cam->currentFrame.pixels) {
    // Shallow copy for demo. Could deep copy if processing slowly
    outFrame->width = cam->currentFrame.width;
    outFrame->height = cam->currentFrame.height;
    outFrame->pixels = cam->currentFrame.pixels; // Careful with ownership
    pthread_mutex_unlock(&cam->lock);
    return 1;
  }
  pthread_mutex_unlock(&cam->lock);
  return 0;
}
#endif

/* --- LOW-LEVEL TERMINAL HANDLING ------------------------------------------ */

enum KEY_ACTION{
  KEY_NULL = 0,       /* NULL */
  CTRL_C = 3,         /* Ctrl-c */
  CTRL_D = 4,         /* Ctrl-d */
  CTRL_F = 6,         /* Ctrl-f */
  CTRL_H = 8,         /* Ctrl-h */
  TAB = 9,            /* Tab */
  CTRL_L = 12,        /* Ctrl+l */
  ENTER = 13,         /* Enter */
  CTRL_Q = 17,        /* Ctrl-q */
  CTRL_S = 19,        /* Ctrl-s */
  CTRL_U = 21,        /* Ctrl-u */
  ESC = 27,           /* Escape */
  BACKSPACE =  127,   /* Backspace */
  /* The following are just soft codes, not really reported by the
   * terminal directly. */
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/* Read a key from the terminal put in raw mode, trying to handle
 * escape sequences. */
int editorReadKey(int fd) {
  int nread;
  char c, seq[3];
  while ((nread = read(fd,&c,1)) == 0);
  if (nread == -1) exit(1);

  while(1) {
    switch(c) {
      case ESC:    /* escape sequence */
        /* If this is just an ESC, we'll timeout here. */
        if (read(fd,seq,1) == 0) return ESC;
        if (read(fd,seq+1,1) == 0) return ESC;

        /* ESC [ sequences. */
        if (seq[0] == '[') {
          if (seq[1] >= '0' && seq[1] <= '9') {
            /* Extended escape, read additional byte. */
            if (read(fd,seq+2,1) == 0) return ESC;
            if (seq[2] == '~') {
              switch(seq[1]) {
                case '3': return DEL_KEY;
                case '5': return PAGE_UP;
                case '6': return PAGE_DOWN;
              }
            }
          } else {
            switch(seq[1]) {
              case 'A': return ARROW_UP;
              case 'B': return ARROW_DOWN;
              case 'C': return ARROW_RIGHT;
              case 'D': return ARROW_LEFT;
              case 'H': return HOME_KEY;
              case 'F': return END_KEY;
            }
          }
        }

        /* ESC O sequences. */
        else if (seq[0] == 'O') {
          switch(seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
          }
        }
        break;
      default:
        return c;
    }
  }
}

static struct termios orig_termios; /* In order to restore at exit.*/

void disableRawMode(int fd) {
  /* Don't even check the return value as it's too late. */
  if (E.rawmode) {
    tcsetattr(fd,TCSAFLUSH,&orig_termios);
    E.rawmode = 0;
  }
}

/* Called at exit to avoid remaining in raw mode. */
void editorAtExit(void) {
  disableRawMode(STDIN_FILENO);
  write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
  struct termios raw;

  if (E.rawmode) return 0; /* Already enabled. */
  if (!isatty(STDIN_FILENO)) goto fatal;
  atexit(editorAtExit);
  if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

  raw = orig_termios;  /* modify the original mode */
  /* input modes: no break, no CR to NL, no parity check, no strip char,
   * no start/stop output control. */
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  /* output modes - disable post processing */
  raw.c_oflag &= ~(OPOST);
  /* control modes - set 8 bit chars */
  raw.c_cflag |= (CS8);
  /* local modes - choing off, canonical off, no extended functions,
   * no signal chars (^Z,^C) */
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  /* control chars - set return condition: min number of bytes and timer. */
  raw.c_cc[VMIN] = 0; /* Return each byte, or zero for timeout. */
  raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

  /* put terminal in raw mode after flushing */
  if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
  E.rawmode = 1;
  return 0;

fatal:
  errno = ENOTTY;
  return -1;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor is stored at *rows and *cols and 0 is returned. */
int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  /* Report cursor location */
  if (write(ofd, "\x1b[6n", 4) != 4) return -1;

  /* Read the response: ESC [ rows ; cols R */
  while (i < sizeof(buf)-1) {
    if (read(ifd,buf+i,1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  /* Parse it. */
  if (buf[0] != ESC || buf[1] != '[') return -1;
  if (sscanf(buf+2,"%d;%d",rows,cols) != 2) return -1;
  return 0;
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    /* ioctl() failed. Try to query the terminal itself. */
    int orig_row, orig_col, retval;

    /* Get the initial position so we can restore it later. */
    retval = getCursorPosition(ifd,ofd,&orig_row,&orig_col);
    if (retval == -1) goto failed;

    /* Go to right/bottom margin and get position. */
    if (write(ofd,"\x1b[999C\x1b[999B",12) != 12) goto failed;
    retval = getCursorPosition(ifd,ofd,rows,cols);
    if (retval == -1) goto failed;

    /* Restore position. */
    char seq[32];
    snprintf(seq,32,"\x1b[%d;%dH",orig_row,orig_col);
    if (write(ofd,seq,strlen(seq)) == -1) {
      /* Can't recover... */
    }
    return 0;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }

failed:
  return -1;
}

/* --- TERMINAL UPDATE ------------------------------------------------------ */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL,0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b,ab->len+len);

  if (new == NULL) return;
  memcpy(new+ab->len,s,len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

void updateWindowSize(void) {
  if (getWindowSize(STDIN_FILENO,STDOUT_FILENO,
        &E.screenrows,&E.screencols) == -1) {
    perror("Unable to query the screen for size (columns / rows)");
    exit(1);
  }
  E.screenrows--;
}

void handleSigWinCh(int unused __attribute__((unused))) {
  updateWindowSize();
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/* --- DENSITY STRING HANDLING ---------------------------------------------- */

/* Return the number of bytes that compose the UTF-8 character starting at
 * 'c'. */
int get_utf8_char_len(unsigned char c) {
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1; // Fallback / Invalid
}

int isUnicodeSupported(void) {
  char *lang = getenv("LANG");
  if (lang && strstr(lang, "UTF-8")) return 1;
  if (lang && strstr(lang, "utf8")) return 1;

  char *lc = getenv("LC_ALL");
  if (lc && strstr(lc, "UTF-8")) return 1;
  if (lc && strstr(lc, "utf8")) return 1;

  return 0;
}

void freeDensityGlyphs(void) {
  if (E.density_glyphs) {
    for (int i = 0; i < E.density_count; i++) {
      free(E.density_glyphs[i]);
    }
    free(E.density_glyphs);
    E.density_glyphs = NULL;
    E.density_count = 0;
  }
}

void setDensityString(const char *str) {
  freeDensityGlyphs();

  // First pass: count glyphs
  int count = 0;
  const char *p = str;
  while (*p) {
    int len = get_utf8_char_len((unsigned char)*p);
    p += len;
    count++;
  }

  if (count == 0) return; // Should not happen with defaults

  E.density_count = count;
  E.density_glyphs = malloc(sizeof(char*) * count);

  // Second pass: store glyphs
  p = str;
  for (int i = 0; i < count; i++) {
    int len = get_utf8_char_len((unsigned char)*p);
    E.density_glyphs[i] = malloc(len + 1);
    memcpy(E.density_glyphs[i], p, len);
    E.density_glyphs[i][len] = '\0';
    p += len;
  }
}

void resolveDensityConfig(void) {
  if (strlen(E.density_arg) > 0) {
    if (strcmp(E.density_arg, "ascii-default") == 0) {
      setDensityString(DENSITY_ASCII_DEFAULT);
    } else if (strcmp(E.density_arg, "unicode-default") == 0) {
      setDensityString(DENSITY_UNICODE_DEFAULT);
    } else {
      setDensityString(E.density_arg);
    }
  } else {
    // Auto-detect
    if (isUnicodeSupported()) {
      setDensityString(DENSITY_UNICODE_DEFAULT);
    } else {
      setDensityString(DENSITY_ASCII_DEFAULT);
    }
  }
}

void initEditor(void) {
  E.mode = MODE_NETWORK;
  E.view_mode = VIEW_PIP;
  E.net_role = NET_ROLE_SERVER;
  E.net_port = 3000;
  E.list_cameras = 0;
  E.camera_target[0] = '\0';
  strcpy(E.net_ip, "127.0.0.1");

  E.density_glyphs = NULL;
  E.density_count = 0;
  E.density_arg[0] = '\0';

  updateWindowSize();
  signal(SIGWINCH, handleSigWinCh);
}

void initTerminal(void) {
  // Clear screen
  write(STDOUT_FILENO, "\x1b[2J", 4);
}

/* --- VIDEO TO GRAYSCALE TO ASCII ------------------------------------------ */

void renderStatus(struct abuf *ab) {
  int cols = E.screencols;
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;1H", E.screenrows + 1);
  abAppend(ab, buf, strlen(buf));
  abAppend(ab, "\x1b[0K", 4);
  int msglen = strlen(E.statusmsg);
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen <= cols ? msglen : cols);
}

// Helper to convert an image buffer (grayscale) to ASCII in the abuf
void renderBuffer(struct abuf *ab, unsigned char *pixels, int w, int h,
    int x_off, int y_off, int target_w, int target_h, int mirror) {
  if (target_w <= 0 || target_h <= 0) return;

  int min = 255, max = 0;
  int d_max = E.density_count - 1;

  /* Normalization (Find min/max) */
  for (int y = 0; y < target_h; y++) {
    for (int x = 0; x < target_w; x++) {
      int ix = mirror ? ((target_w-1-x)*w)/target_w : (x*w)/target_w;
      int iy = (y*h)/target_h;
      if (ix >= w) ix = w-1;
      if (iy >= h) iy = h-1;

      unsigned char v = pixels[iy * w + ix];
      if (v < min) min = v;
      if (v > max) max = v;
    }
  }
  int range = max - min;
  if (range == 0) range = 1;

  /* Rendering */
  for (int y = 0; y < target_h; y++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", y_off + y + 1, x_off + 1);
    abAppend(ab, buf, strlen(buf));

    for (int x = 0; x < target_w; x++) {
      int ix = mirror ? ((target_w-1-x)*w)/target_w : (x*w)/target_w;
      int iy = (y*h)/target_h;
      if (ix >= w) ix = w-1;
      if (iy >= h) iy = h-1;

      unsigned char v = pixels[iy * w + ix];
      if (E.density_count > 0) {
        int idx = (v - min) * d_max / range;
        if (idx < 0) idx = 0;
        if (idx > d_max) idx = d_max;
        abAppend(ab, E.density_glyphs[idx], strlen(E.density_glyphs[idx]));
      }
    }
  }
}

// Helper to convert an image buffer (BGRA) to ASCII in the abuf
void renderBufferBGRA(struct abuf *ab, unsigned char *pixels, int w, int h,
    int x_off, int y_off, int target_w, int target_h, int mirror) {
  if (target_w <= 0 || target_h <= 0) return;

  int min = 255, max = 0;
  int d_max = E.density_count - 1;

  /* Normalization */
  for (int y = 0; y < target_h; y++) {
    for (int x = 0; x < target_w; x++) {
      int ix = mirror ? ((target_w-1-x)*w)/target_w : (x*w)/target_w;
      int iy = (y*h)/target_h;
      if (ix >= w) ix = w-1;
      if (iy >= h) iy = h-1;

      int offset = (iy * w + ix) * 4;
      unsigned char r = pixels[offset+2];
      unsigned char g = pixels[offset+1];
      unsigned char b = pixels[offset+0];
      unsigned char v = (r*77 + g*150 + b*29) >> 8; // Luminance

      if (v < min) min = v;
      if (v > max) max = v;
    }
  }
  int range = max - min;
  if (range == 0) range = 1;

  /* Rendering */
  for (int y = 0; y < target_h; y++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", y_off + y + 1, x_off + 1);
    abAppend(ab, buf, strlen(buf));

    for (int x = 0; x < target_w; x++) {
      int ix = mirror ? ((target_w-1-x)*w)/target_w : (x*w)/target_w;
      int iy = (y*h)/target_h;
      if (ix >= w) ix = w-1;
      if (iy >= h) iy = h-1;

      int offset = (iy * w + ix) * 4;
      unsigned char r = pixels[offset+2];
      unsigned char g = pixels[offset+1];
      unsigned char b = pixels[offset+0];
      unsigned char v = (r*77 + g*150 + b*29) >> 8;

      if (E.density_count > 0) {
        int idx = (v - min) * d_max / range;
        if (idx < 0) idx = 0;
        if (idx > d_max) idx = d_max;
        abAppend(ab, E.density_glyphs[idx], strlen(E.density_glyphs[idx]));
      }
    }
  }
}

void renderAsciiFrame(struct abuf *ab, unsigned char *pixels, int w, int h) {
  abAppend(ab,"\x1b[?25l",6); /* Hide cursor. */
  abAppend(ab,"\x1b[H",3); /* Go home. */
  renderBuffer(ab, pixels, w, h, 0, 0, E.screencols, E.screenrows, 1);
  renderStatus(ab);
}

/* --- NETWORK MODE --------------------------------------------------------- */

// Downscale and grayscale conversion for network transport (Legacy/Unused)
// void resizeAndGray(frame *in, unsigned char *out) ...


int tcpListen(int port) {
  int server_fd, new_socket;
  struct sockaddr_in address;
  int opt = 1;
  int addrlen = sizeof(address);

  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  // Forcefully attach socket to the port
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address))<0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  if (listen(server_fd, 3) < 0) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  fprintf(stderr, "Waiting for connection on port %d... (Ctrl+C to quit)\n",
      port);

  // Wait for connection with Ctrl+C support
  while(1) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    FD_SET(server_fd, &readfds);

    if (select(server_fd + 1, &readfds, NULL, NULL, NULL) < 0) {
      if (errno == EINTR) continue;
      perror("select");
      exit(EXIT_FAILURE);
    }

    // Check for Ctrl+C
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
      char c;
      if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == CTRL_C) {
          write(STDOUT_FILENO, "\r\n", 2);
          exit(0);
        }
      }
    }

    // Check for Incoming Connection
    if (FD_ISSET(server_fd, &readfds)) {
      if ((new_socket = accept(server_fd,
              (struct sockaddr *)&address,
              (socklen_t*)&addrlen)) < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
      }
      break;
    }
  }

  fprintf(stderr, "Connected!\n");
  return new_socket;
}

int tcpConnect(const char *ip, int port) {
  int sock = 0;
  struct sockaddr_in serv_addr;

  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("Socket creation error");
    exit(EXIT_FAILURE);
  }

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);

  if(inet_pton(AF_INET, ip, &serv_addr.sin_addr)<=0) {
    perror("Invalid address/ Address not supported");
    exit(EXIT_FAILURE);
  }

  // Set non-blocking to handle Ctrl+C during connect
  fcntl(sock, F_SETFL, O_NONBLOCK);

  fprintf(stderr, "Connecting to %s:%d... (Ctrl+C to quit)\n", ip, port);

  int ret = connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
  if (ret < 0 && errno != EINPROGRESS) {
    perror("Connection Failed");
    exit(EXIT_FAILURE);
  }

  if (ret == 0) {
    // Connected immediately
    fprintf(stderr, "Connected!\n");
    return sock;
  }

  // Wait for connection completion
  while(1) {
    fd_set readfds, writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(STDIN_FILENO, &readfds);
    FD_SET(sock, &writefds);

    if (select(sock + 1, &readfds, &writefds, NULL, NULL) < 0) {
      if (errno == EINTR) continue;
      perror("select");
      exit(EXIT_FAILURE);
    }

    // Check for Ctrl+C
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
      char c;
      if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == CTRL_C) {
          write(STDOUT_FILENO, "\r\n", 2);
          exit(0);
        }
      }
    }

    // Check for Connect Result
    if (FD_ISSET(sock, &writefds)) {
      int so_error;
      socklen_t len = sizeof(so_error);
      getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);

      if (so_error == 0) {
        break; // Connected!
      } else {
        fprintf(stderr, "Connection failed: %s\n", strerror(so_error));
        exit(EXIT_FAILURE);
      }
    }
  }

  // Reset to blocking (or keep non-blocking as runNetworkMode sets it anyway)
  // fcntl(sock, F_SETFL, 0);

  fprintf(stderr, "Connected!\n");
  return sock;
}

long long current_timestamp(void) {
  struct timeval te;
  gettimeofday(&te, NULL);
  long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000;
  return milliseconds;
}

void redrawNetworkView(camera *cam, unsigned char *peer_pixels,
    int p_w, int p_h) {
  struct abuf ab = ABUF_INIT;
  abAppend(&ab,"\x1b[?25l",6); /* Hide cursor. */
  abAppend(&ab,"\x1b[H",3); /* Go home. */

  if (E.view_mode == VIEW_PIP) {
    /* Render Peer Fullscreen */
    renderBuffer(&ab, peer_pixels, p_w, p_h, 0, 0,
        E.screencols, E.screenrows, 1);

    /* Render Self Small (bottom right) */
    frame my_frame;
    if (cameraGetFrame(cam, &my_frame)) {
      int sw = E.screencols / 4;
      int sh = E.screenrows / 4;
      if (sw < 10) sw = 10;
      if (sh < 5) sh = 5;
      renderBufferBGRA(&ab, my_frame.pixels, my_frame.width, my_frame.height,
          E.screencols - sw - 2, E.screenrows - sh - 2, sw, sh, 1);
    }
  } else {
    /* Split Screen */
    int half_w = E.screencols / 2;
    renderBuffer(&ab, peer_pixels, p_w, p_h, 0, 0, half_w, E.screenrows, 1);

    frame my_frame;
    if (cameraGetFrame(cam, &my_frame)) {
      renderBufferBGRA(&ab, my_frame.pixels, my_frame.width, my_frame.height,
          half_w, 0, E.screencols - half_w, E.screenrows, 1);
    }
  }

  renderStatus(&ab);
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void runNetworkMode(camera *cam) {
  int sockfd;

  // Establish Connection
  if (E.net_role == NET_ROLE_SERVER) {
    sockfd = tcpListen(E.net_port);
  } else {
    sockfd = tcpConnect(E.net_ip, E.net_port);
  }

  // Set socket non-blocking
  fcntl(sockfd, F_SETFL, O_NONBLOCK);

  initTerminal();

  unsigned char *net_buffer = malloc(65536); // Max 255*255 + safety
  unsigned char *recv_buffer = malloc(132000); // 2 * max frame + safety
  int recv_len = 0;

  // State: Peer's last frame for redraws
  unsigned char *last_peer_pixels = malloc(65536);
  int last_peer_w = 0, last_peer_h = 0;

  // State: Resolution I want to receive (My Terminal)
  int my_w = E.screencols;
  int my_h = E.screenrows;
  if (my_w > 255) my_w = 255;
  if (my_h > 255) my_h = 255;

  // State: Resolution Peer wants to receive (Their Terminal)
  // Default to 80x60 until we hear otherwise
  int peer_w = 80;
  int peer_h = 60;

  // Send initial configuration to peer
  unsigned char init_conf[3] = {'C', (unsigned char)my_w, (unsigned char)my_h};
  write(sockfd, init_conf, 3);

  long long next_frame_time = current_timestamp();

  while (1) {
    long long now = current_timestamp();
    long long wait_ms = next_frame_time - now;
    if (wait_ms < 0) wait_ms = 0;

    // Check for Window Resize (I am the source of truth for what I want to see)
    if (E.screencols != my_w || E.screenrows != my_h) {
      // Clamp
      int new_w = E.screencols > 255 ? 255 : E.screencols;
      int new_h = E.screenrows > 255 ? 255 : E.screenrows;

      // Update state
      my_w = new_w;
      my_h = new_h;

      // Notify Peer
      unsigned char conf_pkt[3] = {'C',
        (unsigned char)my_w,
        (unsigned char)my_h};
      write(sockfd, conf_pkt, 3);
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    FD_SET(sockfd, &readfds);

    int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

    struct timeval tv;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;

    int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

    // Handle User Input
    if (activity > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
      char c;
      if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == CTRL_C) break;
        if (c == 'v' || c == 'V') {
          E.view_mode = (E.view_mode == VIEW_PIP) ? VIEW_SPLIT : VIEW_PIP;
          if (last_peer_w > 0)
            redrawNetworkView(cam, last_peer_pixels, last_peer_w, last_peer_h);
        }
      }
    }

    // Handle Network Receive
    if (activity > 0 && FD_ISSET(sockfd, &readfds)) {
      int n = read(sockfd, recv_buffer + recv_len,
          131000 - recv_len);
      if (n == 0) {
        // Connection closed
        editorSetStatusMessage("Connection closed by peer.");
        break;
      } else if (n > 0) {
        recv_len += n;

        // Process all complete packets in buffer
        while (recv_len >= 3) {
          unsigned char type = recv_buffer[0];
          int p_w = recv_buffer[1];
          int p_h = recv_buffer[2];

          int packet_size = 0;

          if (type == 'C') {
            packet_size = 3;
            if (recv_len >= packet_size) {
              // Handle Config
              if (p_w > 0 && p_h > 0) {
                peer_w = p_w;
                peer_h = p_h;
              }
            }
          } else if (type == 'P') {
            packet_size = 3 + (p_w * p_h);
            if (recv_len >= packet_size) {
              // Handle Picture
              last_peer_w = p_w;
              last_peer_h = p_h;
              memcpy(last_peer_pixels, recv_buffer + 3, p_w * p_h);
              redrawNetworkView(cam, last_peer_pixels,
                  last_peer_w, last_peer_h);
            } else {
              // Incomplete picture packet, wait for more data
              break;
            }
          } else {
            // Unknown packet / Desync?
            // Recover by skipping 1 byte (ugly but "robust" enough for a toy)
            memmove(recv_buffer, recv_buffer + 1, recv_len - 1);
            recv_len--;
            continue;
          }

          // If we processed a packet, remove it from buffer
          if (packet_size > 0 && recv_len >= packet_size) {
            recv_len -= packet_size;
            if (recv_len > 0) {
              memmove(recv_buffer, recv_buffer + packet_size, recv_len);
            }
          } else {
            // Incomplete packet (should have been caught above, but safety)
            break;
          }
        }
      }
    }

    // Send Frame (Rate Limited)
    now = current_timestamp();
    if (now >= next_frame_time) {
      frame frame;
      if (cameraGetFrame(cam, &frame)) {
        // Prepare Buffer for Resize (using peer's requested dimensions)
        int w = peer_w;
        int h = peer_h;
        int size = w * h;

        unsigned char header[3] = {'P', (unsigned char)w, (unsigned char)h};

        for (int y = 0; y < h; y++) {
          for (int x = 0; x < w; x++) {
            int ix = (x * frame.width) / w;
            int iy = (y * frame.height) / h;
            int offset = (iy * frame.width + ix) * 4;

            unsigned char b = frame.pixels[offset + 0];
            unsigned char g = frame.pixels[offset + 1];
            unsigned char r = frame.pixels[offset + 2];

            net_buffer[y * w + x] = (r*77 + g*150 + b*29) >> 8;
          }
        }

        write(sockfd, header, 3);
        write(sockfd, net_buffer, size);
      }
      next_frame_time = now + 33; // Target ~30 FPS
    }
  }

  free(net_buffer);
  free(recv_buffer);
  free(last_peer_pixels);
  close(sockfd);
}

/* --- MIRROR MODE ---------------------------------------------------------- */

void runMirrorMode(camera *cam) {
  frame frame;

  while (1) {
    /* Check for keypress to exit */
    struct timeval tv = {0, 0};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    if (select(STDIN_FILENO+1, &readfds, NULL, NULL, &tv) > 0) {
      char c;
      if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == CTRL_C) break;
      }
    }

    if (cameraGetFrame(cam, &frame)) {
      struct abuf ab = ABUF_INIT;

      if (E.screenrows <= 0 || E.screencols <= 0) {
        // Window too small, just sleep and continue
        struct timespec ts = {0, 33000000};
        nanosleep(&ts, NULL);
        continue;
      }

      abAppend(&ab,"\x1b[?25l",6); /* Hide cursor. */
      abAppend(&ab,"\x1b[H",3); /* Go home. */

      renderBufferBGRA(&ab, frame.pixels, frame.width, frame.height,
          0, 0, E.screencols, E.screenrows, 1);

      renderStatus(&ab);

      // Write buffer to stdout and free
      write(STDOUT_FILENO, ab.b, ab.len);
      abFree(&ab);
    }

    // 33ms ~ 30fps
    struct timespec ts = {0, 33000000};
    nanosleep(&ts, NULL);
  }
}

/* --- CONFIG TUI ----------------------------------------------------------- */

// Simple text input in raw mode
void ttyInput(const char *prompt, char *buffer, int maxlen) {
  int len = strlen(buffer);

  while(1) {
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[H\x1b[2J", 7);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s\r\n\r\n > %s", prompt, buffer);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);

    int c = editorReadKey(STDIN_FILENO);

    if (c == ENTER) {
      break;
    } else if (c == BACKSPACE || c == CTRL_H || c == DEL_KEY) {
      if (len > 0) {
        len--;
        // If it's a UTF-8 continuation byte, keep going back
        while (len > 0 && (buffer[len] & 0xC0) == 0x80) len--;
        buffer[len] = '\0';
      }
    } else if (!iscntrl(c) || (unsigned char)c >= 128) {
      if (len < maxlen - 1) {
        buffer[len] = c;
        len++;
        buffer[len] = '\0';
      }
    } else if (c == CTRL_C) {
      // Cancel input
      break;
    }
  }
  write(STDOUT_FILENO, "\x1b[?25l", 6); // Hide cursor again
}

int ttyMenu(const char *prompt, const char **options, int count) {
  int selected = 0;
  while(1) {
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6); /* Hide cursor */
    abAppend(&ab, "\x1b[H\x1b[2J", 7); /* Clear */

    char buf[256];
    snprintf(buf, sizeof(buf), "%s\r\n\r\n", prompt);
    abAppend(&ab, buf, strlen(buf));

    for (int i = 0; i < count; i++) {
      if (i == selected) {
        abAppend(&ab, "\x1b[7m", 4); // Invert
        snprintf(buf, sizeof(buf), " > %s \r\n", options[i]);
        abAppend(&ab, buf, strlen(buf));
        abAppend(&ab, "\x1b[0m", 4);
      } else {
        snprintf(buf, sizeof(buf), "   %s \r\n", options[i]);
        abAppend(&ab, buf, strlen(buf));
      }
    }

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);

    int c = editorReadKey(STDIN_FILENO);

    if (c == ESC || c == 'q' || c == CTRL_C) {
      write(STDOUT_FILENO, "\x1b[2J", 4);
      exit(0);
    }

    switch(c) {
      case ARROW_UP:
        if (selected > 0) selected--;
        break;
      case ARROW_DOWN:
        if (selected < count - 1) selected++;
        break;
      case ENTER:
        return selected;
    }
  }
}

void parseIpPortString(char *str) {
  char *colon = strchr(str, ':');
  if (colon) {
    *colon = '\0';
    strncpy(E.net_ip, str, 63);
    E.net_port = atoi(colon + 1);
  } else {
    strncpy(E.net_ip, str, 63);
  }
}

void configureTUI(void) {
  const char *mode_opts[] = {
    "[Mirror Mode] See yourself in the mirror.",
    "[Network Mode] Call a friend: create a room or join a call."
  };
  int mode = ttyMenu("Select Mode:", mode_opts, 2);

  if (mode == 0) {
    E.mode = MODE_MIRROR;
  } else {
    E.mode = MODE_NETWORK;
    const char *role_opts[] = {
      "[create new room]",
      "[join room]"
    };
    int role = ttyMenu("Select Role:", role_opts, 2);

    if (role == 0) {
      E.net_role = NET_ROLE_SERVER;
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", E.net_port);
      ttyInput("Enter Port:", buf, 31);
      E.net_port = atoi(buf);
    } else {
      E.net_role = NET_ROLE_CLIENT;
      char buf[128];
      snprintf(buf, sizeof(buf), "%s:%d", E.net_ip, E.net_port);
      ttyInput("Enter IP:PORT:", buf, 127);
      parseIpPortString(buf);
    }
  }

  // Camera Selection
  CameraInfo *list = enumerateCameras();
  int count = 0;
  while (list[count].name[0]) count++;

  const char **cam_opts = malloc(sizeof(char*) * count);
  for (int i = 0; i < count; i++) {
    cam_opts[i] = list[i].name;
  }

  int cam_idx = ttyMenu("Select Camera:", cam_opts, count);
  strcpy(E.camera_target, list[cam_idx].id);

  free(cam_opts);
  freeCameraList(list);

  // Density Selection
  const char *density_opts[] = {
    "[ascii default]   (" DENSITY_ASCII_DEFAULT ")",
    "[unicode default] (" DENSITY_UNICODE_DEFAULT ")",
    "[custom]"
  };
  int dens = ttyMenu("Select Density String:", density_opts, 3);
  if (dens == 0) {
    strcpy(E.density_arg, "ascii-default");
  } else if (dens == 1) {
    strcpy(E.density_arg, "unicode-default");
  } else {
    ttyInput("Enter Density String (dark to light):", E.density_arg, 255);
  }
}

/* --- MAIN ----------------------------------------------------------------- */

int main(int argc, char **argv) {
  initEditor();

  camera cam;

  if (argc > 1) {
    parse_config_args(argc, argv);

    if (E.list_cameras) {
      CameraInfo *list = enumerateCameras();
      fprintf(stdout, "Available Cameras:\n");
      for (int i = 0; list[i].name[0]; i++) {
        fprintf(stdout, "  %s (ID: %s)\n", list[i].name, list[i].id);
      }
      freeCameraList(list);
      exit(0);
    }

    initTerminal();
    enableRawMode(STDIN_FILENO);
  } else {
    initTerminal();
    enableRawMode(STDIN_FILENO);
    configureTUI();
  }

  resolveDensityConfig();
  cameraInit(&cam, 640, 480);
  cameraStart(&cam);

  char *mode_str = (E.mode == MODE_MIRROR) ? "mirror" : "network";
  editorSetStatusMessage("HELP: Ctrl-C = quit | 'v' = toggle view | mode: %s",
      mode_str);

  if (E.mode == MODE_MIRROR) {
    runMirrorMode(&cam);
  } else if (E.mode == MODE_NETWORK) {
    runNetworkMode(&cam);
  }

  return 0;
}
