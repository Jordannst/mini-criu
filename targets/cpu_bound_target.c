#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    keep_running = 0;
}

int main(void)
{
    unsigned long long iterations = 0;
    uint64_t accumulator = 0;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("cpu_bound_target dimulai dengan PID %d\n", getpid());
    puts("Proses ini berjalan dalam loop aritmetika sederhana.");
    puts("Gunakan Ctrl+C untuk menghentikannya.");

    while (keep_running) {
        accumulator += (iterations % 97u) * (iterations % 13u);
        ++iterations;

        if (iterations % 100000000ULL == 0) {
            printf("heartbeat: iterasi=%llu akumulator=%llu\n",
                   iterations,
                   (unsigned long long)accumulator);
            fflush(stdout);
        }
    }

    printf("cpu_bound_target berhenti setelah %llu iterasi\n", iterations);
    return 0;
}
