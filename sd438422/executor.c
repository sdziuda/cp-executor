#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "utils.h"
#include "err.h"

enum {
    MAX_TASKS = 4096,
    MAX_INPUT_LENGTH = 512,
    MAX_OUTPUT_LENGTH = 1024
};

struct SharedStorage {
    char task_out[MAX_TASKS][MAX_OUTPUT_LENGTH];
    char task_err[MAX_TASKS][MAX_OUTPUT_LENGTH];
    pid_t task_pid[MAX_TASKS];
    bool finished[MAX_TASKS];

    sem_t mutex;
    sem_t exec;
    sem_t pid_available[MAX_TASKS];
};
typedef struct SharedStorage SharedStorage;

void exec_run(int task_number, char **args, SharedStorage *shared_storage) {
    ASSERT_SYS_OK(sem_wait(&shared_storage->exec));

    char **task_args = &args[1];
    if (!task_args[0]) {
        exit(1);
    }

    int pipe_out_dsc[2], pipe_err_dsc[2];
    ASSERT_SYS_OK(pipe(pipe_out_dsc));
    ASSERT_SYS_OK(pipe(pipe_err_dsc));

    pid_t pid_out = fork();
    ASSERT_SYS_OK(pid_out);
    if (!pid_out) { // buff_out
        ASSERT_SYS_OK(close(STDIN_FILENO));
        ASSERT_SYS_OK(close(pipe_out_dsc[1]));
        ASSERT_SYS_OK(close(pipe_err_dsc[0]));
        ASSERT_SYS_OK(close(pipe_err_dsc[1]));
        free_split_string(args);

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
            strncpy(shared_storage->task_out[task_number], buff, MAX_OUTPUT_LENGTH);
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
        free_split_string(args);

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
            strncpy(shared_storage->task_err[task_number], buff, MAX_OUTPUT_LENGTH);
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
            ASSERT_SYS_OK(close(STDIN_FILENO));
            ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));
            printf("Task %d started: pid %d.\n", task_number, getpid());
            ASSERT_SYS_OK(fflush(stdout));

            ASSERT_SYS_OK(close(pipe_out_dsc[0]));
            ASSERT_SYS_OK(close(pipe_err_dsc[0]));

            ASSERT_SYS_OK(dup2(pipe_out_dsc[1], STDOUT_FILENO));
            ASSERT_SYS_OK(dup2(pipe_err_dsc[1], STDERR_FILENO));

            ASSERT_SYS_OK(close(pipe_out_dsc[1]));
            ASSERT_SYS_OK(close(pipe_err_dsc[1]));

            shared_storage->task_pid[task_number] = getpid();
            ASSERT_SYS_OK(sem_post(&shared_storage->pid_available[task_number]));
            ASSERT_SYS_OK(sem_post(&shared_storage->mutex));
            ASSERT_SYS_OK(sem_post(&shared_storage->exec));

            ASSERT_SYS_OK(execvp(task_args[0], task_args));

            exit(0);
        }

        ASSERT_SYS_OK(close(STDIN_FILENO));
        ASSERT_SYS_OK(close(pipe_out_dsc[0]));
        ASSERT_SYS_OK(close(pipe_out_dsc[1]));
        ASSERT_SYS_OK(close(pipe_err_dsc[0]));
        ASSERT_SYS_OK(close(pipe_err_dsc[1]));
        free_split_string(args);
        ASSERT_SYS_OK(sem_wait(&shared_storage->pid_available[task_number]));

        pid_t task_pid = shared_storage->task_pid[task_number];

        ASSERT_SYS_OK(sem_post(&shared_storage->pid_available[task_number]));

        int status;
        pid_t wait_res = waitpid(task_pid, &status, 0);
        ASSERT_SYS_OK(wait_res);
        ASSERT_SYS_OK(sem_wait(&shared_storage->exec));

        ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));
        shared_storage->finished[task_number] = true;
        ASSERT_SYS_OK(sem_post(&shared_storage->mutex));

        if (WIFEXITED(status)) {
            printf("Task %d ended: status %d.\n", task_number, WEXITSTATUS(status));
        } else {
            printf("Task %d ended: signalled.\n", task_number);
        }
        ASSERT_SYS_OK(fflush(stdout));

        ASSERT_SYS_OK(sem_post(&shared_storage->exec));
        exit(0);
    }
}

