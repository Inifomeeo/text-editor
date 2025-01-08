/*
 * txtedit.c - Simple text editor in C
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TAB_STOP 8
#define QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)
#define APPEND_BUF_INIT {NULL, 0}

#define _BSD_SOURCE
#define _GNU_SOURCE

enum EditorKeys {
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

typedef struct EditorRow {
    int size;
    int rsize;
    char *chars;
    char *render;
} ERow;

struct EditorConfig {
    int cursor_x;
    int cursor_y;
    int rx;
    int row_offset;
    int col_offset;
    int screen_rows;
    int screen_cols;
    int num_rows;
    ERow *rows;
    int dirty;
    char *filename;
    char status_msg[80];
    time_t status_msg_time;
    struct termios og_termios; /* original terminal settings */
};

struct EditorConfig E;

struct AppendBuf {
  char *buf;
  int len;
};

void set_status_message(const char *fmt, ...);

/* display error message */
void display_error(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    
    perror(s);
    exit(1);
}

/* disable raw mode in terminal */
void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.og_termios) == -1) {
        display_error("tcsetattr");
    };
}

/* set terminal to raw mode */
void enable_raw_mode()
{
    if (tcgetattr(STDIN_FILENO, &E.og_termios) == -1) {
        display_error("tcgetattr");
    };
    atexit(disable_raw_mode);

    struct termios raw = E.og_termios;

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

/*
* read keypresses from user
* returns: the ASCII code of the keypress
*/
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

/*
* get cursor position
* returns: 0 on success, -1 on error
*/
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

/*
* get terminal size
* returns: 0 on success, -1 on error
*/
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

/*
* convert chars index to render index
* returns: render index
*/
int cx_to_rx(ERow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}

/* render tabs as spaces */
void editor_update_row(ERow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);
    
    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        }
        else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

/* append lines of text to the editor */
void editor_append_row(char *s, size_t len)
{
    E.rows = realloc(E.rows, sizeof(ERow) * (E.num_rows + 1));

    int at = E.num_rows;
    E.rows[at].size = len;
    E.rows[at].chars = malloc(len + 1);
    memcpy(E.rows[at].chars, s, len);
    E.rows[at].chars[len] = '\0';

    E.rows[at].rsize = 0;
    E.rows[at].render = NULL;
    editor_update_row(&E.rows[at]);

    E.num_rows++;
    E.dirty++;
}

