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

struct editor_config {
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
char read_keypress()
{
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            display_error("read");
        }
    };

    return c;
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
        if (i == ecfg.screen_rows / 3) {
            char welcome[80];
            int welcome_len = snprintf(welcome, sizeof(welcome),
                    "Welcome to txtedit! Press Ctrl+q to quit."); /* welcome message */
            if (welcome_len > ecfg.screen_cols) {
                welcome_len = ecfg.screen_cols;
            }
            int padding = (ecfg.screen_cols - welcome_len) / 2;
            if (padding) {
                ab_append(ab, "~", 1);
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

    ab_append(&ab, "\x1b[H", 3);
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    ab_free(&ab);
}

/* process user keypresses */
void process_keypress()
{
    char c = read_keypress();

    switch (c) {
        /* quit when Ctrl+q is pressed */
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}

/* initialize editor */
void init_editor()
{
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