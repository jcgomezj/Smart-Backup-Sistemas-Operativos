/**
 * main.c
 * ---------------------------------------------------------------------------
 * Interfaz de usuario del Smart Backup Kernel-Space Utility.
 *
 * Basado en el código base del profesor. Agrega el modo --bench para la
 * comparativa de rendimiento requerida en la rúbrica.
 *
 * Modos de uso:
 *   ./backup -h                        Ayuda
 *   ./backup -b <origen> <destino>     Respaldar archivo o directorio
 *   ./backup --bench                   Benchmark: syscalls vs stdio
 *
 * Materia  : Sistemas Operativos – Universidad EAFIT
 * Autores  : Juan José Barón · Estefanía Ramírez · Juan Camilo Gómez Jiménez
 * ---------------------------------------------------------------------------
 */

#define _POSIX_C_SOURCE 200809L

#include "smart_copy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* clock_gettime, CLOCK_MONOTONIC */
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* =========================================================================
 * CONFIGURACIÓN DEL BENCHMARK
 * =========================================================================*/

#define N_RUNS  5   /**< Repeticiones por método/tamaño */

static const long long BENCH_SIZES[] = {
    1LL * 1024,                    /*   1 KB */
    1LL * 1024 * 1024,             /*   1 MB */
    1LL * 1024 * 1024 * 1024,      /*   1 GB */
};
static const char *BENCH_LABELS[] = { "1 KB", "1 MB", "1 GB" };
#define N_SIZES  ((int)(sizeof(BENCH_SIZES) / sizeof(BENCH_SIZES[0])))

/* =========================================================================
 * print_help()  —  igual al código base del profesor
 * =========================================================================*/
void print_help(const char *prog_name)
{
    printf("==========================================\n");
    printf("      SISTEMA DE BACKUP C - SysCalls      \n");
    printf("==========================================\n");
    printf("Uso: %s [OPCIÓN] [ORIGEN] [DESTINO]\n", prog_name);
    printf("\nPrueba de concepto de un sistema de copias de seguridad usando las\n");
    printf("llamadas al sistema de POSIX/Linux (open, read, write, close, mkdir, etc.).\n\n");
    printf("Opciones:\n");
    printf("  -h, --help    Muestra esta ayuda.\n");
    printf("  -b, --backup  Respaldar un archivo o directorio recursivamente.\n");
    printf("  --bench       Ejecutar benchmark: syscalls vs stdio (1KB, 1MB, 1GB).\n");
    printf("\nEjemplos:\n");
    printf("  %s -b archivo.txt        backup_archivo.txt\n", prog_name);
    printf("  %s -b /home/user/docs    /tmp/backup_docs\n",   prog_name);
    printf("  %s --bench\n\n", prog_name);
}

/* =========================================================================
 * HELPERS DEL BENCHMARK
 * =========================================================================*/

static double elapsed_ms(const struct timespec *s, const struct timespec *e)
{
    return (e->tv_sec - s->tv_sec) * 1000.0
         + (e->tv_nsec - s->tv_nsec) / 1.0e6;
}

static int generate_file(const char *path, long long size)
{
    FILE  *fp;
    char   buf[BUFFER_SIZE];
    long long rem = size;
    size_t chunk;

    fp = fopen(path, "wb");
    if (!fp) { perror("generate_file"); return -1; }

    for (int i = 0; i < BUFFER_SIZE; i++) buf[i] = (char)(i & 0xFF);

    while (rem > 0) {
        chunk = (rem > (long long)sizeof(buf)) ? sizeof(buf) : (size_t)rem;
        if (fwrite(buf, 1, chunk, fp) != chunk) {
            perror("generate_file fwrite"); fclose(fp); return -1;
        }
        rem -= (long long)chunk;
    }
    fclose(fp);
    return 0;
}

static void print_sep(void)
{
    printf("+----------------+------------+-----------+------------+-----------+-----------+\n");
}

/* =========================================================================
 * run_benchmark()
 * =========================================================================
 * Genera archivos de prueba (1 KB, 1 MB, 1 GB), ejecuta copy_file() y
 * stdio_copy() N_RUNS veces cada uno y muestra tabla comparativa con
 * min, max y promedio de tiempos usando CLOCK_MONOTONIC.
 * =========================================================================*/
