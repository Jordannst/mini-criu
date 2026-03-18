#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

#define BUFFER_SIZE (64 * 1024 * 1024)
#define PAGE_STRIDE 4096

static volatile sig_atomic_t keep_running = 1;

/*
 * Handler sinyal sederhana agar target bisa keluar dengan rapi saat diuji.
 */
static void handle_signal(int signo)
{
    (void)signo;
    keep_running = 0;
}

/*
 * Pada Linux/WSL modern, ptrace untuk proses sibling sering dibatasi.
 * Dummy target ini secara eksplisit mengizinkan tracing agar fitur freeze
 * dapat diuji dari CLI mini-criu selama pengembangan.
 */
static void allow_ptrace_for_demo(void)
{
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) == -1) {
        perror("gagal mengatur izin ptrace");
    }
}

/*
 * Program ini menjaga area memori cukup besar tetap aktif dan terus berubah.
 *
 * Target seperti ini berguna untuk menguji parsing maps dan dump memori karena
 * heap serta region writable lain akan berisi data yang berubah dari waktu ke
 * waktu.
 */
int main(void)
{
    unsigned char *buffer;
    size_t pass = 0;
    unsigned long checksum = 0;

    /* Daftarkan handler agar proses dapat dihentikan dari terminal dengan rapi. */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    allow_ptrace_for_demo();

    /* Buffer besar ini dipakai untuk memaksa adanya aktivitas tulis di memori. */
    buffer = malloc(BUFFER_SIZE);
    if (buffer == NULL) {
        perror("gagal malloc");
        return 1;
    }

    printf("memory_bound_target dimulai dengan PID %d\n", getpid());
    printf("Buffer %d MiB dialokasikan untuk penulisan halaman berulang.\n", BUFFER_SIZE / (1024 * 1024));
    puts("Gunakan Ctrl+C untuk menghentikannya.");

    /*
     * Setiap putaran menulis satu byte per halaman. Pola ini sengaja ringan,
     * tetapi cukup untuk membuat isi memori berubah dan mudah diamati.
     */
    while (keep_running) {
        checksum = 0;

        for (size_t i = 0; i < BUFFER_SIZE; i += PAGE_STRIDE) {
            buffer[i] = (unsigned char)((i / PAGE_STRIDE + pass) % 256u);
            checksum += buffer[i];
        }

        ++pass;
        /* Heartbeat memperlihatkan bahwa buffer terus diperbarui. */
        printf("heartbeat: putaran=%zu checksum=%lu\n", pass, checksum);
        fflush(stdout);
        sleep(1);
    }

    /* Bebaskan buffer sebelum proses selesai. */
    free(buffer);
    printf("memory_bound_target berhenti setelah %zu putaran\n", pass);
    return 0;
}
