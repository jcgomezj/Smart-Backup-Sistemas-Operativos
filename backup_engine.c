/**
 * backup_engine.c
 * ---------------------------------------------------------------------------
 * Motor de respaldo del Smart Backup Kernel-Space Utility.
 *
 * Basado en el código base del profesor, extendido con:
 *   - Manejo completo de errno (ENOENT, EACCES, ENOSPC, ENOMEM, EIO).
 *   - Registro de actividad via syslog(3) (equivalente a printk en kernel).
 *   - Escrituras parciales manejadas con bucle interno.
 *   - stdio_copy() como implementación de referencia para el benchmark.
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
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <syslog.h>

/* Variable global para silenciar stderr durante el benchmark.
 * Se activa desde main.c antes de run_benchmark(). */
int sc_silent_mode = 0;

/* =========================================================================
 * HELPER: log interno
 * =========================================================================
 * En un módulo de kernel real esto sería printk(KERN_INFO ...).
 * En userspace usamos syslog(3) + stderr para máxima visibilidad.
 * En modo benchmark (sc_silent_mode=1) solo escribe en syslog.
 */
static void sc_log(int priority, const char *fmt, ...)
{
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    syslog(priority, "%s", msg);
    if (!sc_silent_mode) {
        fprintf(stderr, "[smart_backup] %s\n", msg);
    }
}

/* =========================================================================
 * copy_file()
 * =========================================================================
 * Copia un archivo usando syscalls POSIX directas: open, read, write, close.
 *
 * Preserva los permisos del archivo original (st.st_mode) tal como lo hace
 * el código base del profesor. Agrega:
 *   - Manejo de escrituras parciales con bucle interno.
 *   - Detección de ENOSPC en write(2).
 *   - Registro de la operación en syslog.
 *   - Cierre garantizado de descriptores en toda ruta de error.
 *
 * Por qué es una función "de sistema":
 *   - Usa las mismas primitivas que el kernel: open(2)/read(2)/write(2).
 *   - Buffer de PAGE_SIZE (4096 B) alineado a la granularidad del VFS.
 *   - Registra en syslog, equivalente a printk(KERN_INFO) en un módulo real.
 * =========================================================================*/