/* insert a character into a row at a given index */
void row_insert_char(ERow *row, int at, int c)
{
    if (at < 0 || at > row->size) {
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

/* insert a character into the position of the cursor */
void insert_char(int c) {
    if (E.cursor_y == E.num_rows) {
        editor_append_row("", 0);
    }
    row_insert_char(&E.rows[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

/* convert rows to a single string */
char *rows_to_string(int *buflen)
{
    int totlen = 0;
    int j;
    for (j = 0; j < E.num_rows; j++) {
        totlen += E.rows[j].rsize + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.num_rows; j++) {
        memcpy(p, E.rows[j].chars, E.rows[j].size);
        p += E.rows[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

/* open and read a file from disk */
void editor_open(char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        display_error("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editor_append_row(line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

/* save a file to disk */
void editor_save() {
    if (E.filename == NULL) return;

    int len;
    char *buf = rows_to_string(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                set_status_message("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    set_status_message("Can't save file! I/O error: %s", strerror(errno));
}

/* fill values in append buffer */
void ab_append(struct AppendBuf *ab,  const char *s, int len)
{
    char *new = realloc(ab->buf, ab->len + len);
    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->buf = new;
    ab->len += len;
}

/* free append buffer */
void ab_free(struct AppendBuf *ab)
{
    free(ab->buf);
}

/* prevent the cursor from going off the screen */
void editor_scroll()
{
    E.rx = 0;
    if (E.cursor_y < E.num_rows) {
        E.rx = cx_to_rx(&E.rows[E.cursor_y], E.cursor_x);
    }

    if (E.cursor_y < E.row_offset) {
        E.row_offset = E.cursor_y;
    }
    if (E.cursor_y >= E.row_offset + E.screen_rows) {
        E.row_offset = E.cursor_y - E.screen_rows + 1;
    }
    if (E.rx < E.col_offset) {
        E.col_offset = E.rx;
    }
    if (E.rx >= E.col_offset + E.screen_cols) {
        E.col_offset = E.rx - E.screen_cols + 1;
    }
}

/* draw a column of tildes on the left side of the screen */
void draw_rows(struct AppendBuf *ab)
{
    int i;
    for (i = 0; i < E.screen_rows; i++) {
        int file_row = i + E.row_offset;
        if (file_row >= E.num_rows) {
            /* display welcome message in the middle of the screen */
            if (E.num_rows == 0 && i == E.screen_rows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                        "Welcome to txtedit! Press ^Q to quit.");
                if (welcome_len > E.screen_cols) {
                    welcome_len = E.screen_cols;
                }
                int padding = (E.screen_cols - welcome_len) / 2;
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
        }
        else {
            int len = E.rows[file_row].rsize - E.col_offset;
            if (len < 0) len = 0;
            if (len > E.screen_cols) len = E.screen_cols;
            ab_append(ab, &E.rows[file_row].render[E.col_offset], len);
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

/* draw a status bar at the bottom of the screen */
void draw_status_bar(struct AppendBuf *ab)
{
    ab_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.num_rows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cursor_y + 1, E.num_rows);
    if (len > E.screen_cols) {
        len = E.screen_cols;
    }
    ab_append(ab, status, len);
    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        }
        else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

/* draw a message bar at the bottom of the screen */
void draw_message_bar(struct AppendBuf *ab)
{
    ab_append(ab, "\x1b[K", 3);
    int len = strlen(E.status_msg);
    if (len > E.screen_cols) {
        len = E.screen_cols;
    }
    if (len && time(NULL) - E.status_msg_time < 5) {
        ab_append(ab, E.status_msg, len);
    }
}

/* refresh the screen */
void refresh_screen()
{
    editor_scroll();

    struct AppendBuf ab = APPEND_BUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    draw_rows(&ab);
    draw_status_bar(&ab);
    draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.row_offset) + 1, (E.rx - E.col_offset) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    ab_free(&ab);
}

/* set a status message */
void set_status_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
    va_end(ap);
    E.status_msg_time = time(NULL);
}

/* move the cursor using the arrow keys */
void move_cursor(int key)
{
    ERow *row = (E.cursor_y >= E.num_rows) ? NULL : &E.rows[E.cursor_y];

    switch (key) {
        case ARROW_UP:
            if (E.cursor_y != 0) {
                E.cursor_y--;
            }
            break;
        case ARROW_DOWN:
            if (E.cursor_y < E.num_rows) {
                E.cursor_y++;
            }
            break;
        case ARROW_LEFT:
            if (E.cursor_x != 0) {
                E.cursor_x--;
            }
            else if (E.cursor_y > 0) {
                E.cursor_y--;
                E.cursor_x = E.rows[E.cursor_y].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cursor_x < row->size) {
                E.cursor_x++;
            }
            else if (row && E.cursor_x == row->size) {
                E.cursor_y++;
                E.cursor_x = 0;
            }
            break;
    }

    row = (E.cursor_y >= E.num_rows) ? NULL : &E.rows[E.cursor_y];
    int row_len = row ? row->size : 0;
    if (E.cursor_x > row_len) {
        E.cursor_x = row_len;
    }
}

/* process user keypresses */
void process_keypress()
{
    static int quit_times = QUIT_TIMES;

    int c = read_keypress();

    switch (c) {
        case '\r':
            break;
        
        /* quit when Ctrl+q is pressed */
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                set_status_message("WARNING: File has unsaved changes. Use ^S to save or Press ^Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editor_save();
            break;
        
        case HOME_KEY:
            E.cursor_x = 0;
            break;
        case END_KEY:
            if (E.cursor_y < E.num_rows) {
                E.cursor_x = E.rows[E.cursor_y].size;
            }
            break;
        
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cursor_y = E.row_offset;
                }
                else if (c == PAGE_DOWN) {
                    E.cursor_y = E.row_offset + E.screen_rows - 1;
                    if (E.cursor_y > E.num_rows) {
                        E.cursor_y = E.num_rows;                        
                    }
                }
                int times = E.screen_rows;
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
        
        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            insert_char(c);
            break;
    }

    quit_times = QUIT_TIMES;
}

/* initialize editor */
void editor_init()
{
    E.cursor_x = 0;
    E.cursor_y = 0;
    E.rx = 0;
    E.row_offset = 0;
    E.col_offset = 0;
    E.num_rows = 0;
    E.rows = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.status_msg[0] = '\0';
    E.status_msg_time = 0;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
        display_error("get_window_size");
    }
    E.screen_rows -= 2;
}

int main(int argc, char **argv)
{
    enable_raw_mode();
    editor_init();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    set_status_message("HELP: ^S = save | ^Q = quit");

    while(1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}