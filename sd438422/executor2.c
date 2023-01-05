#include <errno.h>
#include <fcntl.h> /* For O_* constants */
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h> /* For mode constants */
#include <sys/wait.h>
#include <unistd.h>

#include "err.h"
#include "utils.h"

#define ACTIONS 6
#define MAX_LINE_SIZE 512  // input
#define MAX_N_TASKS 4096
#define MAX_OUTPUT_LINE_SIZE 1023  // output
#define debug false
#define renegade_debug true

struct TaskOutput {
    char out_line[MAX_OUTPUT_LINE_SIZE];
    char err_line[MAX_OUTPUT_LINE_SIZE];
};
typedef struct TaskOutput TaskOutput_t;

struct SharedStorage {
    int n_tasks_running;
    TaskOutput_t tasks[MAX_N_TASKS];  // ostatnie linie outputu
    sem_t mutex;
    pid_t pids[MAX_N_TASKS];
    sem_t exec_mutex;
    sem_t pid_set[MAX_N_TASKS];
};
typedef struct SharedStorage SharedStorage_t;

int main() {
    // inititlize SharedStorage
    SharedStorage_t* storage =
            mmap(NULL, sizeof(SharedStorage_t), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (storage == MAP_FAILED) syserr("mmap");
    storage->n_tasks_running = 0;
    for (int i = 0; i < MAX_N_TASKS; ++i) {
        strcpy(storage->tasks[i].out_line, "");
        strcpy(storage->tasks[i].err_line, "");
    }
    ASSERT_SYS_OK(sem_init(&(storage->mutex), 1, 1));
    ASSERT_SYS_OK(sem_init(&(storage->exec_mutex), 1, 1));
    for (int i = 0; i < MAX_N_TASKS; ++i) {
        ASSERT_SYS_OK(sem_init(&(storage->pid_set[i]), 1, 0));
    }

    char buffer[MAX_LINE_SIZE];

    char** split_line = NULL;

    while (read_line(buffer, MAX_LINE_SIZE, stdin)) {
        split_line = split_string(buffer);

        char* command = split_line[0];
        size_t size = strlen(command);
        if (size == 0) {
            continue;
        }
        if (command[size - 1] == '\n') {
            command[size - 1] = '\0';
        }
        if (renegade_debug)
            fprintf(stderr, "read line: %s by pid: %d\n", buffer, getpid());

        if (strcmp(command, "run") == 0) {
            ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
            // run command
            int task_id;
            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
            task_id = storage->n_tasks_running++;
            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
            pid_t buffer_pid = fork();
            ASSERT_SYS_OK(buffer_pid);
            if (debug) fprintf(stderr, "forked: buffer_pid: %d\n", buffer_pid);
            if (buffer_pid == 0) {
                if (debug) fprintf(stderr, "I make pipes\n");
                // children: buffers.
                // create pipes
                int buff_out_desc[2];
                int buff_err_desc[2];
                ASSERT_SYS_OK(pipe(buff_out_desc));
                ASSERT_SYS_OK(pipe(buff_err_desc));
                pid_t err_buffer_pid = fork();
                ASSERT_SYS_OK(err_buffer_pid);
                if (err_buffer_pid == 0) {  // err buffer
                    ASSERT_SYS_OK(close(STDIN_FILENO));
                    free_split_string(split_line);
                    if (debug) fprintf(stderr, "I am err buffer\n");
                    ASSERT_SYS_OK(close(buff_out_desc[0]));
                    ASSERT_SYS_OK(close(buff_out_desc[1]));
                    ASSERT_SYS_OK(close(buff_err_desc[1]));
                    char err_buffer[MAX_OUTPUT_LINE_SIZE];
                    // use fdopen to get FILE* from fd
                    FILE* err_file = fdopen(buff_err_desc[0], "r");
                    // read line by line
                    while (fgets(err_buffer, MAX_OUTPUT_LINE_SIZE, err_file) != NULL) {
                        if (debug) fprintf(stderr, "err: %s", err_buffer);
                        // save line
                        if (strlen(err_buffer) == 1 && err_buffer[0] == EOF) break;
                        if (err_buffer[strlen(err_buffer) - 1] == '\n') {
                            err_buffer[strlen(err_buffer) - 1] = '\0';
                        }
                        ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
                        strcpy(storage->tasks[task_id].err_line, err_buffer);
                        // storage->tasks[task_id].err_line = strdup(err_buffer);
                        ASSERT_SYS_OK(sem_post(&(storage->mutex)));
                    }
                    ASSERT_SYS_OK(close(buff_err_desc[0]));
                    exit(0);
                } else {  // out buffer, task
                    pid_t task_pid = fork();
                    ASSERT_SYS_OK(task_pid);
                    if (task_pid == 0) {  // out buffer
                        ASSERT_SYS_OK(close(STDIN_FILENO));
                        free_split_string(split_line);
                        if (debug) fprintf(stderr, "I am out buffer\n");
                        ASSERT_SYS_OK(close(buff_out_desc[1]));
                        ASSERT_SYS_OK(close(buff_err_desc[1]));
                        ASSERT_SYS_OK(close(buff_err_desc[0]));
                        char out_buffer[MAX_OUTPUT_LINE_SIZE];
                        // use fdopen to get FILE* from fd
                        FILE* err_file = fdopen(buff_out_desc[0], "r");
                        // read line by line
                        while (fgets(out_buffer, MAX_OUTPUT_LINE_SIZE, err_file) != NULL) {
                            if (debug) fprintf(stderr, "out: %s", out_buffer);
                            // save line
                            if (strlen(out_buffer) == 1 && out_buffer[0] == EOF) break;
                            if (out_buffer[strlen(out_buffer) - 1] == '\n') {
                                out_buffer[strlen(out_buffer) - 1] = '\0';
                            }
                            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
                            // if (debug)
                            //   fprintf(stderr, "task %d saving line: %s\n", task_id,
                            //           out_buffer);
                            strcpy(storage->tasks[task_id].out_line, out_buffer);
                            // if (debug)
                            //   fprintf(stderr, "task %d saved line: %s", task_id,
                            // storage->tasks[task_id].out_line);
                            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
                        }
                        ASSERT_SYS_OK(close(buff_out_desc[0]));
                        exit(0);
                    } else {  // task & waiter.

                        pid_t waiter_pid = fork();
                        if (waiter_pid != 0) {  // waiter
                            ASSERT_SYS_OK(close(STDIN_FILENO));
                            ASSERT_SYS_OK(close(buff_out_desc[0]));
                            ASSERT_SYS_OK(close(buff_err_desc[0]));
                            ASSERT_SYS_OK(close(buff_out_desc[1]));
                            ASSERT_SYS_OK(close(buff_err_desc[1]));
                            free_split_string(split_line);
                            int status;
                            if (debug)
                                fprintf(stderr, "\nI am waiter for task %d.\n", task_id);
                            ASSERT_SYS_OK(sem_wait(&(storage->pid_set[task_id])));
                            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));

                            if (debug)
                                fprintf(stderr, "\nwaiter for task %d got task pid\n", task_id);
                            pid_t task_pid = storage->pids[task_id];
                            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
                            if (debug)
                                fprintf(stderr, "Waiting for task %d: pid %d\n", task_id,
                                        task_pid);
                            if (waitpid(task_pid, &status, 0) == -1) {
                                printf("\nwaitpid problem: errno %d\n", errno);
                                exit(1);
                            } else {
                                ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
                                if (WIFSIGNALED(status)) {
                                    printf("Task %d ended: signalled.\n", task_id);
                                } else {
                                    printf("Task %d ended: status %d.\n", task_id,
                                           WEXITSTATUS(status));
                                }
                                ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));
                                exit(0);
                            }
                        } else {  // task
                            // ASSERT_SYS_OK(close(STDIN_FILENO));
                            printf("Task %d started: pid %d.\n", task_id, task_pid);
                            ASSERT_SYS_OK(fflush(stdout));
                            char** args = &split_line[1];
                            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
                            storage->pids[task_id] = getpid();
                            if (debug)
                                fprintf(stderr, "Task %d pid set in storage: %d\n", task_id,
                                        task_pid);
                            ASSERT_SYS_OK(sem_post(&(storage->pid_set[task_id])));
                            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
                            if (debug) {
                                fprintf(stderr, "About to dup & execpv: %s\n", args[0]);
                            }
                            ASSERT_SYS_OK(dup2(buff_out_desc[1], STDOUT_FILENO));
                            ASSERT_SYS_OK(dup2(buff_err_desc[1], STDERR_FILENO));
                            ASSERT_SYS_OK(close(buff_out_desc[0]));
                            ASSERT_SYS_OK(close(buff_out_desc[1]));
                            ASSERT_SYS_OK(close(buff_err_desc[0]));
                            ASSERT_SYS_OK(close(buff_err_desc[1]));
                            ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));
                            ASSERT_SYS_OK(execvp(args[0], args));
                        }
                    }
                }
            } else {  // main process
                if (debug) fprintf(stderr, "run\n");
            }

        } else if (strcmp(command, "out") == 0) {
            ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
            int task_id = atoi(split_line[1]);
            if (debug) fprintf(stderr, "out %d\n", task_id);

            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
            char out_line[MAX_OUTPUT_LINE_SIZE];
            strcpy(out_line, storage->tasks[task_id].out_line);
            // char* out_line = strdup(storage->tasks[task_id].out_line);
            if (debug)
                fprintf(stderr, "out line: '%s'\n", storage->tasks[task_id].out_line);
            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
            if (renegade_debug)
                fprintf(stderr, "out %d by pid %d\n", task_id, getpid());
            printf("Task %d stdout: '%s'.\n", task_id, out_line);
            ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));

        } else if (strcmp(command, "err") == 0) {
            ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
            int task_id = atoi(split_line[1]);
            if (debug) fprintf(stderr, "err %d\n", task_id);
            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
            char err_line[MAX_OUTPUT_LINE_SIZE];
            strcpy(err_line, storage->tasks[task_id].err_line);
            // char* err_line = storage->tasks[task_id].err_line;
            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
            printf("Task %d stderr: '%s'.\n", task_id, err_line);
            ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));

        } else if (strcmp(command, "kill") == 0) {
            ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
            int task_id = atoi(split_line[1]);
            if (debug) fprintf(stderr, "kill %d\n", task_id);
            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
            pid_t task_pid = storage->pids[task_id];
            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
            kill(task_pid, SIGINT);
            ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));

        } else if (strcmp(command, "sleep") == 0) {
            // ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
            int sleep_time = atoi(split_line[1]);
            if (debug) fprintf(stderr, "sleep %d\n", sleep_time);
            sleep_time *= 1000;
            usleep(sleep_time);
            // ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));

        } else if (strcmp(command, "quit") == 0) {
            ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
            if (debug) fprintf(stderr, "quit\n");
            int tasks_running;
            ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
            tasks_running = storage->n_tasks_running;
            for (int i = 0; i < tasks_running; ++i) {
                pid_t task_pid = storage->pids[i];
                kill(task_pid, SIGKILL);
            }
            ASSERT_SYS_OK(sem_post(&(storage->mutex)));
            ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));
            free_split_string(split_line);
            exit(0);

        } else {
            printf("Unknown command: %s\n", command);
        }

        free_split_string(split_line);
    }

    ASSERT_SYS_OK(sem_wait(&(storage->exec_mutex)));
    if (debug) fprintf(stderr, "quit\n");
    int tasks_running;
    ASSERT_SYS_OK(sem_wait(&(storage->mutex)));
    tasks_running = storage->n_tasks_running;
    for (int i = 0; i < tasks_running; ++i) {
        pid_t task_pid = storage->pids[i];
        kill(task_pid, SIGKILL);
    }
    ASSERT_SYS_OK(sem_post(&(storage->mutex)));
    ASSERT_SYS_OK(sem_post(&(storage->exec_mutex)));

    // todo: clean up shared memory

    return 0;
}
