/*
 * Picturephone --- Ascii video-conferencing in the terminal.
 * -----------------------------------------------------------------------
 *
 * Credits and inspiration: antirez/kilo, ertdfgcvb/play.core, kubrick/2001.
 *
 * Author:                  Daniel Falbo <hi at danielfalbo dot com>.
 *
 * */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <pthread.h>

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
} camera;

// Initialize the camera (select default device)
void cameraInit(camera *cam, int width, int height);

// Start capturing
void cameraStart(camera *cam);

// Copy the latest frame safely into the target buffer.
// Returns 1 if a new frame was available, 0 otherwise.
int cameraGetFrame(camera *cam, frame *outFrame);

/* --- DEVICE/OS WEBCAM IMPLEMENTATIONS ------------------------------------- */

#ifdef __linux__
// TODO: Implement Linux/V4L2 support

void cameraInit(camera *cam, int width, int height) {
  (void)cam; (void)width; (void)height;
  // Stub
}

void cameraStart(camera *cam) {
  (void)cam;
  // Stub
}

int cameraGetFrame(camera *cam, frame *outFrame) {
  (void)cam; (void)outFrame;
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

// Context to pass into the delegate callback
static camera *global_cam_context = NULL;

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
  id mediaType = ((id_msg_str_t)objc_msgSend)(
      (id)objc_getClass("NSString"),
      sel_registerName("stringWithUTF8String:"),
      "vide" // "vide" = AVMediaTypeVideo
      );

  id device = ((id_msg_id_t)objc_msgSend)(
      AVCaptureDevice,
      sel_registerName("defaultDeviceWithMediaType:"),
      mediaType
      );

  if (!device) {
    fprintf(stderr, "Error: No video device found.\n");
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
  id session = (id)cam->internal;
  ((void_msg_ptr_t)objc_msgSend)(session,
    sel_registerName("startRunning"),
    NULL);
  cam->isRunning = 1;
}

int cameraGetFrame(camera *cam, frame *outFrame) {
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

#define KILO_VERSION "0.0.1"

struct editorConfig;

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
  int idx;            /* Row index in the file, zero-based. */
  int size;           /* Size of the row, excluding the null term. */
  int rsize;          /* Size of the rendered row. */
  char *chars;        /* Row content. */
  char *render;       /* Row content "rendered" for screen (for TABs). */
  unsigned char *hl;  /* Syntax highlight type for each character in render.*/
  int hl_oc;          /* Row had open comment at end in last syntax highlight
                         check. */
} erow;

#define MODE_MIRROR 0
#define MODE_NETWORK 1

struct editorConfig {
  int cx,cy;  /* Cursor x and y position in characters */
  int rowoff;     /* Offset of row displayed. */
  int coloff;     /* Offset of column displayed. */
  int screenrows; /* Number of rows that we can show */
  int screencols; /* Number of cols that we can show */
  int numrows;    /* Number of rows */
  int rawmode;    /* Is terminal raw mode enabled? */
  erow *row;      /* Rows */
  int dirty;      /* File modified but not saved. */
  char *filename; /* Currently open filename */
  char statusmsg[80];
  time_t statusmsg_time;
  int mode;       /* Application mode */
};

static struct editorConfig E;

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
  if (E.cy > E.screenrows) E.cy = E.screenrows - 1;
  if (E.cx > E.screencols) E.cx = E.screencols - 1;
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.mode = MODE_NETWORK;
  updateWindowSize();
  signal(SIGWINCH, handleSigWinCh);
}

void initTerminal(void) {
  // Clear screen
  write(STDOUT_FILENO, "\x1b[2J", 4);
}

/* --- VIDEO TO GRAYSCALE TO ASCII ------------------------------------------ */

#define DENSITY_STR " .x?A@"
#define DENSITY_LEN (sizeof(DENSITY_STR) - 1)

/* --- NETWORK MODE --------------------------------------------------------- */

void runNetworkMode(camera *cam) {
  frame frame;
  cameraGetFrame(cam, &frame);

  initTerminal();
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

      abAppend(&ab,"\x1b[?25l",6); /* Hide cursor. */
      abAppend(&ab,"\x1b[H",3); /* Go home. */

      unsigned char *p = frame.pixels;
      int rows = E.screenrows;
      int cols = E.screencols;

      for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
          // Map Terminal Coordinate (x,y) -> Image Coordinate (ix, iy)
          // To Mirror: Scan Image X backwards relative to Terminal X
          int ix = ((cols - 1 - x) * frame.width) / cols;
          int iy = (y * frame.height) / rows;

          // Clamp just in case integer math goes wild
          if (ix >= frame.width) ix = frame.width - 1;
          if (iy >= frame.height) iy = frame.height - 1;

          // Get Pixel (BGRA)
          int offset = (iy * frame.width + ix) * 4;
          unsigned char b = p[offset + 0];
          unsigned char g = p[offset + 1];
          unsigned char r = p[offset + 2];

          // Our grayscale conversion algorithm is very simple:
          // we just grab the average of the RGB channels.
          int intensity = (r+g+b)/3;

          // Map to Char
          int char_idx = (intensity * (DENSITY_LEN - 1)) / 255;
          abAppend(&ab, &DENSITY_STR[char_idx], 1);
        }
        abAppend(&ab, "\r\n", 2);
      }

      // Render status message
      abAppend(&ab, "\x1b[0K", 4);
      int msglen = strlen(E.statusmsg);
      if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(&ab, E.statusmsg, msglen <= cols ? msglen : cols);

      // Write buffer to stdout and free
      write(STDOUT_FILENO, ab.b, ab.len);
      abFree(&ab);
    }

    // 33ms ~ 30fps
    struct timespec ts = {0, 33000000};
    nanosleep(&ts, NULL);
  }
}

/* --- RUN CONFIG ----------------------------------------------------------- */

/* Parse the arguments to determine the config of this run.
 * Panics on invalid config. */
void parse_arguments(int argc, char **argv) {
  // When called with no arguments, default to network mode.
  if (argc == 1) {
    E.mode = MODE_NETWORK;
    return;
  }

  if (argc == 2) {
    if (strcmp(argv[1], "--mirror") == 0) {
      E.mode = MODE_MIRROR; // Mirror mode
      return;
    } else if (strcmp(argv[1], "--network") == 0) {
      E.mode = MODE_NETWORK; // Network mode
      return;
    }
  }

  // Show usage string.
  fprintf(stderr, "Usage: %s [--help | --mirror | --network]\n", argv[0]);

  // Exit with err code 0 if program was called with --help, 1 otherwise.
  if (argc == 2 && strcmp(argv[1], "--help") == 0) {
    exit(0);
  } else {
    exit(1);
  }
}

/* --- MAIN ----------------------------------------------------------------- */

int main(int argc, char **argv) {
  initEditor();
  parse_arguments(argc, argv);

  camera cam;
  cameraInit(&cam, 640, 480);
  cameraStart(&cam);

  initTerminal();
  enableRawMode(STDIN_FILENO);

  char *mode_str = (E.mode == MODE_MIRROR) ? "mirror" : "network";
  editorSetStatusMessage("HELP: Ctrl-C = quit | starting in %s mode", mode_str);

  if (E.mode == MODE_MIRROR) {
    runMirrorMode(&cam);
  } else if (E.mode == MODE_NETWORK) {
    runNetworkMode(&cam);
  }

  return 0;
}
