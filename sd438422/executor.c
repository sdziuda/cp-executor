#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "utils.h"
#include "err.h"

enum {
    MAX_TASKS = 4096,
    MAX_INPUT_LENGTH = 512,
    MAX_OUTPUT_LENGTH = 1024,
    DEBUG = false
};

struct TaskOutput {
    char stdout[MAX_OUTPUT_LENGTH];
    char stderr[MAX_OUTPUT_LENGTH];
};
typedef struct TaskOutput TaskOutput;

struct SharedStorage {
    TaskOutput task_outputs[MAX_TASKS];
    pid_t task_pids[MAX_TASKS];
    int task_count;
    sem_t mutex;
    sem_t pid_already_set[MAX_TASKS];
};
typedef struct SharedStorage SharedStorage;

void exec_run(int task_number, char **args, SharedStorage *shared_storage) {
    ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));

    shared_storage->task_count++;

    char **task_args = &args[1];
    if (!task_args[0]) {
        exit(1);
    }

    int pipe_out_dsc[2], pipe_err_dsc[2];
    ASSERT_SYS_OK(pipe(pipe_out_dsc));
    ASSERT_SYS_OK(pipe(pipe_err_dsc));

    ASSERT_SYS_OK(sem_post(&shared_storage->mutex));

    pid_t pid_out = fork();
    ASSERT_SYS_OK(pid_out);
    if (!pid_out) { // buff_out
        ASSERT_SYS_OK(close(STDIN_FILENO));
        ASSERT_SYS_OK(close(pipe_out_dsc[1]));
        ASSERT_SYS_OK(close(pipe_err_dsc[0]));
        ASSERT_SYS_OK(close(pipe_err_dsc[1]));

        char buff[MAX_OUTPUT_LENGTH];
        FILE* pipe_out = fdopen(pipe_out_dsc[0], "r");

        while (fgets(buff, MAX_OUTPUT_LENGTH, pipe_out)) {
            if (strlen(buff) > 0 && buff[strlen(buff) - 1] == EOF) {
                break;
            }

            if (strlen(buff) > 0 && buff[strlen(buff) - 1] == '\n') {
                buff[strlen(buff) - 1] = '\0';
            }

            ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));
            strncpy(shared_storage->task_outputs[task_number].stdout, buff, MAX_OUTPUT_LENGTH);
            ASSERT_SYS_OK(sem_post(&shared_storage->mutex));
        }

        ASSERT_SYS_OK(fclose(pipe_out));
        exit(0);
    }

    pid_t pid_err = fork();
    ASSERT_SYS_OK(pid_err);
    if (!pid_err) { // buff_err
        ASSERT_SYS_OK(close(STDIN_FILENO));
        ASSERT_SYS_OK(close(pipe_out_dsc[0]));
        ASSERT_SYS_OK(close(pipe_out_dsc[1]));
        ASSERT_SYS_OK(close(pipe_err_dsc[1]));

        char buff[MAX_OUTPUT_LENGTH];
        FILE* pipe_err = fdopen(pipe_err_dsc[0], "r");

        while (fgets(buff, MAX_OUTPUT_LENGTH, pipe_err)) {
            if (strlen(buff) > 0 && buff[strlen(buff) - 1] == EOF) {
                break;
            }

            if (strlen(buff) > 0 && buff[strlen(buff) - 1] == '\n') {
                buff[strlen(buff) - 1] = '\0';
            }

            ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));
            strncpy(shared_storage->task_outputs[task_number].stderr, buff, MAX_OUTPUT_LENGTH);
            ASSERT_SYS_OK(sem_post(&shared_storage->mutex));
        }

        ASSERT_SYS_OK(fclose(pipe_err));
        exit(0);
    }

    pid_t pid_wait = fork();
    ASSERT_SYS_OK(pid_wait);
    if (!pid_wait) { // wait
        pid_t pid_task = fork();
        ASSERT_SYS_OK(pid_task);
        if (!pid_task) { // task
            ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));
            printf("Task %d started: pid %d.\n", task_number, getpid());
            ASSERT_SYS_OK(fflush(stdout));

            ASSERT_SYS_OK(close(STDIN_FILENO));
            ASSERT_SYS_OK(close(pipe_out_dsc[0]));
            ASSERT_SYS_OK(close(pipe_err_dsc[0]));

            ASSERT_SYS_OK(dup2(pipe_out_dsc[1], STDOUT_FILENO));
            ASSERT_SYS_OK(dup2(pipe_err_dsc[1], STDERR_FILENO));

            ASSERT_SYS_OK(close(pipe_out_dsc[1]));
            ASSERT_SYS_OK(close(pipe_err_dsc[1]));

            shared_storage->task_pids[task_number] = getpid();
            ASSERT_SYS_OK(sem_post(&shared_storage->pid_already_set[task_number]));
            ASSERT_SYS_OK(sem_post(&shared_storage->mutex));

            ASSERT_SYS_OK(execvp(task_args[0], task_args));

            exit(0);
        }

        ASSERT_SYS_OK(close(STDIN_FILENO));
        ASSERT_SYS_OK(close(pipe_out_dsc[0]));
        ASSERT_SYS_OK(close(pipe_out_dsc[1]));
        ASSERT_SYS_OK(close(pipe_err_dsc[0]));
        ASSERT_SYS_OK(close(pipe_err_dsc[1]));
        ASSERT_SYS_OK(sem_wait(&shared_storage->pid_already_set[task_number]));

        int status;
        pid_t pid = waitpid(shared_storage->task_pids[task_number], &status, 0);
        ASSERT_SYS_OK(pid);
        ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));

        shared_storage->task_count--;

        if (WIFEXITED(status)) {
            printf("Task %d ended: status %d.\n", task_number, WEXITSTATUS(status));
        } else {
            printf("Task %d ended: signalled.\n", task_number);
        }

        ASSERT_SYS_OK(sem_post(&shared_storage->mutex));
        exit(0);
    }
}

