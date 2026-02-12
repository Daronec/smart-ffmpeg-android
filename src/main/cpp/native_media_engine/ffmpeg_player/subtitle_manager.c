#include "subtitle_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>

#define INITIAL_CAPACITY 256

void subtitle_manager_init(SubtitleManager *sm) {
    if (!sm) {
        return;
    }
    
    memset(sm, 0, sizeof(SubtitleManager));
    sm->capacity = INITIAL_CAPACITY;
    sm->items = (SubtitleItem *)malloc(sm->capacity * sizeof(SubtitleItem));
    sm->last_index = 0;
    sm->user_offset = 0.0;
    sm->loaded = false;
}

void subtitle_manager_destroy(SubtitleManager *sm) {
    if (!sm) {
        return;
    }
    
    subtitle_manager_clear(sm);
    
    if (sm->items) {
        free(sm->items);
        sm->items = NULL;
    }
    
    sm->capacity = 0;
    sm->count = 0;
}

void subtitle_manager_clear(SubtitleManager *sm) {
    if (!sm || !sm->items) {
        return;
    }
    
    // Освобождаем тексты
    for (int i = 0; i < sm->count; i++) {
        if (sm->items[i].text) {
            free(sm->items[i].text);
            sm->items[i].text = NULL;
        }
    }
    
    sm->count = 0;
    sm->last_index = 0;
    sm->loaded = false;
}

int subtitle_manager_add(SubtitleManager *sm, double start, double end, const char *text) {
    if (!sm || !text || start < 0 || end <= start) {
        return -1;
    }
    
    // Расширяем массив если нужно
    if (sm->count >= sm->capacity) {
        int new_capacity = sm->capacity * 2;
        SubtitleItem *new_items = (SubtitleItem *)realloc(
            sm->items,
            new_capacity * sizeof(SubtitleItem)
        );
        
        if (!new_items) {
            return -1;
        }
        
        sm->items = new_items;
        sm->capacity = new_capacity;
    }
    
    // Добавляем субтитр
    SubtitleItem *item = &sm->items[sm->count];
    item->start = start;
    item->end = end;
    item->text_len = strlen(text);
    item->text = (char *)malloc(item->text_len + 1);
    
    if (!item->text) {
        return -1;
    }
    
    strcpy(item->text, text);
    sm->count++;
    
    return 0;
}

// Парсинг времени SRT: "00:01:02,500" → 62.500
static double parse_srt_time(const char *time_str) {
    int hours, minutes, seconds, milliseconds;
    
    if (sscanf(time_str, "%d:%d:%d,%d", &hours, &minutes, &seconds, &milliseconds) != 4) {
        return -1.0;
    }
    
    return hours * 3600.0 + minutes * 60.0 + seconds + milliseconds / 1000.0;
}

int subtitle_manager_parse_srt(SubtitleManager *sm, const char *path) {
    if (!sm || !path) {
        return -1;
    }
    
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    
    char line[1024];
    int index = -1;
    double start = -1.0, end = -1.0;
    char text[4096] = {0};
    int text_pos = 0;
    
    subtitle_manager_clear(sm);
    
    while (fgets(line, sizeof(line), file)) {
        // Убираем перевод строки
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        
        // Пропускаем пустые строки
        if (strlen(line) == 0) {
            if (index >= 0 && start >= 0 && end >= 0 && text_pos > 0) {
                text[text_pos] = '\0';
                subtitle_manager_add(sm, start, end, text);
                index = -1;
                start = -1.0;
                end = -1.0;
                text_pos = 0;
                text[0] = '\0';
            }
            continue;
        }
        
        // Парсим индекс
        if (index < 0) {
            index = atoi(line);
            continue;
        }
        
        // Парсим время: "00:01:02,500 --> 00:01:05,200"
        if (start < 0) {
            char start_str[32], end_str[32];
            if (sscanf(line, "%31[^ ] --> %31s", start_str, end_str) == 2) {
                start = parse_srt_time(start_str);
                end = parse_srt_time(end_str);
            }
            continue;
        }
        
        // Текст субтитра (может быть многострочным)
        if (text_pos > 0) {
            text[text_pos++] = '\n';
        }
        strcpy(text + text_pos, line);
        text_pos += strlen(line);
    }
    
    // Добавляем последний субтитр
    if (index >= 0 && start >= 0 && end >= 0 && text_pos > 0) {
        text[text_pos] = '\0';
        subtitle_manager_add(sm, start, end, text);
    }
    
    fclose(file);
    sm->loaded = true;
    
    return 0;
}

// Парсинг времени ASS: "0:01:02.50" → 62.50
static double parse_ass_time(const char *time_str) {
    int hours, minutes, seconds, centiseconds;
    
    if (sscanf(time_str, "%d:%d:%d.%d", &hours, &minutes, &seconds, &centiseconds) != 4) {
        return -1.0;
    }
    
    return hours * 3600.0 + minutes * 60.0 + seconds + centiseconds / 100.0;
}

