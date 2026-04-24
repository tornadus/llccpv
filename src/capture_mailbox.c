#include "capture_mailbox.h"

void capture_mailbox_init(struct capture_mailbox *mb)
{
    memset(mb, 0, sizeof(*mb));
    pthread_mutex_init(&mb->lock, NULL);
}

void capture_mailbox_destroy(struct capture_mailbox *mb)
{
    pthread_mutex_destroy(&mb->lock);
}

void capture_mailbox_publish(struct capture_mailbox *mb,
                             const struct frame_info *f,
                             int *displaced_out)
{
    int displaced = -1;
    pthread_mutex_lock(&mb->lock);
    if (atomic_load(&mb->has_frame))
        displaced = mb->frame.buf_index;
    mb->frame = *f;
    atomic_store(&mb->has_frame, true);
    pthread_mutex_unlock(&mb->lock);
    if (displaced_out) *displaced_out = displaced;
}

int capture_mailbox_consume(struct capture_mailbox *mb, struct frame_info *out)
{
    if (!atomic_load(&mb->has_frame))
        return -1;

    pthread_mutex_lock(&mb->lock);
    if (!atomic_load(&mb->has_frame)) {
        pthread_mutex_unlock(&mb->lock);
        return -1;
    }
    *out = mb->frame;
    atomic_store(&mb->has_frame, false);
    pthread_mutex_unlock(&mb->lock);
    return 0;
}

int capture_mailbox_drain(struct capture_mailbox *mb, int *drained_out)
{
    int drained = -1;
    pthread_mutex_lock(&mb->lock);
    if (atomic_load(&mb->has_frame)) {
        drained = mb->frame.buf_index;
        atomic_store(&mb->has_frame, false);
    }
    pthread_mutex_unlock(&mb->lock);
    if (drained_out) *drained_out = drained;
    return drained >= 0 ? 0 : -1;
}
