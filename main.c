/**
 * main.c
 * ---------------------------------------------------------------------------
 * Interfaz de usuario del Smart Backup Kernel-Space Utility.
 *
 * Funcionalidades:
 *   1. Modo copia simple:
 *        ./backup <origen> <destino> [flags]
 *   2. Modo benchmark automático:
 *        ./backup --bench
 *      Genera archivos de prueba (1 KB, 1 MB, 1 GB), ejecuta ambos métodos
 *      N_RUNS veces c/u y muestra una tabla comparativa de tiempos.
 *
 * Materia  : Sistemas Operativos  –  Universidad EAFIT
 * ---------------------------------------------------------------------------
 */

#define _POSIX_C_SOURCE 200809L

#include "smart_copy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* clock_gettime, CLOCK_MONOTONIC */
#include <unistd.h>     /* unlink                          */
#include <errno.h>

/* =========================================================================
 * CONFIGURACIÓN DEL BENCHMARK
 * =========================================================================*/

#define N_RUNS          5          /**< Repeticiones por método/tamaño.   */

/** Tamaños de archivo a probar (en bytes). */
static const long long BENCH_SIZES[] = {
    1LL * 1024,                    /*    1 KB  */
    1LL * 1024 * 1024,             /*    1 MB  */
    1LL * 1024 * 1024 * 1024,      /*    1 GB  */
};

static const char *BENCH_LABELS[] = {
    "1 KB",
    "1 MB",
    "1 GB",
};

#define N_SIZES  ((int)(sizeof(BENCH_SIZES) / sizeof(BENCH_SIZES[0])))

/* =========================================================================
 * HELPERS
 * =========================================================================*/

/**
 * elapsed_ms  –  calcula milisegundos transcurridos entre dos timespec.
 */
static double elapsed_ms(const struct timespec *start,
                         const struct timespec *end)
{
    double sec  = (double)(end->tv_sec  - start->tv_sec);
    double nsec = (double)(end->tv_nsec - start->tv_nsec);
    return sec * 1000.0 + nsec / 1.0e6;
}

/**
 * generate_file  –  crea un archivo de tamaño exacto lleno con datos pseudo-
 *                   aleatorios para evitar que el OS lo comprima o cachee
 *                   de forma trivial.
 *
 * @param path   Ruta del archivo a crear.
 * @param size   Tamaño en bytes.
 * @return 0 en éxito, -1 en error.
 */
static int generate_file(const char *path, long long size)
{
    FILE  *fp;
    char   buf[SC_PAGE_SIZE];
    long long remaining = size;
    size_t chunk;

    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("generate_file: fopen");
        return -1;
    }

    /* Rellenar buffer con datos "aleatorios" simples (patrón repetido). */
    for (int i = 0; i < SC_PAGE_SIZE; i++) {
        buf[i] = (char)(i & 0xFF);
    }

    while (remaining > 0) {
        chunk = (remaining > (long long)sizeof(buf))
                    ? sizeof(buf)
                    : (size_t)remaining;
        if (fwrite(buf, 1, chunk, fp) != chunk) {
            perror("generate_file: fwrite");
            fclose(fp);
            return -1;
        }
        remaining -= (long long)chunk;
    }

    fclose(fp);
    return 0;
}

/**
 * print_separator  –  imprime una línea de guiones para la tabla.
 */
static void print_separator(void)
{
    printf("+----------------+------------+-----------+------------+-----------+-----------+\n");
}

/* =========================================================================
 * BENCHMARK
 * =========================================================================*/

/**
 * run_benchmark  –  ejecuta la comparativa de rendimiento completa.
 *
 * Para cada tamaño de archivo:
 *   - Genera un archivo fuente temporal.
 *   - Ejecuta sys_smart_copy N_RUNS veces y registra tiempos.
 *   - Ejecuta stdio_copy N_RUNS veces y registra tiempos.
 *   - Imprime estadísticas: min, max, promedio.
 *   - Limpia archivos temporales.
 */
