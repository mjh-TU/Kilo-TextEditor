//
//
/************* includes *************/
//
//

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#define KILO_QUIT_TIMES 3

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

//
//
/************* defines *************/
//
//
#define KILO_TAB_STOP 8
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
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

// Typedef allows us to refer to the type as erow instead of struct erow.
// Data type for storing a row of text in our editor. (erow) = editor row
typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;


struct termios orig_termios;

struct editorConfig {
    int cx, cy;
    int rx;
    // vertical scrolling
    int rowoff;
    // horizontal scrolling
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    // Make E.row an array of erow structs. That way we can store multiple lines and will be a dynamically allocated array, so we'll make it a pointer to erow and initialize the pointer to NULL.
    erow *row;
    // We call a text buffer "dirty" if it has been modified since opening or saving the file
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
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
/************* prototypes *************/
//
//

// To fix error because we were trying to call the function before it was defined so we declare this function here which allows us to call the function before its defined
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));



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
/************* row operations *************/
//
//


int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        // For each character, if it's a tab we use rx % KILO_TAB_STOP to find out how many columns we are to the right of the last tab stop, and then subtract that from KILO_TAB_STOP -1 to find out how many columns we are to the left of the next tab stop.
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP -1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP -1) - (cur_rx %KILO_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    return cx;
}

// Function that uses the chars string of an erow to fill in the contents of the render string. We'll copy each character from chars to render. We won't worry about how to render tabs yet.
void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;
    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP -1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}



void editorInsertRow(int at, char *s, size_t len) {

    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    if (at < 0 || at >- E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}


// Insert a single character into erow, at a given position
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

// When backspacing take current line and copy it to the previous line
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}


//
//
/************* editor operations *************/
//
//

void editorInsertChar(int c) {
    // If cursor is on the tilde line after the end of the file, so append a new row to the file before isserting a character.
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewLine() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    if (E.cy == E.numrows) return;
    // If cursor at begining of first line there is nothing to do
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}


//
//
/************* file i/o *************/
//
//

// Function that converts our array of erow structs into a single string that is ready to be written to a file
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}



// Take a filename and opens the file for reading using fopen. Allow the user to choose a file by passing a filename via cli argument. If they did call editorOpen, if not editorOpen will not be called and they will start with a blank file.
void editorOpen(char *filename) {

    free(E.filename);
    // strdup() makes copy of the given string, allocating the required memory and assuming you will free() that memory. We initialize E.filename to NULL pointer and it will stay NULL if a file isn't opened.
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // GetLine is usedful for reading lines from a file when we don't know how much memory to allocate for each line. Takes care of memory management for you. We pass it a line pointer and linecap (line capacity) of 0. Makes it allocate new memory for the next line it reads and set line to point to the memory and set linecap to let you know how much memory it allocated.
    // Add a while loop so editorOpen to read an entire file into E.row
    // While loop works because getline() returns -1 when it gets to the end of the file
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
          linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}


void editorSave() {
    // If it's a new file then E.filename will be NULL and won't know where to save (will fix later)
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);
    // Tell open to create a new file it not already exists (O_CREAT) and pass extra argument containing the mode (permissions) the new file should have
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

    // Both open() and ftruncate() both return -1 on error. Whether or not an error occured, we ensure that the file is closed and the memory that buf points to is freed
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}



//
//
/************* find *************/
//
//

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for (i=0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        erow *row = &E.row[current];
        // Use strstr to check if query is a substring of the current row. Returns NULL if no match
        char *match = strstr(row->render, query);
        // If found move to that spot
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            break;
        }
    }
}


void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
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

// Check if cursor has moved outside the visible window and if so adjust E.rowoff so that the cursor is just inside the visible window.
void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.cx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}


// Draws a ~ in each row, which means that row is not part of the file and can't contain any text
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        // Wrap our previosu row-draing code in an if that checks whether we are currently drawing a row that is part of the text buffer, or a row that comes after the end of the text buffer.
        if (filerow >= E.numrows) {
            // Only display welcome message if no argument given to specify file
            if (E.numrows == 0 && y == E.screenrows / 3) {
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
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }
        

        abAppend(ab, "\x1b[K", 3);

        // If last line don't create new line
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    // Make room for a second line beneath our status bar where we will display the message
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
    editorScroll();
    struct abuf ab = ABUF_INIT;

    // Hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    
    // Reposition cursor at top left of screen
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // Move cursor to the position stored in E.cx and E.cy
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);


    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// '...' argument makes the function a variadic function, meaning it can take any number of arguments. C's way of dealing with these arguments is by having you call va_start() and va_end() on a value of type va_list.
// the last argument before the '...' (fmt) must be passed to va_start(), so that the address of the next arguments is known.
// then between the va_start() and va_end() calls, you would call a va_arg() and pass it the type of the next argument and it would return the value of that argument.
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // vsnprintf helps us make our own printf() style function. Store the resulting string in E.statusmsg and set E.statusmsg_time to the current time.
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

//
//
/************* input *************/
//
//

// Function to display a prompt in the status bar and lets the suer input a line of text after the prompt
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();

        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}



void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
      case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
      case ARROW_RIGHT:
        if (row && E.cx < row->size) {
            E.cx++;
        } else if (row && E.cx == row->size) {
            E.cy++;
            E.cx = 0;
        }
        break;
      case ARROW_UP:
        if (E.cy != 0) {
            E.cy--;
        }
        break;
      case ARROW_DOWN:
        // Not allow cursor to advance past the bottom of screen
        if (E.cy < E.numrows) {
            E.cy++;
        }
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}


void editorProcessKeypress() {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();
    switch (c) {

        // Enter Key
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. " "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case BACKSPACE:
        // Backspace character (original ctrl h back in old days)
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

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

        // Ctrl - l is originally to refresh screen but our editor does that already
        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            // If keyboard press not mapped to something write to file
            editorInsertChar(c);
            break;
    }
    quit_times = KILO_QUIT_TIMES;
}

//
//
/************* init *************/
//
//

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    // Keep track of scroller to view lines. Default to 0 so scrolled to top by default
    E.rowoff = 0;
    E.coloff = 0;
    // For now editor will only display a single line of text, and so numrows can be either 0 or 1
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    // Initialize E.statusmsg to an empty string so the message will be displayed by default
    E.statusmsg[0] = '\0';
    // E.statusmsg_time will contain the timestamp when e set a status message.
    E.statusmsg_time = 0;
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -=2;
}

int main(int argc, char *argv[]) {
    // Disbale the echo feature
    enableRawMode();
    initEditor();
    // If arguments to specify a file to edit
    if (argc >= 2) {
        // EditorOpen will eventually be for opening and reading a file from disk so we put in a new file i/o section
        editorOpen(argv[1]);
    }
    
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    // Read STDIN and save to char c variable. If variable is q then quit. Runs infinitely.
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    // Default return code for C which represents a successful run
    return 0;
}