void exec_out(char *arg, SharedStorage *shared_storage) {
    long task_number = strtol(arg, NULL, 10);

    ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));

    printf("Task %ld stdout: \'%s\'.\n", task_number,
           shared_storage->task_outputs[task_number].stdout);

    ASSERT_SYS_OK(sem_post(&shared_storage->mutex));
}

void exec_err(char *arg, SharedStorage *shared_storage) {
    long task_number = strtol(arg, NULL, 10);

    ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));

    printf("Task %ld stderr: \'%s\'.\n", task_number,
           shared_storage->task_outputs[task_number].stderr);

    ASSERT_SYS_OK(sem_post(&shared_storage->mutex));
}

void exec_kill(char *arg, SharedStorage *shared_storage) {
    long task_number = strtol(arg, NULL, 10);

    kill(shared_storage->task_pids[task_number], SIGINT);
}

void exec_sleep(char *time) {
    long time_int = strtol(time, NULL, 10) * 1000;

    usleep(time_int);
}

void shared_storage_init(SharedStorage *shared_storage) {
    ASSERT_SYS_OK(sem_init(&shared_storage->mutex, 1, 1));
    for (int i = 0; i < MAX_TASKS; i++) {
        ASSERT_SYS_OK(sem_init(&shared_storage->pid_already_set[i], 1, 0));
    }
}

int main(void) {
    int task_number = 0;
    char buffer[MAX_INPUT_LENGTH];
    SharedStorage *shared_storage = mmap(
            NULL,
            sizeof(SharedStorage),
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS,
            -1,
            0);
    if (shared_storage == MAP_FAILED)
        syserr("mmap");

    shared_storage_init(shared_storage);

    while (read_line(buffer, sizeof(buffer), stdin)) {
        size_t buffer_len = strlen(buffer);

        if (buffer_len > 0 && buffer[buffer_len - 1] == '\n') {
            buffer[buffer_len - 1] = '\0';
        } else if (buffer_len == 0) {
            continue;
        }

        char **line = split_string(buffer);

        if (strcmp(line[0], "run") == 0) {
            if (DEBUG) printf("run\n");

            exec_run(task_number++, line, shared_storage);
        } else if (strcmp(line[0], "out") == 0) {
            if (DEBUG) printf("out\n");

            exec_out(line[1], shared_storage);
        } else if (strcmp(line[0], "err") == 0) {
            if (DEBUG) printf("err\n");

            exec_err(line[1], shared_storage);
        } else if (strcmp(line[0], "kill") == 0) {
            if (DEBUG) printf("kill\n");

            exec_kill(line[1], shared_storage);
        } else if (strcmp(line[0], "sleep") == 0) {
            if (DEBUG) printf("sleep\n");

            exec_sleep(line[1]);

            if (DEBUG) printf("sleep done\n");
        } else if (strcmp(line[0], "quit") == 0) {
            if (DEBUG) printf("quit\n");

            free_split_string(line);
            break;
        }

        free_split_string(line);
    }

    ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));
    for (int i = 0; i < task_number; i++) {
        kill(shared_storage->task_pids[i], SIGKILL);
    }
    ASSERT_SYS_OK(sem_post(&shared_storage->mutex));

    ASSERT_SYS_OK(sem_destroy(&shared_storage->mutex));
    for (int i = 0; i < MAX_TASKS; i++) {
        ASSERT_SYS_OK(sem_destroy(&shared_storage->pid_already_set[i]));
    }

    ASSERT_SYS_OK(munmap(shared_storage, sizeof(SharedStorage)));

    return 0;
}