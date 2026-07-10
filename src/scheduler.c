#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../include/scheduler.h"

typedef enum
{
    SCHED_HIGH,
    SCHED_MEDIUM,
    SCHED_LOW,
    SCHED_LEVELS
} sched_level_t;

typedef struct sched_job
{
    int id;
    time_t run_at;
    sched_level_t level;
    char *cmdline;
    int launched;
    struct sched_job *next;
} sched_job_t;

static sched_job_t *job_queue = NULL;
static int next_sched_id = 1;
static scheduler_executor_t executor = NULL;

static const int demote_seconds[SCHED_LEVELS] = {0, 30, 60};

void scheduler_init(void)
{
    job_queue = NULL;
    next_sched_id = 1;
    executor = NULL;
}

void scheduler_register_executor(scheduler_executor_t exec)
{
    executor = exec;
}

int scheduler_add_job(int delay_sec, const char *cmdline)
{
    sched_job_t *job = calloc(1, sizeof(sched_job_t));
    job->id = next_sched_id++;
    job->run_at = time(NULL) + delay_sec;
    job->level = SCHED_HIGH;
    job->cmdline = strdup(cmdline);
    job->launched = 0;
    job->next = NULL;

    if (!job_queue)
    {
        job_queue = job;
    }
    else
    {
        sched_job_t *cur = job_queue;
        while (cur->next)
            cur = cur->next;
        cur->next = job;
    }
    return job->id;
}

static const char *level_name(sched_level_t level)
{
    switch (level)
    {
    case SCHED_HIGH:
        return "HIGH";
    case SCHED_MEDIUM:
        return "MEDIUM";
    case SCHED_LOW:
        return "LOW";
    default:
        return "UNKNOWN";
    }
}

static void update_job_levels(void)
{
    time_t now = time(NULL);
    for (sched_job_t *job = job_queue; job; job = job->next)
    {
        if (job->launched)
            continue;
        if (job->level == SCHED_HIGH && now >= job->run_at + demote_seconds[1])
            job->level = SCHED_MEDIUM;
        else if (job->level == SCHED_MEDIUM && now >= job->run_at + demote_seconds[2])
            job->level = SCHED_LOW;
    }
}

void scheduler_print_queue(void)
{
    printf("Scheduled jobs:\n");
    for (sched_job_t *job = job_queue; job; job = job->next)
    {
        char buf[64];
        struct tm tm;
        localtime_r(&job->run_at, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        printf("[%d] %s @ %s priority=%s %s\n",
               job->id,
               job->cmdline,
               buf,
               level_name(job->level),
               job->launched ? "(launched)" : "(pending)");
    }
}

static sched_job_t *find_ready_job(void)
{
    time_t now = time(NULL);
    sched_job_t *best = NULL;
    for (sched_job_t *job = job_queue; job; job = job->next)
    {
        if (job->launched)
            continue;
        if (job->run_at > now)
            continue;
        if (!best || job->level < best->level || (job->level == best->level && job->run_at < best->run_at))
            best = job;
    }
    return best;
}

static sched_job_t *remove_job(sched_job_t *target)
{
    sched_job_t **pp = &job_queue;
    while (*pp)
    {
        if (*pp == target)
        {
            sched_job_t *next = (*pp)->next;
            *pp = next;
            return target;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

int scheduler_next_due(void)
{
    time_t now = time(NULL);
    time_t next = -1;
    for (sched_job_t *job = job_queue; job; job = job->next)
    {
        if (job->launched)
            continue;
        if (next == -1 || job->run_at < next)
            next = job->run_at;
    }
    if (next == -1)
        return -1;
    if (next <= now)
        return 0;
    return (int)(next - now);
}

void scheduler_check_pending(void)
{
    if (!executor)
        return;

    update_job_levels();
    while (1)
    {
        sched_job_t *job = find_ready_job();
        if (!job)
            break;
        job->launched = 1;
        executor(job->cmdline);
        sched_job_t *removed = remove_job(job);
        if (removed)
        {
            free(removed->cmdline);
            free(removed);
        }
    }
}