int subtitle_manager_parse_ass(SubtitleManager *sm, const char *path) {
    if (!sm || !path) {
        return -1;
    }
    
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    
    char line[4096];
    subtitle_manager_clear(sm);
    
    while (fgets(line, sizeof(line), file)) {
        // Убираем перевод строки
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        
        // Парсим Dialogue строку: "Dialogue: 0,0:01:02.50,0:01:05.20,Default,,0,0,0,,Text"
        if (strncmp(line, "Dialogue:", 9) == 0) {
            char *p = line + 9;
            
            // Пропускаем до первого поля (Layer)
            while (*p && *p != ',') p++;
            if (*p) p++;
            
            // Парсим Start time
            char start_str[32] = {0};
            int i = 0;
            while (*p && *p != ',' && i < 31) {
                start_str[i++] = *p++;
            }
            if (*p) p++;
            
            double start = parse_ass_time(start_str);
            if (start < 0) continue;
            
            // Парсим End time
            char end_str[32] = {0};
            i = 0;
            while (*p && *p != ',' && i < 31) {
                end_str[i++] = *p++;
            }
            if (*p) p++;
            
            double end = parse_ass_time(end_str);
            if (end < 0) continue;
            
            // Пропускаем до 9-го поля (Text)
            int field = 0;
            while (*p && field < 8) {
                if (*p == ',') field++;
                p++;
            }
            
            // Текст начинается после 9-й запятой
            const char *text = p;
            
            // Убираем ASS теги (пока просто берём текст)
            // TODO: Парсить ASS теги для стилей
            char clean_text[2048] = {0};
            int clean_pos = 0;
            bool in_tag = false;
            
            for (const char *t = text; *t && clean_pos < 2047; t++) {
                if (*t == '{') {
                    in_tag = true;
                } else if (*t == '}') {
                    in_tag = false;
                } else if (!in_tag) {
                    clean_text[clean_pos++] = *t;
                }
            }
            
            clean_text[clean_pos] = '\0';
            
            if (strlen(clean_text) > 0) {
                subtitle_manager_add(sm, start, end, clean_text);
            }
        }
    }
    
    fclose(file);
    sm->loaded = true;
    
    return 0;
}

int subtitle_manager_load_embedded(SubtitleManager *sm, void *fmt_ctx, int stream_index) {
    // TODO: Реализовать загрузку embedded субтитров через FFmpeg
    // avcodec_decode_subtitle2() и т.д.
    (void)sm;
    (void)fmt_ctx;
    (void)stream_index;
    return -1;
}

const SubtitleItem *subtitle_manager_get_active(SubtitleManager *sm, double audio_clock) {
    if (!sm || !sm->items || sm->count == 0) {
        return NULL;
    }
    
    // Шаг 32.5: SubtitleClock = audio clock (master clock)
    // Применяем user offset
    double clock_with_offset = audio_clock + sm->user_offset;
    
    // Оптимизация: начинаем поиск с last_index
    int start_index = sm->last_index;
    if (start_index >= sm->count) {
        start_index = 0;
    }
    
    // Шаг 32.6: Ищем активный субтитр для audio_clock (master clock)
    
    for (int i = start_index; i < sm->count; i++) {
        if (sm->items[i].start <= clock_with_offset && clock_with_offset <= sm->items[i].end) {
            sm->last_index = i;
            return &sm->items[i];
        }
        
        // Если прошли нужное время, можно остановиться
        if (sm->items[i].start > clock_with_offset) {
            break;
        }
    }
    
    // Если не нашли, проверяем с начала
    for (int i = 0; i < start_index; i++) {
        if (sm->items[i].start <= clock_with_offset && clock_with_offset <= sm->items[i].end) {
            sm->last_index = i;
            return &sm->items[i];
        }
        
        if (sm->items[i].start > clock_with_offset) {
            break;
        }
    }
    
    return NULL;
}

void subtitle_manager_seek(SubtitleManager *sm, double audio_clock) {
    if (!sm) {
        return;
    }
    
    // Шаг 32.9: Seek handling - сбрасываем last_index для оптимизации
    sm->last_index = 0;
    
    // Ищем ближайший индекс для audio_clock (с учётом offset)
    double clock_with_offset = audio_clock + sm->user_offset;
    
    for (int i = 0; i < sm->count; i++) {
        if (sm->items[i].start > clock_with_offset) {
            sm->last_index = i > 0 ? i - 1 : 0;
            break;
        }
    }
}

void subtitle_manager_set_offset(SubtitleManager *sm, double offset) {
    if (!sm) {
        return;
    }
    sm->user_offset = offset;
}

double subtitle_manager_get_offset(SubtitleManager *sm) {
    if (!sm) {
        return 0.0;
    }
    return sm->user_offset;
}

