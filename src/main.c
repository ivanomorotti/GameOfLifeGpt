#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define INITIAL_HASH_CAPACITY 2048
#define COUNT_HASH_CAPACITY 4096
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static struct termios original_termios;
static bool termios_saved = false;

static void restore_terminal(void) {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    }
}

static void setup_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
        perror("tcgetattr");
        exit(EXIT_FAILURE);
    }
    termios_saved = true;
    atexit(restore_terminal);

    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }
}

struct cell {
    int x;
    int y;
    struct cell *next;
};

struct cell_set {
    struct cell **buckets;
    size_t capacity;
    size_t size;
};

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static size_t cell_hash(const struct cell_set *set, int x, int y) {
    uint64_t key = ((uint64_t)(uint32_t)x << 32) ^ (uint32_t)y;
    return (size_t)(mix64(key) % set->capacity);
}

static void cell_set_init(struct cell_set *set, size_t capacity) {
    set->capacity = capacity;
    set->size = 0;
    set->buckets = calloc(set->capacity, sizeof(struct cell *));
    if (!set->buckets) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }
}

static void cell_set_clear(struct cell_set *set) {
    if (!set->buckets) {
        return;
    }
    for (size_t i = 0; i < set->capacity; ++i) {
        struct cell *node = set->buckets[i];
        while (node) {
            struct cell *next = node->next;
            free(node);
            node = next;
        }
        set->buckets[i] = NULL;
    }
    set->size = 0;
}

static void cell_set_destroy(struct cell_set *set) {
    cell_set_clear(set);
    free(set->buckets);
    set->buckets = NULL;
    set->capacity = 0;
    set->size = 0;
}

static bool cell_set_contains(const struct cell_set *set, int x, int y) {
    if (!set->buckets) {
        return false;
    }
    size_t index = cell_hash(set, x, y);
    struct cell *node = set->buckets[index];
    while (node) {
        if (node->x == x && node->y == y) {
            return true;
        }
        node = node->next;
    }
    return false;
}

static void cell_set_expand(struct cell_set *set);

static void cell_set_insert(struct cell_set *set, int x, int y) {
    if (cell_set_contains(set, x, y)) {
        return;
    }
    if ((set->size + 1) * 2 > set->capacity) {
        cell_set_expand(set);
    }
    size_t index = cell_hash(set, x, y);
    struct cell *node = malloc(sizeof(*node));
    if (!node) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    node->x = x;
    node->y = y;
    node->next = set->buckets[index];
    set->buckets[index] = node;
    set->size++;
}

static void cell_set_expand(struct cell_set *set) {
    size_t new_capacity = set->capacity ? set->capacity * 2 : INITIAL_HASH_CAPACITY;
    struct cell **new_buckets = calloc(new_capacity, sizeof(struct cell *));
    if (!new_buckets) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < set->capacity; ++i) {
        struct cell *node = set->buckets[i];
        while (node) {
            struct cell *next = node->next;
            uint64_t key = ((uint64_t)(uint32_t)node->x << 32) ^ (uint32_t)node->y;
            size_t index = (size_t)(mix64(key) % new_capacity);
            node->next = new_buckets[index];
            new_buckets[index] = node;
            node = next;
        }
    }
    free(set->buckets);
    set->buckets = new_buckets;
    set->capacity = new_capacity;
}

struct cell_iterator {
    const struct cell_set *set;
    size_t bucket;
    struct cell *node;
};

static struct cell_iterator cell_set_iter(const struct cell_set *set) {
    struct cell_iterator it = {set, 0, NULL};
    if (set->capacity > 0) {
        for (size_t i = 0; i < set->capacity; ++i) {
            if (set->buckets[i]) {
                it.bucket = i;
                it.node = set->buckets[i];
                break;
            }
        }
    }
    return it;
}

static bool cell_iter_next(struct cell_iterator *it, int *x, int *y) {
    if (!it->set || !it->set->buckets) {
        return false;
    }
    if (!it->node) {
        for (size_t i = it->bucket + 1; i < it->set->capacity; ++i) {
            if (it->set->buckets[i]) {
                it->bucket = i;
                it->node = it->set->buckets[i];
                break;
            }
        }
        if (!it->node) {
            return false;
        }
    }
    *x = it->node->x;
    *y = it->node->y;
    it->node = it->node->next;
    return true;
}

