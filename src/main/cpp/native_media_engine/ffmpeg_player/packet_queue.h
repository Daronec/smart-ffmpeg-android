#pragma once

#include <pthread.h>
#include <stdbool.h>
#include "libavcodec/avcodec.h"

/// Узел очереди пакетов
typedef struct PacketNode {
    AVPacket pkt;
    struct PacketNode *next;
} PacketNode;

/// Очередь пакетов (thread-safe, linked list)
///
/// Используется для передачи AVPacket между потоками:
/// - demux thread → decode threads
/// - thread-safe через mutex + cond
/// - linked list (packets разного размера)
/// - abort-safe (можно прервать из любого потока)
typedef struct PacketQueue {
    PacketNode *first_pkt;
    PacketNode *last_pkt;
    int nb_packets;
    int size;              // bytes
    bool abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

/// Инициализировать очередь пакетов
///
/// @param q Очередь для инициализации
void packet_queue_init(PacketQueue *q);

/// Освободить ресурсы очереди
///
/// @param q Очередь
void packet_queue_destroy(PacketQueue *q);

/// Прервать очередь (для stop/destroy)
///
/// @param q Очередь
void packet_queue_abort(PacketQueue *q);

/// Сбросить флаг abort_request (для restart)
///
/// @param q Очередь
void packet_queue_reset_abort(PacketQueue *q);

/// Очистить очередь (для seek/stop)
///
/// @param q Очередь
void packet_queue_flush(PacketQueue *q);

/// Добавить пакет в очередь
///
/// @param q Очередь
/// @param pkt Пакет для добавления (будет скопирован через av_packet_ref)
/// @return 0 при успехе, <0 при ошибке
int packet_queue_put(PacketQueue *q, AVPacket *pkt);

/// Извлечь пакет из очереди (блокирующий или неблокирующий)
///
/// @param q Очередь
/// @param pkt Буфер для пакета (ownership переходит вызывающему)
/// @param block true = блокирующий (ждёт пакет), false = неблокирующий
/// @return 1 при успехе, 0 если очередь пуста (block=false), <0 при abort
int packet_queue_get(PacketQueue *q, AVPacket *pkt, bool block);

// Алиасы для совместимости
#define packet_queue_push packet_queue_put
#define packet_queue_pop(q, pkt) packet_queue_get(q, pkt, true)