void copy_file(const char *src, const char *dest)
{
    int     fd_src  = -1;
    int     fd_dest = -1;
    ssize_t bytes_read;
    ssize_t bytes_written;
    ssize_t total_written;
    char    buffer[BUFFER_SIZE];
    struct  stat st;

    /* --- Obtener permisos del archivo original (igual que código base) --- */
    if (stat(src, &st) == -1) {
        perror("Error al obtener los stats del archivo de origen");
        return;
    }

    /* --- Registrar inicio de operación (extensión: syslog) --- */
    openlog("smart_backup", LOG_PID | LOG_CONS, LOG_USER);
    sc_log(LOG_INFO, "Iniciando copia: '%s' -> '%s'", src, dest);

    /* --- open() lectura — syscall directa al VFS --- */
    fd_src = open(src, O_RDONLY);
    if (fd_src < 0) {
        perror("Error al abrir el archivo de origen");
        /* errno puede ser ENOENT (no existe) o EACCES (sin permisos) */
        sc_log(LOG_ERR, "open() origen falló: %s", strerror(errno));
        goto cleanup;
    }

    /* --- open() escritura — preserva permisos originales --- */
    fd_dest = open(dest, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (fd_dest < 0) {
        perror("Error al crear/abrir el archivo de destino");
        sc_log(LOG_ERR, "open() destino falló: %s", strerror(errno));
        goto cleanup;
    }

    /* --- Bucle read(2) → write(2) ---
     *
     * Cada iteración provoca DOS context switches (user→kernel→user):
     *   1. read(2):  kernel copia datos del page cache al buffer de usuario.
     *   2. write(2): kernel copia buffer al page cache del destino.
     *
     * Extensión respecto al código base:
     *   - write() puede retornar menos bytes que los solicitados (escritura
     *     parcial). El bucle interno reintenta hasta completar o detectar error.
     *   - Se detecta ENOSPC (disco lleno) explícitamente.
     */
    while ((bytes_read = read(fd_src, buffer, BUFFER_SIZE)) > 0) {

        total_written = 0;

        while (total_written < bytes_read) {
            bytes_written = write(fd_dest,
                                  buffer + total_written,
                                  (size_t)(bytes_read - total_written));
            if (bytes_written < 0) {
                if (errno == ENOSPC) {
                    fprintf(stderr, "Error: disco lleno al escribir en '%s'\n", dest);
                    sc_log(LOG_ERR, "write() falló: disco lleno");
                } else {
                    perror("Error al escribir en el archivo de destino");
                    sc_log(LOG_ERR, "write() falló: %s", strerror(errno));
                }
                goto cleanup;
            }
            total_written += bytes_written;
        }
    }

    if (bytes_read < 0) {
        perror("Error al leer el archivo de origen");
        sc_log(LOG_ERR, "read() falló: %s", strerror(errno));
        goto cleanup;
    }

    /* --- Éxito --- */
    if (!sc_silent_mode) {
        printf("[OK] Archivo respaldado: %s -> %s\n", src, dest);
    }
    sc_log(LOG_INFO, "Copia completada: '%s' -> '%s'", src, dest);

cleanup:
    if (fd_src  >= 0) close(fd_src);
    if (fd_dest >= 0) close(fd_dest);
    closelog();
}

/* =========================================================================
 * copy_directory()
 * =========================================================================
 * Copia un directorio de forma recursiva usando opendir/readdir/closedir.
 * Código base del profesor, sin modificaciones estructurales.
 * =========================================================================*/
void copy_directory(const char *src, const char *dest)
{
    struct stat st;

    if (stat(src, &st) == -1) {
        perror("Error al obtener stats del directorio de origen");
        return;
    }

    /* mkdir() — crea el directorio destino preservando permisos */
    if (mkdir(dest, st.st_mode) == -1) {
        if (errno != EEXIST) {
            perror("Error al crear el directorio de destino");
            return;
        }
    } else {
        printf("[OK] Directorio creado: %s\n", dest);
    }

    /* opendir() — internamente usa open(2) + getdents(2) */
    DIR *dir = opendir(src);
    if (!dir) {
        perror("Error al abrir el directorio de origen");
        return;
    }

    struct dirent *entry;
    char next_src[1024];
    char next_dest[1024];

    while ((entry = readdir(dir)) != NULL) {

        /* Ignorar "." y ".." para evitar recursión infinita */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(next_src,  sizeof(next_src),  "%s/%s", src,  entry->d_name);
        snprintf(next_dest, sizeof(next_dest), "%s/%s", dest, entry->d_name);

        struct stat next_st;
        /* lstat() — detecta links simbólicos para evitar ciclos */
        if (lstat(next_src, &next_st) == -1) {
            perror("Error al obtener los stats de un elemento");
            continue;
        }

        if (S_ISDIR(next_st.st_mode)) {
            copy_directory(next_src, next_dest);
        } else if (S_ISREG(next_st.st_mode)) {
            copy_file(next_src, next_dest);
        } else {
            /* Links simbólicos, character devices, etc. se ignoran */
            printf("[INFO] Elemento ignorado (especial o link): %s\n", next_src);
        }
    }

    closedir(dir);
}

/* =========================================================================
 * stdio_copy()
 * =========================================================================
 * Implementación de referencia usando stdio.h (fread/fwrite).
 * Se usa ÚNICAMENTE para el benchmark comparativo.
 *
 * Diferencia clave con copy_file():
 *   stdio mantiene un buffer en ESPACIO DE USUARIO (~8 KB en glibc).
 *   Esto reduce la frecuencia de syscalls read/write y por tanto el número
 *   de context switches, haciéndolo más eficiente en ciertos escenarios.
 * =========================================================================*/
int stdio_copy(const char *src, const char *dest)
{
    FILE  *fp_src  = NULL;
    FILE  *fp_dest = NULL;
    char   buf[BUFFER_SIZE];
    size_t n_read, n_written;
    int    ret = SC_OK;

    fp_src = fopen(src, "rb");
    if (!fp_src) {
        perror("fopen() origen");
        return SC_ERROR;
    }

    fp_dest = fopen(dest, "wb");
    if (!fp_dest) {
        perror("fopen() destino");
        fclose(fp_src);
        return SC_ERROR;
    }

    /*
     * fread/fwrite usan el buffer interno de FILE*.
     * Para archivos pequeños (< 8 KB) es posible que no se emita
     * ninguna syscall adicional — los datos quedan en el buffer de usuario.
     */
    while ((n_read = fread(buf, 1, sizeof(buf), fp_src)) > 0) {
        n_written = fwrite(buf, 1, n_read, fp_dest);
        if (n_written != n_read) {
            perror("fwrite() destino");
            ret = SC_ERROR;
            break;
        }
    }

    if (ret == SC_OK && ferror(fp_src)) {
        perror("fread() origen");
        ret = SC_ERROR;
    }

    fclose(fp_src);
    fclose(fp_dest);
    return ret;
}
