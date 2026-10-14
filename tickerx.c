#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curses.h>
#include <curl/curl.h>
#include <time.h>

#define MAX_FEEDS 100
#define MAX_ITEMS_PER_FEED 15
#define MAX_TOTAL_ITEMS (MAX_FEEDS * MAX_ITEMS_PER_FEED)
#define TITLE_LEN 256
#define DESC_LEN 1024
#define LINK_LEN 512

typedef struct {
    char title[TITLE_LEN];
    char description[DESC_LEN];
    char link[LINK_LEN];
    time_t pub_date;
    int ticker_pos;
    int speed_interval;
    int speed_counter;
    int source_index;
} FeedItem;

struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback for libcurl to write fetched data into memory
static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

// Strip basic HTML tags and CDATA markers for clean text
void clean_text(char *dest, const char *src, size_t max_len) {
    int d = 0;
    int in_tag = 0;
    int i = 0;

    if (strncmp(src, "<![CDATA[", 9) == 0) {
        src += 9;
    }

    while (src[i] != '\0' && d < (int)max_len - 1) {
        if (strncmp(&src[i], "]]>", 3) == 0) {
            i += 3;
            continue;
        }
        if (src[i] == '<') {
            in_tag = 1;
        } else if (src[i] == '>') {
            in_tag = 0;
            i++;
            continue;
        }

        if (!in_tag) {
            char c = src[i];
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            dest[d++] = c;
        }
        i++;
    }
    dest[d] = '\0';
}

// Extract tag content between <tag> and </tag>
int extract_tag_content(const char *xml, const char *tag, char *dest, size_t max_len) {
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return 0;

    start = strchr(start, '>');
    if (!start) return 0;
    start++;

    const char *end = strstr(start, close_tag);
    if (!end) return 0;

    size_t len = end - start;
    if (len >= max_len) len = max_len - 1;

    char raw[DESC_LEN];
    memcpy(raw, start, len);
    raw[len] = '\0';

    clean_text(dest, raw, max_len);
    return 1;
}

// Parse RSS date strings into time_t
time_t parse_rss_date(const char *date_str) {
    struct tm tm;
    memset(&tm, 0, sizeof(struct tm));

    if (!date_str || strlen(date_str) == 0) return time(NULL);

    if (strptime(date_str, "%a, %d %b %Y %H:%M:%S", &tm) != NULL) {
        return mktime(&tm);
    }
    if (strptime(date_str, "%d %b %Y %H:%M:%S", &tm) != NULL) {
        return mktime(&tm);
    }
    if (strptime(date_str, "%Y-%m-%dT%H:%M:%S", &tm) != NULL) {
        return mktime(&tm);
    }

    return time(NULL);
}

// Fetch and parse RSS feed items
int fetch_rss_feed(const char *url, FeedItem *feed_items, int *count) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_handle = curl_easy_init();
    if (!curl_handle) return 0;

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);

    res = curl_easy_perform(curl_handle);
    curl_easy_cleanup(curl_handle);

    if (res != CURLE_OK) {
        free(chunk.memory);
        return 0;
    }

    const char *ptr = chunk.memory;
    int parsed_count = 0;
    FeedItem temp_items[50];
    int temp_count = 0;

    while ((ptr = strstr(ptr, "<item>")) != NULL && temp_count < 50) {
        FeedItem item;
        memset(&item, 0, sizeof(item));

        extract_tag_content(ptr, "title", item.title, TITLE_LEN);
        extract_tag_content(ptr, "description", item.description, DESC_LEN);
        extract_tag_content(ptr, "link", item.link, LINK_LEN);

        char date_str[128] = {0};
        extract_tag_content(ptr, "pubDate", date_str, sizeof(date_str));
        if (strlen(date_str) == 0) {
            extract_tag_content(ptr, "dc:date", date_str, sizeof(date_str));
        }
        item.pub_date = parse_rss_date(date_str);

        if (strlen(item.title) > 0) {
            temp_items[temp_count++] = item;
        }
        ptr += 6;
    }

    int start_idx = 0;
    if (temp_count > MAX_ITEMS_PER_FEED) {
        start_idx = temp_count - MAX_ITEMS_PER_FEED;
    }

    for (int i = start_idx; i < temp_count; i++) {
        feed_items[parsed_count++] = temp_items[i];
    }

    *count = parsed_count;
    free(chunk.memory);
    return 1;
}

