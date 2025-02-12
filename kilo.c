/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/
struct termios orig_termios;

// Die function to print an error message and exit program
void die(const char *s) {
    perror(s);
    exit(1);
}

/*** terminal ***/
void disableRawMode() {
    // Set terminal attributes back to normal with orig_termios
    // You may notice that leftover input is no longer fed back to the shell. TCSAFLUSH below takes care of that
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
}

// Disable characters being printed after 'q' which quits the program
void enableRawMode() {
    
    // Gather terminal attributes. orig_termios is the original terminal attributes.
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    // Register out disableRawMode to be called automatically when the program exits
    atexit(disableRawMode);

    
    struct termios raw = orig_termios;

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

/*** init ***/
int main() {
    // Disbale the echo feature
    enableRawMode();

    // Read STDIN and save to char c variable. If variable is q then quit. Runs infinitely.
    while (1) {
        char c = '\0';
        read(STDIN_FILENO, &c, 1);
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
        // If character c is a control character. Character that wont't print to screen
        if (iscntrl(c)) {
            // Format the byte as a decimal number (ascii).
            printf("%d\r\n", c);
        }
        else {
            // Format byte as a decimal number (ascii) and %c writes byte as a character
            printf("%d ('%c')\r\n", c, c);
        }
        if (c == 'q') break;
    }

    // Default return code for C which represents a successful run
    return 0;
}