struct count_entry {
    int x;
    int y;
    int count;
    struct count_entry *next;
};

struct count_map {
    struct count_entry **buckets;
    size_t capacity;
};

static size_t count_hash(const struct count_map *map, int x, int y) {
    uint64_t key = ((uint64_t)(uint32_t)x << 32) ^ (uint32_t)y;
    return (size_t)(mix64(key) % map->capacity);
}

static void count_map_init(struct count_map *map, size_t capacity) {
    map->capacity = capacity;
    map->buckets = calloc(map->capacity, sizeof(struct count_entry *));
    if (!map->buckets) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }
}

static void count_map_destroy(struct count_map *map) {
    if (!map->buckets) {
        return;
    }
    for (size_t i = 0; i < map->capacity; ++i) {
        struct count_entry *node = map->buckets[i];
        while (node) {
            struct count_entry *next = node->next;
            free(node);
            node = next;
        }
    }
    free(map->buckets);
    map->buckets = NULL;
    map->capacity = 0;
}

static struct count_entry *count_map_get(struct count_map *map, int x, int y) {
    size_t index = count_hash(map, x, y);
    struct count_entry *node = map->buckets[index];
    while (node) {
        if (node->x == x && node->y == y) {
            return node;
        }
        node = node->next;
    }
    struct count_entry *entry = malloc(sizeof(*entry));
    if (!entry) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    entry->x = x;
    entry->y = y;
    entry->count = 0;
    entry->next = map->buckets[index];
    map->buckets[index] = entry;
    return entry;
}

static void count_map_increment(struct count_map *map, int x, int y) {
    struct count_entry *entry = count_map_get(map, x, y);
    entry->count += 1;
}

struct life_state {
    struct cell_set live;
    size_t generation;
};

static void life_state_init(struct life_state *state) {
    cell_set_init(&state->live, INITIAL_HASH_CAPACITY);
    state->generation = 0;
}

static void life_state_clear(struct life_state *state) {
    cell_set_clear(&state->live);
    state->generation = 0;
}

static void life_state_destroy(struct life_state *state) {
    cell_set_destroy(&state->live);
}

static void life_state_step(struct life_state *state) {
    struct count_map counts;
    count_map_init(&counts, COUNT_HASH_CAPACITY);

    struct cell_iterator it = cell_set_iter(&state->live);
    int x, y;
    while (cell_iter_next(&it, &x, &y)) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                count_map_increment(&counts, x + dx, y + dy);
            }
        }
    }

    struct cell_set next;
    cell_set_init(&next, state->live.capacity);

    for (size_t i = 0; i < counts.capacity; ++i) {
        struct count_entry *entry = counts.buckets[i];
        while (entry) {
            bool alive = cell_set_contains(&state->live, entry->x, entry->y);
            if (entry->count == 3 || (alive && entry->count == 2)) {
                cell_set_insert(&next, entry->x, entry->y);
            }
            entry = entry->next;
        }
    }

    cell_set_clear(&state->live);
    free(state->live.buckets);
    state->live = next;
    state->generation += 1;

    count_map_destroy(&counts);
}

static int life_state_import_file(struct life_state *state, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    life_state_clear(state);
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    int y = 0;
    while ((read = getline(&line, &len, fp)) != -1) {
        if (read > 0 && (line[0] == '!' || line[0] == '#')) {
            continue;
        }
        for (int x = 0; x < read; ++x) {
            char c = line[x];
            if (c == '\n' || c == '\r') {
                break;
            }
            if (c == 'O' || c == 'o' || c == 'X' || c == '1') {
                cell_set_insert(&state->live, x, y);
            }
        }
        y += 1;
    }
    free(line);
    fclose(fp);
    return 0;
}

static size_t cell_set_count(const struct cell_set *set) {
    return set->size;
}

struct view_state {
    int center_x;
    int center_y;
    int scale;
};

static void view_init(struct view_state *view) {
    view->center_x = 0;
    view->center_y = 0;
    view->scale = 1;
}

static void view_zoom_in(struct view_state *view) {
    view->scale = MAX(1, view->scale / 2);
}

static void view_zoom_out(struct view_state *view) {
    if (view->scale < 1024) {
        view->scale *= 2;
    }
}

