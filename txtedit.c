/*
 * txtedit.c
 *
 * Simple text editor in C
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define APPEND_BUF_INIT {NULL, 0}

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

struct editor_config {
    int cursor_x;
    int cursor_y;
    int screen_rows;
    int screen_cols;
    struct termios og_termios; /* original terminal settings */
};

struct editor_config ecfg;

struct append_buf {
  char *buf;
  int len;
};

/* display error message */
void display_error(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    
    perror(s);
    exit(1);
}

/* disable raw mode */
void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ecfg.og_termios) == -1) {
        display_error("tcsetattr");
    };
}

/* set terminal to raw mode */
void enable_raw_mode()
{
    if (tcgetattr(STDIN_FILENO, &ecfg.og_termios) == -1) {
        display_error("tcgetattr");
    };
    atexit(disable_raw_mode);

    struct termios raw = ecfg.og_termios;

    /* disable flags */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        display_error("tcsetattr");
    };
    
}

/* read keypresses from user */
int read_keypress()
{
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            display_error("read");
        }
    };

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }

        return '\x1b';
    }
    else {
        return c;
    }
}

/* get cursor position */
/* returns 0 on success, -1 on error */
int get_cursor_pos(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

/* get terminal size */
/* returns 0 on success, -1 on error */
int get_window_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != -12) {
            return -1;
        }

        return get_cursor_pos(rows, cols);
    }
    else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;

        return 0;
    }
}

/* fill values in append buffer */
void ab_append(struct append_buf *ab,  const char *s, int len) {
    char *new = realloc(ab->buf, ab->len + len);
    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->buf = new;
    ab->len += len;
}

/* free append buffer */
void ab_free(struct append_buf *ab) {
    free(ab->buf);
}

/* draw a column of tildes on the left side of the screen */
void draw_rows(struct append_buf *ab)
{
    for (int i = 0; i < ecfg.screen_rows; i++) {
        /* display welcome message in the middle of the screen */
        if (i == ecfg.screen_rows / 3) {
            char welcome[80];
            int welcome_len = snprintf(welcome, sizeof(welcome),
                    "Welcome to txtedit! Press Ctrl+q to quit.");
            if (welcome_len > ecfg.screen_cols) {
                welcome_len = ecfg.screen_cols;
            }
            int padding = (ecfg.screen_cols - welcome_len) / 2;
            if (padding) {
                ab_append(ab, "~", 1); /* tilde */
                padding--;
            }
            while (padding--) {
                ab_append(ab, " ", 1);
            }
            ab_append(ab, welcome, welcome_len);
        }
        else {
            ab_append(ab, "~", 1); /* tilde */
        }

        ab_append(ab, "\x1b[K", 3);
        if (i < ecfg.screen_rows - 1) {
            ab_append(ab, "\r\n", 2);
        }
    }
}

/* refresh the screen */
void refresh_screen()
{
    struct append_buf ab = APPEND_BUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    draw_rows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ecfg.cursor_y + 1, ecfg.cursor_x + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    ab_free(&ab);
}

/* move the cursor using the arrow keys */
void move_cursor(int key) {
    switch (key) {
        case ARROW_UP:
            if (ecfg.cursor_y != 0) {
                ecfg.cursor_y--;
            }
            break;
        case ARROW_DOWN:
            if (ecfg.cursor_y != ecfg.screen_rows - 1) {
                ecfg.cursor_y++;
            }
            break;
        case ARROW_LEFT:
            if (ecfg.cursor_x != 0) {
                ecfg.cursor_x--;
            }
            break;
        case ARROW_RIGHT:
            if (ecfg.cursor_x != ecfg.screen_cols - 1) {
                ecfg.cursor_x++;
            }
            break;
    }
}

/* process user keypresses */
void process_keypress()
{
    int c = read_keypress();

    switch (c) {
        /* quit when Ctrl+q is pressed */
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        case HOME_KEY:
            ecfg.cursor_x = 0;
            break;
        case END_KEY:
            ecfg.cursor_x = ecfg.screen_cols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = ecfg.screen_rows;
                while (times--) {
                    move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
        
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            move_cursor(c);
            break;
    }
}

/* initialize editor */
void init_editor()
{
    ecfg.cursor_x = 0;
    ecfg.cursor_y = 0;
    if (get_window_size(&ecfg.screen_rows, &ecfg.screen_cols) == -1) {
        display_error("get_window_size");
    }
}

int main()
{
    enable_raw_mode();
    init_editor();

    while(1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}