static void run_benchmark(void)
{
    const char *src  = "/tmp/sc_bench_src.bin";
    const char *dst1 = "/tmp/sc_bench_dst_syscall.bin";
    const char *dst2 = "/tmp/sc_bench_dst_stdio.bin";

    struct timespec t0, t1;
    double ts[N_RUNS], tst[N_RUNS];
    double sum, mn, mx, avg;
    int i, s;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║      SMART BACKUP  –  Benchmark: copy_file (syscalls) vs stdio_copy    ║\n");
    printf("║      Plataforma: Linux (POSIX)        Runs por método: %-3d             ║\n", N_RUNS);
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n\n");
    printf("Nota: CLOCK_MONOTONIC elimina distorsiones por cambios de hora del sistema.\n");
    printf("      Los tiempos incluyen apertura y cierre de descriptores.\n\n");

    print_sep();
    printf("| %-14s | %-10s | %-9s | %-10s | %-9s | %-9s |\n",
           "Tamaño", "Método", "Min (ms)", "Max (ms)", "Avg (ms)", "Bloques");
    print_sep();

    for (s = 0; s < N_SIZES; s++) {

        printf("| Generando %-4s...                                                        |\n",
               BENCH_LABELS[s]);
        fflush(stdout);

        if (generate_file(src, BENCH_SIZES[s]) != 0) {
            fprintf(stderr, "ERROR: no se pudo generar archivo de %s\n", BENCH_LABELS[s]);
            return;
        }

        /* --- copy_file (syscalls directas) --- */
        for (i = 0; i < N_RUNS; i++) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            copy_file(src, dst1);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ts[i] = elapsed_ms(&t0, &t1);
            unlink(dst1);
        }
        sum = 0; mn = ts[0]; mx = ts[0];
        for (i = 0; i < N_RUNS; i++) {
            sum += ts[i];
            if (ts[i] < mn) mn = ts[i];
            if (ts[i] > mx) mx = ts[i];
        }
        avg = sum / N_RUNS;
        printf("| %-14s | %-10s | %-9.3f | %-10.3f | %-9.3f | %-9lld |\n",
               BENCH_LABELS[s], "syscalls", mn, mx, avg,
               BENCH_SIZES[s] / BUFFER_SIZE + (BENCH_SIZES[s] % BUFFER_SIZE ? 1 : 0));

        /* --- stdio_copy (fread/fwrite) --- */
        for (i = 0; i < N_RUNS; i++) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            stdio_copy(src, dst2);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            tst[i] = elapsed_ms(&t0, &t1);
            unlink(dst2);
        }
        sum = 0; mn = tst[0]; mx = tst[0];
        for (i = 0; i < N_RUNS; i++) {
            sum += tst[i];
            if (tst[i] < mn) mn = tst[i];
            if (tst[i] > mx) mx = tst[i];
        }
        avg = sum / N_RUNS;
        printf("| %-14s | %-10s | %-9.3f | %-10.3f | %-9.3f | %-9s |\n",
               BENCH_LABELS[s], "stdio", mn, mx, avg, "N/A");

        print_sep();
        unlink(src);
    }

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  ANÁLISIS: syscalls directas vs buffer de espacio de usuario (stdio)   ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("║                                                                          ║\n");
    printf("║  copy_file() usa read(2)/write(2) directamente:                        ║\n");
    printf("║    → Cada llamada provoca un CONTEXT SWITCH user-space → kernel.       ║\n");
    printf("║    → Para 1 KB con buf=4KB: 1 read + 1 write = 2 context switches.    ║\n");
    printf("║    → Para 1 GB: hasta 524.288 context switches (262144 x 2).          ║\n");
    printf("║                                                                          ║\n");
    printf("║  stdio_copy() usa fread()/fwrite() con buffer interno (~8 KB glibc):  ║\n");
    printf("║    → Para archivos < 8 KB puede completarse sin syscalls extra.        ║\n");
    printf("║    → Menos context switches = menor latencia y mayor estabilidad.      ║\n");
    printf("║                                                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n\n");
}

/* =========================================================================
 * main()
 * =========================================================================*/
int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    /* --- Ayuda --- */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_help(argv[0]);
        return EXIT_SUCCESS;
    }

    /* --- Benchmark --- */
    if (strcmp(argv[1], "--bench") == 0) {
        sc_silent_mode = 1;
        run_benchmark();
        return EXIT_SUCCESS;
    }

    /* --- Backup (igual al código base del profesor) --- */
    if (strcmp(argv[1], "-b") == 0 || strcmp(argv[1], "--backup") == 0) {

        if (argc != 4) {
            fprintf(stderr,
                    "Error: Faltan argumentos. Forma correcta: %s -b origen destino\n",
                    argv[0]);
            return EXIT_FAILURE;
        }

        const char *src  = argv[2];
        const char *dest = argv[3];
        struct stat st;

        if (stat(src, &st) == -1) {
            perror("Error comprobando el directorio/archivo de origen");
            return EXIT_FAILURE;
        }

        if (S_ISDIR(st.st_mode)) {
            printf("--- Iniciando respaldo del directorio '%s' en '%s' ---\n", src, dest);
            copy_directory(src, dest);
            printf("--- Respaldo completado ---\n");
        } else if (S_ISREG(st.st_mode)) {
            printf("--- Iniciando respaldo del archivo '%s' en '%s' ---\n", src, dest);
            copy_file(src, dest);
            printf("--- Respaldo completado ---\n");
        } else {
            fprintf(stderr,
                    "Error: El origen no es válido. Debe ser carpeta o archivo.\n");
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    fprintf(stderr, "Error: Opción no reconocida '%s'.\n", argv[1]);
    print_help(argv[0]);
    return EXIT_FAILURE;
}