// Comparison function for sorting items by date descending (newest first)
int compare_items_by_date(const void *a, const void *b) {
    const FeedItem *itemA = (const FeedItem *)a;
    const FeedItem *itemB = (const FeedItem *)b;
    if (itemA->pub_date > itemB->pub_date) return -1;
    if (itemA->pub_date < itemB->pub_date) return 1;
    return 0;
}

// Update, merge all feeds, deduplicate, and sort by date descending
void refresh_all_feeds(const char **urls, int num_feeds, FeedItem *all_items, int *total_items) {
    FeedItem *new_pool = calloc(MAX_TOTAL_ITEMS, sizeof(*new_pool));
    if (!new_pool) return;
    int new_pool_count = 0;

    for (int f = 0; f < num_feeds; f++) {
        FeedItem feed_items[MAX_ITEMS_PER_FEED];
        int feed_count = 0;
        if (fetch_rss_feed(urls[f], feed_items, &feed_count)) {
            for (int i = 0; i < feed_count; i++) {
                int duplicate = 0;
                for (int j = 0; j < new_pool_count; j++) {
                    if (new_pool[j].source_index == f &&
                        ((feed_items[i].link[0] && strcmp(new_pool[j].link, feed_items[i].link) == 0) ||
                         strcmp(new_pool[j].title, feed_items[i].title) == 0)) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate && new_pool_count < MAX_TOTAL_ITEMS) {
                    feed_items[i].source_index = f;
                    new_pool[new_pool_count] = feed_items[i];
                    int found_existing = 0;
                    for (int k = 0; k < *total_items; k++) {
                        if (strcmp(all_items[k].link, feed_items[i].link) == 0) {
                            new_pool[new_pool_count].ticker_pos = all_items[k].ticker_pos;
                            new_pool[new_pool_count].speed_interval = all_items[k].speed_interval;
                            new_pool[new_pool_count].speed_counter = all_items[k].speed_counter;
                            found_existing = 1;
                            break;
                        }
                    }
                    if (!found_existing) {
                        new_pool[new_pool_count].ticker_pos = 0;
                        new_pool[new_pool_count].speed_interval = 0;
                        new_pool[new_pool_count].speed_counter = 0;
                    }
                    new_pool_count++;
                }
            }
        }
    }

    qsort(new_pool, new_pool_count, sizeof(FeedItem), compare_items_by_date);

    for (int i = 0; i < new_pool_count; i++) {
        all_items[i] = new_pool[i];
    }
    *total_items = new_pool_count;
    free(new_pool);
}

