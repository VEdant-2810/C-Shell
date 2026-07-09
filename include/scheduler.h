#ifndef SCHEDULER_H
#define SCHEDULER_H

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void (*scheduler_executor_t)(const char *cmdline);

    void scheduler_init(void);
    void scheduler_register_executor(scheduler_executor_t executor);
    int scheduler_add_job(int delay_sec, const char *cmdline);
    void scheduler_check_pending(void);
    int scheduler_next_due(void);
    void scheduler_print_queue(void);

#ifdef __cplusplus
}
#endif

#endif
