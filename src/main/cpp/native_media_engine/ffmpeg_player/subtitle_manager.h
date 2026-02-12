#ifndef SUBTITLE_MANAGER_H
#define SUBTITLE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/// Элемент субтитра
///
/// Все форматы (SRT, ASS, embedded) конвертируются в эту структуру
/// Время всегда в секундах (double)
typedef struct {
    /// Время начала (в секундах)
    double start;
    
    /// Время окончания (в секундах)
    double end;
    
    /// Текст субтитра (UTF-8 или ASS текст)
    char *text;
    
    /// Длина текста
    size_t text_len;
} SubtitleItem;

/// Менеджер субтитров
///
/// Управляет:
/// - Парсингом субтитров (SRT, ASS, embedded)
/// - Синхронизацией по video_clock
/// - Поиском активного субтитра
///
/// ❗ SubtitleManager НЕ знает про audio, UI, rendering
typedef struct {
    /// Массив субтитров
    SubtitleItem *items;
    
    /// Количество субтитров
    int count;
    
    /// Емкость массива
    int capacity;
    
    /// Индекс последнего найденного субтитра (оптимизация)
    int last_index;
    
    /// Пользовательский offset (в секундах)
    double user_offset;
    
    /// Флаг, что субтитры загружены
    bool loaded;
} SubtitleManager;

/// Инициализировать менеджер субтитров
///
/// @param sm Менеджер для инициализации
void subtitle_manager_init(SubtitleManager *sm);

/// Освободить ресурсы менеджера субтитров
///
/// @param sm Менеджер
void subtitle_manager_destroy(SubtitleManager *sm);

/// Очистить все субтитры
///
/// @param sm Менеджер
void subtitle_manager_clear(SubtitleManager *sm);

/// Добавить субтитр
///
/// @param sm Менеджер
/// @param start Время начала (в секундах)
/// @param end Время окончания (в секундах)
/// @param text Текст субтитра (копируется)
/// @return 0 при успехе, <0 при ошибке
int subtitle_manager_add(SubtitleManager *sm, double start, double end, const char *text);

/// Парсить SRT файл
///
/// @param sm Менеджер
/// @param path Путь к .srt файлу
/// @return 0 при успехе, <0 при ошибке
int subtitle_manager_parse_srt(SubtitleManager *sm, const char *path);

/// Парсить ASS/SSA файл
///
/// На этом этапе парсим только тайминг и текст, без стилей
///
/// @param sm Менеджер
/// @param path Путь к .ass/.ssa файлу
/// @return 0 при успехе, <0 при ошибке
int subtitle_manager_parse_ass(SubtitleManager *sm, const char *path);

/// Загрузить embedded субтитры из AVFormatContext
///
/// @param sm Менеджер
/// @param fmt_ctx Format context
/// @param stream_index Индекс subtitle stream
/// @return 0 при успехе, <0 при ошибке
int subtitle_manager_load_embedded(SubtitleManager *sm, void *fmt_ctx, int stream_index);

/// Получить активный субтитр для текущего audio_clock (Шаг 32.5)
///
/// 🎯 Синхронизируется по AUDIO CLOCK (master clock), не по video или wall clock
/// Это ключевое изменение шага 32 - субтитры всегда синхронизируются с аудио
///
/// @param sm Менеджер
/// @param audio_clock Текущий audio clock (в секундах) - MASTER CLOCK
/// @return Указатель на активный субтитр, или NULL если нет активного
const SubtitleItem *subtitle_manager_get_active(SubtitleManager *sm, double audio_clock);

/// Выполнить seek (Шаг 32.9)
///
/// Сбрасывает last_index для оптимизации и очищает активные субтитры
///
/// @param sm Менеджер
/// @param audio_clock Новая позиция audio clock (в секундах)
void subtitle_manager_seek(SubtitleManager *sm, double audio_clock);

/// Установить пользовательский offset
///
/// @param sm Менеджер
/// @param offset Offset в секундах (может быть отрицательным)
void subtitle_manager_set_offset(SubtitleManager *sm, double offset);

/// Получить пользовательский offset
///
/// @param sm Менеджер
/// @return Offset в секундах
double subtitle_manager_get_offset(SubtitleManager *sm);

#endif // SUBTITLE_MANAGER_H