int main(void) {
    srand(time(NULL));

    const useconds_t TICKER_DELAY_US = 150000; // 150ms

    // Add one RSS URL per line. Up to MAX_FEEDS (100) URLs are supported.
    const char *urls[MAX_FEEDS] = {
        "https://www.alexjoneslive.com/feed/",
        "https://yts.ag/rss",
        "https://getcomics.org/feed",
        "https://theawesomer.com/tag/gadgets/feed/",
        "https://techcrunch.com/feed/",
        "https://gizmodo.com/rss",
        "https://www.tecmint.com/feed/",
        "https://vpuniverse.com/discover/1.xml/?member=1774&amp;key=2fa5a8557ebc017399704eb6de4d35ae",
        "https://www.wired.com/feed/rss"
    };
    int num_feeds = 9; // Update this count when adding URLs above (maximum 100).

    curl_global_init(CURL_GLOBAL_ALL);

    // Initialize ncurses
    initscr();
    noecho();
    cbreak();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    // Initialize 24-bit TrueColor for even (#eeeeee) and odd (#cccccc) lines
    if (has_colors()) {
        start_color();
        use_default_colors();
        if (can_change_color()) {
            // Even lines: #eeeeee (R:238, G:238, B:238 -> ~933 on 0-1000 scale)
            init_color(8, 933, 933, 933);
            init_pair(1, 8, -1);

            // Odd lines: #cccccc (R:204, G:204, B:204 -> ~800 on 0-1000 scale)
            init_color(9, 800, 800, 800);
            init_pair(2, 9, -1);

            // Yellowgreen (#9ACD32) for feed item titles, if the terminal supports color 10.
            if (COLORS > 10) {
                init_color(10, 604, 804, 196);
                init_pair(3, 10, -1);
            } else {
                init_pair(3, COLOR_GREEN, -1);
            }
        } else {
            init_pair(1, COLOR_WHITE, -1);
            init_pair(2, COLOR_CYAN, -1);
            init_pair(3, COLOR_YELLOW, -1);
        }
    }

    FeedItem *all_items = calloc(MAX_TOTAL_ITEMS, sizeof(*all_items));
    if (!all_items) {
        endwin();
        fprintf(stderr, "Out of memory allocating feed items.\n");
        curl_global_cleanup();
        return 1;
    }
    int total_items = 0;

    refresh_all_feeds(urls, num_feeds, all_items, &total_items);

    if (total_items == 0) {
        endwin();
        fprintf(stderr, "No RSS items fetched or parsing failed.\n");
        curl_global_cleanup();
        free(all_items);
        return 1;
    }

    time_t last_fetch_time = time(NULL);
    int old_max_y = -1, old_max_x = -1;
    int scroll_offset = 0;

    // Main ticker loop
    int running = 1;
    while (running) {
        time_t current_time = time(NULL);
        if (current_time - last_fetch_time >= 300) {
            refresh_all_feeds(urls, num_feeds, all_items, &total_items);
            last_fetch_time = current_time;
        }

        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);

        if (max_y != old_max_y || max_x != old_max_x) {
            clear();
            old_max_y = max_y;
            old_max_x = max_x;
        }

        int usable_height = max_y - 1; // Reserve bottom row for footer
        int items_per_page = usable_height / 2;
        if (items_per_page < 1) items_per_page = 1;
        if (items_per_page > total_items) items_per_page = total_items;

        if (scroll_offset > total_items - items_per_page) {
            scroll_offset = total_items - items_per_page;
        }
        if (scroll_offset < 0) {
            scroll_offset = 0;
        }

        int current_row = 0;
        int last_speed = -1;

        for (int i = scroll_offset; i < total_items && current_row < usable_height; i++) {
            FeedItem *item = &all_items[i];

            // Feed item titles use yellowgreen; other ticker text alternates gray.
            int color_pair = (i % 2 == 0) ? 1 : 2;

            if (item->speed_interval == 0 || item->speed_interval == last_speed) {
                int new_speed;
                do {
                    new_speed = (rand() % 3) + 1;
                } while (new_speed == last_speed);
                item->speed_interval = new_speed;
            }
            last_speed = item->speed_interval;

            char combined[TITLE_LEN + DESC_LEN + 4];
            snprintf(combined, sizeof(combined), " %s: %s", item->title, item->description);
            int combined_len = strlen(combined);

            if (combined_len > 0) {
                int width = max_x - 1;
                if (width <= 0) width = 1;
                char ticker_buf[2048];
                if (width > (int)sizeof(ticker_buf) - 1) width = sizeof(ticker_buf) - 1;

                for (int w = 0; w < width; w++) {
                    int char_idx = (item->ticker_pos + w) % combined_len;
                    ticker_buf[w] = combined[char_idx];
                }
                ticker_buf[width] = '\0';

                move(current_row, 0);
                clrtoeol();

                // Color title characters yellowgreen even as the ticker scrolls.
                int title_len = (int)strlen(item->title) + 1; // include required leading space
                for (int w = 0; w < width; w++) {
                    int char_idx = (item->ticker_pos + w) % combined_len;
                    attron(COLOR_PAIR(char_idx < title_len ? 3 : color_pair));
                    mvaddch(current_row, w, (unsigned char)ticker_buf[w]);
                    attroff(COLOR_PAIR(char_idx < title_len ? 3 : color_pair));
                }

                if (current_row + 1 < usable_height) {
                    move(current_row + 1, 0);
                    clrtoeol();
                }

                item->speed_counter++;
                if (item->speed_counter >= item->speed_interval) {
                    item->ticker_pos = (item->ticker_pos + 1) % combined_len;
                    item->speed_counter = 0;
                }
            }
            
            current_row += 2;
        }

        move(max_y - 1, 0);
        clrtoeol();
        mvprintw(max_y - 1, 0, " [q] Quit | [Up/Down or k/j] Scroll (%d/%d) ", scroll_offset + 1, total_items - items_per_page + 1);
        
        refresh();

        int ch = getch();
        if (ch == KEY_RESIZE) {
            clear();
            getmaxyx(stdscr, old_max_y, old_max_x);
        } else if (ch == KEY_UP || ch == 'k') {
            if (scroll_offset > 0) {
                scroll_offset--;
                clear();
            }
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (scroll_offset < total_items - items_per_page) {
                scroll_offset++;
                clear();
            }
        } else if (ch == 'q' || ch == 'Q') {
            running = 0;
        }

        usleep(TICKER_DELAY_US);
    }

    endwin();
    curl_global_cleanup();
    free(all_items);
    return 0;
}