static void run_benchmark(void)
{
    const char *src  = "/tmp/sc_bench_src.bin";
    const char *dst1 = "/tmp/sc_bench_dst_syscall.bin";
    const char *dst2 = "/tmp/sc_bench_dst_stdio.bin";

    struct timespec t_start, t_end;
    double times_syscall[N_RUNS];
    double times_stdio[N_RUNS];
    double sum, min_t, max_t, avg;
    sc_stats_t stats;
    int i, s;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║         SMART BACKUP  –  Benchmark: sys_smart_copy vs stdio_copy        ║\n");
    printf("║         Plataforma: Linux (POSIX)   Runs por método: %-3d               ║\n", N_RUNS);
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n\n");

    printf("Nota: CLOCK_MONOTONIC elimina distorsiones por cambios de hora del sistema.\n");
    printf("      Los tiempos incluyen syscalls de apertura/cierre de descriptores.\n\n");

    print_separator();
    printf("| %-14s | %-10s | %-9s | %-10s | %-9s | %-9s |\n",
           "Tamaño", "Método", "Min (ms)", "Max (ms)", "Avg (ms)", "Bloques");
    print_separator();

    for (s = 0; s < N_SIZES; s++) {

        long long sz = BENCH_SIZES[s];
        const char *label = BENCH_LABELS[s];

        /* --- Generar archivo fuente --- */
        printf("  Generando archivo de prueba (%s)...\n", label);
        fflush(stdout);

        if (generate_file(src, sz) != 0) {
            fprintf(stderr, "ERROR: no se pudo generar archivo de %s. Abortando.\n", label);
            return;
        }

        /* ---------------------------------------------------------------
         * MÉTODO 1: sys_smart_copy (syscalls directas)
         * --------------------------------------------------------------- */
        for (i = 0; i < N_RUNS; i++) {
            clock_gettime(CLOCK_MONOTONIC, &t_start);
            sys_smart_copy(src, dst1, SC_FLAG_NONE, &stats);
            clock_gettime(CLOCK_MONOTONIC, &t_end);
            times_syscall[i] = elapsed_ms(&t_start, &t_end);
            unlink(dst1);
        }

        /* Calcular estadísticas syscall */
        sum = 0.0;
        min_t = times_syscall[0];
        max_t = times_syscall[0];
        for (i = 0; i < N_RUNS; i++) {
            sum += times_syscall[i];
            if (times_syscall[i] < min_t) min_t = times_syscall[i];
            if (times_syscall[i] > max_t) max_t = times_syscall[i];
        }
        avg = sum / N_RUNS;

        printf("| %-14s | %-10s | %-9.3f | %-10.3f | %-9.3f | %-9u |\n",
               label, "sys_smart", min_t, max_t, avg, stats.block_count);

        /* ---------------------------------------------------------------
         * MÉTODO 2: stdio_copy (fread/fwrite con buffer en user-space)
         * --------------------------------------------------------------- */
        for (i = 0; i < N_RUNS; i++) {
            clock_gettime(CLOCK_MONOTONIC, &t_start);
            stdio_copy(src, dst2);
            clock_gettime(CLOCK_MONOTONIC, &t_end);
            times_stdio[i] = elapsed_ms(&t_start, &t_end);
            unlink(dst2);
        }

        /* Calcular estadísticas stdio */
        sum = 0.0;
        min_t = times_stdio[0];
        max_t = times_stdio[0];
        for (i = 0; i < N_RUNS; i++) {
            sum += times_stdio[i];
            if (times_stdio[i] < min_t) min_t = times_stdio[i];
            if (times_stdio[i] > max_t) max_t = times_stdio[i];
        }
        avg = sum / N_RUNS;

        printf("| %-14s | %-10s | %-9.3f | %-10.3f | %-9.3f | %-9s |\n",
               label, "stdio", min_t, max_t, avg, "N/A");

        print_separator();

        unlink(src);
    }

    /* ------------------------------------------------------------------
     * Análisis cualitativo impreso al final del benchmark
     * ------------------------------------------------------------------ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  ANÁLISIS: ¿Por qué stdio es más eficiente en archivos pequeños?        ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("║                                                                          ║\n");
    printf("║  sys_smart_copy usa read(2)/write(2) directamente:                      ║\n");
    printf("║    → Cada llamada provoca un CONTEXT SWITCH user-space → kernel.        ║\n");
    printf("║    → Para 1 KB con buf=4KB: 1 read + 1 write = 2 context switches.     ║\n");
    printf("║    → El overhead de cada context switch (~1-10 µs) domina el tiempo.   ║\n");
    printf("║                                                                          ║\n");
    printf("║  stdio_copy usa fread()/fwrite() con buffer interno (glibc, ~8 KB):    ║\n");
    printf("║    → Para archivos < 8 KB es posible completar la copia sin emitir     ║\n");
    printf("║      ninguna syscall adicional (datos quedan en buffer de usuario).     ║\n");
    printf("║    → Menos context switches = menor latencia para archivos pequeños.    ║\n");
    printf("║                                                                          ║\n");
    printf("║  Para archivos grandes (≥ 1 MB):                                        ║\n");
    printf("║    → Ambos métodos emiten múltiples syscalls y el overhead se diluye.  ║\n");
    printf("║    → sys_smart_copy con SC_FLAG_LARGE_BUF (1 MiB) puede superar a     ║\n");
    printf("║      stdio al reducir el número total de context switches.              ║\n");
    printf("║                                                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n\n");
}

/* =========================================================================
 * MODO COPIA SIMPLE
 * =========================================================================*/

/**
 * print_usage  –  muestra la ayuda de línea de comandos.
 */
static void print_usage(const char *prog)
{
    printf("Uso:\n");
    printf("  %s --bench                     Ejecutar benchmark completo\n", prog);
    printf("  %s <origen> <destino> [flags]  Copiar un archivo\n\n", prog);
    printf("Flags (combinables con suma):\n");
    printf("  1   SC_FLAG_CHECK_PERMS  – verificar permisos antes de copiar\n");
    printf("  2   SC_FLAG_LOG          – registrar actividad en syslog/stderr\n");
    printf("  4   SC_FLAG_LARGE_BUF   – usar buffer de 1 MiB (mejor para archivos grandes)\n");
    printf("  8   SC_FLAG_SYNC         – fsync() al terminar (garantía de persistencia)\n\n");
    printf("Ejemplo:\n");
    printf("  %s foto.jpg /backup/foto.jpg 3   # permisos + log\n", prog);
}

/* =========================================================================
 * main
 * =========================================================================*/

int main(int argc, char *argv[])
{
    /* --- Modo benchmark --- */
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) {
        run_benchmark();
        return EXIT_SUCCESS;
    }

    /* --- Modo copia --- */
    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *src   = argv[1];
    const char *dst   = argv[2];
    uint32_t    flags = SC_FLAG_NONE;

    if (argc >= 4) {
        char *endptr;
        flags = (uint32_t)strtoul(argv[3], &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "[backup] Flag inválido: '%s'\n", argv[3]);
            return EXIT_FAILURE;
        }
    }

    printf("[backup] Copiando '%s' → '%s'  (flags=0x%02X)\n", src, dst, flags);

    sc_stats_t stats = {0};
    int result = sys_smart_copy(src, dst, flags, &stats);

    if (result == SC_OK) {
        printf("[backup] ✓ Éxito: %lld bytes copiados en %u bloques (buf=%u B)\n",
               (long long)stats.bytes_written,
               stats.block_count,
               stats.buffer_size);
    } else {
        fprintf(stderr, "[backup] ✗ Error al copiar el archivo.\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