void exec_out(char *arg, SharedStorage *shared_storage) {
    ASSERT_SYS_OK(sem_wait(&shared_storage->exec));

    long task_number = strtol(arg, NULL, 10);

    ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));

    printf("Task %ld stdout: \'%s\'.\n", task_number,
           shared_storage->task_out[task_number]);
    ASSERT_SYS_OK(fflush(stdout));

    ASSERT_SYS_OK(sem_post(&shared_storage->mutex));

    ASSERT_SYS_OK(sem_post(&shared_storage->exec));
}

void exec_err(char *arg, SharedStorage *shared_storage) {
    ASSERT_SYS_OK(sem_wait(&shared_storage->exec));

    long task_number = strtol(arg, NULL, 10);

    ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));

    printf("Task %ld stderr: \'%s\'.\n", task_number,
           shared_storage->task_err[task_number]);
    ASSERT_SYS_OK(fflush(stdout));

    ASSERT_SYS_OK(sem_post(&shared_storage->mutex));

    ASSERT_SYS_OK(sem_post(&shared_storage->exec));
}

void exec_kill(char *arg, SharedStorage *shared_storage) {
    ASSERT_SYS_OK(sem_wait(&shared_storage->exec));

    long task_number = strtol(arg, NULL, 10);

    ASSERT_SYS_OK(sem_wait(&shared_storage->pid_available[task_number]));

    if (!shared_storage->finished[task_number]) {
        kill(shared_storage->task_pid[task_number], SIGINT);
    }

    ASSERT_SYS_OK(sem_post(&shared_storage->pid_available[task_number]));

    ASSERT_SYS_OK(sem_post(&shared_storage->exec));
}

void exec_sleep(char *time) {
    long time_int = strtol(time, NULL, 10) * 1000;

    usleep(time_int);
}

void shared_storage_init(SharedStorage *shared_storage) {
    ASSERT_SYS_OK(sem_init(&shared_storage->mutex, 1, 1));
    ASSERT_SYS_OK(sem_init(&shared_storage->exec, 1, 1));
    for (int i = 0; i < MAX_TASKS; i++) {
        shared_storage->finished[i] = false;
        ASSERT_SYS_OK(sem_init(&shared_storage->pid_available[i], 1, 0));
    }
}

void shared_storage_destroy(SharedStorage *shared_storage, int task_count) {
    ASSERT_SYS_OK(sem_wait(&shared_storage->exec));
    ASSERT_SYS_OK(sem_wait(&shared_storage->mutex));
    for (int i = 0; i < task_count; i++) {
        if (shared_storage->task_pid[i] != 0 && shared_storage->task_pid[i] != getpid()) {
            ASSERT_SYS_OK(sem_wait(&shared_storage->pid_available[i]));

            if (!shared_storage->finished[i]) {
                kill(shared_storage->task_pid[i], SIGKILL);
                shared_storage->finished[i] = true;
            }

            ASSERT_SYS_OK(sem_post(&shared_storage->pid_available[i]));
        }
    }
    ASSERT_SYS_OK(sem_post(&shared_storage->mutex));
    ASSERT_SYS_OK(sem_post(&shared_storage->exec));

    ASSERT_SYS_OK(sem_destroy(&shared_storage->mutex));
    ASSERT_SYS_OK(sem_destroy(&shared_storage->exec));
    for (int i = 0; i < MAX_TASKS; i++) {
        ASSERT_SYS_OK(sem_destroy(&shared_storage->pid_available[i]));
    }

    ASSERT_SYS_OK(munmap(shared_storage, sizeof(SharedStorage)));
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
        }

        if (strlen(buffer) == 0) {
            continue;
        }

        char **line = split_string(buffer);

        if (strcmp(line[0], "run") == 0) {
            exec_run(task_number++, line, shared_storage);
        } else if (strcmp(line[0], "out") == 0) {
            exec_out(line[1], shared_storage);
        } else if (strcmp(line[0], "err") == 0) {
            exec_err(line[1], shared_storage);
        } else if (strcmp(line[0], "kill") == 0) {
            exec_kill(line[1], shared_storage);
        } else if (strcmp(line[0], "sleep") == 0) {
            exec_sleep(line[1]);
        } else if (strcmp(line[0], "quit") == 0) {
            free_split_string(line);
            break;
        }

        free_split_string(line);
    }

    shared_storage_destroy(shared_storage, task_number);

    return 0;
}