static void view_pan(struct view_state *view, int dx, int dy) {
    view->center_x += dx * view->scale;
    view->center_y += dy * view->scale;
}

static void clear_screen(void) {
    printf("\033[2J\033[H");
}

static void render_state(const struct life_state *life, const struct view_state *view, bool paused, int delay_ms, const char *info_message) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }
    int rows = ws.ws_row > 4 ? ws.ws_row - 4 : ws.ws_row;
    int cols = ws.ws_col;

    clear_screen();

    int half_rows = rows / 2;
    int half_cols = cols / 2;

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            int origin_x = view->center_x - half_cols * view->scale + col * view->scale;
            int origin_y = view->center_y - half_rows * view->scale + row * view->scale;
            bool alive = false;
            for (int dy = 0; dy < view->scale && !alive; ++dy) {
                for (int dx = 0; dx < view->scale; ++dx) {
                    if (cell_set_contains(&life->live, origin_x + dx, origin_y + dy)) {
                        alive = true;
                        break;
                    }
                }
            }
            putchar(alive ? 'O' : '.');
        }
        putchar('\n');
    }

    printf("Generation: %zu | Live cells: %zu | Speed: %d ms | Scale: %d | Center: (%d,%d)\n", life->generation, cell_set_count(&life->live), delay_ms, view->scale, view->center_x, view->center_y);
    printf("Status: %s | Controls: q=quit p=pause/resume n=step w/a/s/d=pan +/-=zoom | r=reset to origin\n", paused ? "paused" : "running");
    if (info_message && *info_message) {
        printf("Info: %s\n", info_message);
    } else {
        printf("Info: \n");
    }
    fflush(stdout);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-t delay_ms] [-f file]\n", prog);
    fprintf(stderr, "  -t delay_ms  Set delay between generations in milliseconds (default 200)\n");
    fprintf(stderr, "  -f file      Load initial configuration from file\n");
}

int main(int argc, char **argv) {
    int opt;
    int delay_ms = 200;
    const char *file_path = NULL;
    while ((opt = getopt(argc, argv, "t:f:h")) != -1) {
        switch (opt) {
            case 't':
                delay_ms = atoi(optarg);
                if (delay_ms < 0) {
                    fprintf(stderr, "Invalid delay: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'f':
                file_path = optarg;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    struct life_state life;
    life_state_init(&life);

    if (file_path) {
        if (life_state_import_file(&life, file_path) == -1) {
            fprintf(stderr, "Failed to load configuration file '%s': %s\n", file_path, strerror(errno));
            life_state_destroy(&life);
            return EXIT_FAILURE;
        }
    }

    setup_terminal();

    struct view_state view;
    view_init(&view);

    bool paused = false;
    bool single_step = false;
    bool running = true;
    char info_message[128] = "Press q to quit, p to pause.";

    render_state(&life, &view, paused, delay_ms, info_message);
    info_message[0] = '\0';

    while (running) {
        char ch;
        ssize_t bytes;
        while ((bytes = read(STDIN_FILENO, &ch, 1)) > 0) {
            if (ch == 'q') {
                running = false;
                break;
            } else if (ch == 'p') {
                paused = !paused;
            } else if (ch == 'n') {
                single_step = true;
            } else if (ch == 'w') {
                view_pan(&view, 0, -1);
            } else if (ch == 's') {
                view_pan(&view, 0, 1);
            } else if (ch == 'a') {
                view_pan(&view, -1, 0);
            } else if (ch == 'd') {
                view_pan(&view, 1, 0);
            } else if (ch == '+' || ch == '=') {
                view_zoom_in(&view);
            } else if (ch == '-') {
                view_zoom_out(&view);
            } else if (ch == 'r') {
                view_init(&view);
                snprintf(info_message, sizeof(info_message), "View reset to origin");
            }
        }

        if (bytes == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read");
            break;
        }

        if ((!paused) || single_step) {
            life_state_step(&life);
            single_step = false;
        }

        render_state(&life, &view, paused, delay_ms, info_message);
        info_message[0] = '\0';

        if (delay_ms > 0) {
            struct timespec req = {delay_ms / 1000, (delay_ms % 1000) * 1000000L};
            nanosleep(&req, NULL);
        }
    }

    restore_terminal();
    clear_screen();
    life_state_destroy(&life);
    return EXIT_SUCCESS;
}
