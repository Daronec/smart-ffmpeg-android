#include "packet_queue.h"
#include <stdlib.h>
#include <string.h>

/// Освободить узел пакета
static void packet_node_free(PacketNode *node) {
    if (!node) return;
    av_packet_unref(&node->pkt);
    free(node);
}

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->abort_request = false;
}

void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

void packet_queue_abort(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->abort_request = true;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

void packet_queue_reset_abort(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->abort_request = false;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

void packet_queue_flush(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    
    PacketNode *node = q->first_pkt;
    while (node) {
        PacketNode *next = node->next;
        packet_node_free(node);
        node = next;
    }
    
    q->first_pkt = NULL;
    q->last_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    PacketNode *node = malloc(sizeof(PacketNode));
    if (!node) {
        return -1;
    }
    
    av_init_packet(&node->pkt);
    if (av_packet_ref(&node->pkt, pkt) < 0) {
        free(node);
        return -1;
    }
    
    node->next = NULL;
    
    pthread_mutex_lock(&q->mutex);
    
    if (q->abort_request) {
        pthread_mutex_unlock(&q->mutex);
        packet_node_free(node);
        return -1;
    }
    
    if (!q->last_pkt) {
        q->first_pkt = node;
    } else {
        q->last_pkt->next = node;
    }
    
    q->last_pkt = node;
    q->nb_packets++;
    q->size += node->pkt.size;
    
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    
    return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, bool block) {
    pthread_mutex_lock(&q->mutex);
    
    for (;;) {
        if (q->abort_request) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }
        
        PacketNode *node = q->first_pkt;
        if (node) {
            q->first_pkt = node->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            
            q->nb_packets--;
            q->size -= node->pkt.size;
            
            *pkt = node->pkt; // ownership переходит вызывающему
            free(node);
            
            pthread_mutex_unlock(&q->mutex);
            return 1;
        } else if (!block) {
            pthread_mutex_unlock(&q->mutex);
            return 0;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
}
