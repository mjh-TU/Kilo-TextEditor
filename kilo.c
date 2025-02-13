//
//
/************* includes *************/
//
//
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

//
//
/************* defines *************/
//
//

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
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

//
//
/************* data *************/
//
//

struct termios orig_termios;

struct editorConfig {
    int cx, cy;
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

// Die function to print an error message and exit program
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

//
//
/************* terminal *************/
//
//

void disableRawMode() {
    // Set terminal attributes back to normal with orig_termios
    // You may notice that leftover input is no longer fed back to the shell. TCSAFLUSH below takes care of that
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

// Disable characters being printed after 'q' which quits the program
void enableRawMode() {
    
    // Gather terminal attributes. orig_termios is the original terminal attributes.
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    // Register out disableRawMode to be called automatically when the program exits
    atexit(disableRawMode);

    
    struct termios raw = E.orig_termios;

    // Modify the terminal attributes. Echo is a bitflag, we use '~' to get the opposite bit then bitwise-AND this value with the flags field forcing the fourth bit to become `0` and every other bit to change back to its original value.
    // ICANON flag reads input byte-by-byte not line-by-line
    // IEXTEN disables Ctrl-V
    // ISIG Disables Ctrl-C and Ctrl-Z signals
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // IXON Disables Ctrl-S and Ctrl-Q
    // ICRNL fix's Ctrl-M
    // ISTRIP causes the 8th bit of each input byte to be stripped.
    // INPCK enables parity checking
    // BRKINT a break condition will cause SIGINT signal to be sent to the program. Like Ctrl-C
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // Turn off output processing. 
    raw.c_oflag &= ~(OPOST);
    // Sets the character size (CS) to 8 bits byte.
    raw.c_cflag |= (CS8);

    // VMIN value sets minimum number of bytes of input needed before read() can return
    raw.c_cc[VMIN] = 0;
    // VTIME value sets the maximum amount of time to wait before read() returns.
    raw.c_cc[VTIME] = 1;

    // Pass modified struct to write the new terminal attributes back out
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}


int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[' ) {

            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }
    else {
        return c;
    }
}


int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
  }

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        // On success ioctl() will place number of columns wide and number of rows high the terminal is given to winsize struct.
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

//
//
/************* append buffer *************/
//
//


struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

// Instead of using our write function multiple times append data to dynamic string and then finally write output to terminal. Creating our own dynamic string using the heap.
void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
  }
  void abFree(struct abuf *ab) {
    free(ab->b);
  }



//
//
/************* output *************/
//
//

// Draws a ~ in each row, which means that row is not part of the file and can't contain any text
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {

        if (y == E.screenrows / 3) {
            // Create welcome message
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
            if (welcomelen > E.screencols) welcomelen = E.screencols;

            // Center message
            int padding = (E.screencols - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            } 
            while (padding--) abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomelen);
        } 
        else {
            abAppend(ab, "~", 1);
        }

        abAppend(ab, "\x1b[K", 3);

        // If last line don't create new line
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}


void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    // Hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    
    // Reposition cursor at top left of screen
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // Move cursor to the position stored in E.cx and E.cy
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);


    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

//
//
/************* input *************/
//
//

void editorMoveCursor(int key) {
    switch (key) {
      case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        }
        break;
      case ARROW_RIGHT:
        if (E.cx != E.screencols - 1) {
            E.cx++;
        }
        break;
      case ARROW_UP:
        if (E.cy != 0) {
            E.cy--;
        }
        break;
      case ARROW_DOWN:
        if (E.cy != E.screenrows - 1) {
            E.cy++;
        }
        break;
    }
}


void editorProcessKeypress() {
    int c = editorReadKey();
    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screencols - 1;
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

//
//
/************* init *************/
//
//

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    // Disbale the echo feature
    enableRawMode();
    initEditor();

    // Read STDIN and save to char c variable. If variable is q then quit. Runs infinitely.
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    // Default return code for C which represents a successful run
    return 0;
}