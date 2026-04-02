/**
 * backup_engine.c
 * ---------------------------------------------------------------------------
 * Lógica central del Smart Backup Kernel-Space Utility.
 *
 * Simula el comportamiento de una syscall de copia que operaría en espacio
 * de kernel:
 *   - Usa descriptores de archivo POSIX (open/read/write/close) en lugar
 *     de FILE* para evitar el buffer de espacio de usuario de stdio.
 *   - Gestiona un buffer alineado a PAGE_SIZE (4096 bytes) para maximizar
 *     el throughput en transferencias de bloque.
 *   - Valida permisos, maneja errno en cada syscall crítica y garantiza
 *     que no queden descriptores abiertos ante cualquier ruta de error.
 *   - Registra la actividad via syslog(3) / stderr cuando SC_FLAG_LOG
 *     está activo (equivalente a printk en un módulo real).
 *
 * Materia  : Sistemas Operativos  –  Universidad EAFIT
 * ---------------------------------------------------------------------------
 */

#define _POSIX_C_SOURCE 200809L   /* Para fdatasync, syslog, etc. */

#include "smart_copy.h"

#include <stdio.h>      /* perror, fprintf, fopen, fread, fwrite, fclose */
#include <stdlib.h>     /* malloc, free                                  */
#include <string.h>     /* strerror                                      */
#include <stdarg.h>     /* va_list, va_start, va_end                     */
#include <errno.h>      /* errno, ENOENT, EACCES, ENOSPC, ENOMEM, EIO   */
#include <unistd.h>     /* open, read, write, close, access, fsync       */
#include <fcntl.h>      /* O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC          */
#include <sys/types.h>  /* off_t, ssize_t                                */
#include <sys/stat.h>   /* fstat, struct stat, S_IRUSR, S_IWUSR          */
#include <syslog.h>     /* openlog, syslog, vsyslog, closelog            */

/* =========================================================================
 * HELPERS INTERNOS  (no expuestos en el header)
 * =========================================================================*/

/**
 * log_event  –  registra un mensaje de log cuando SC_FLAG_LOG está activo.
 *
 * En producción (módulo de kernel) se usaría printk(KERN_INFO ...).
 * En userspace combinamos syslog(3) + stderr para máxima visibilidad.
 *
 * @param priority   Prioridad syslog (LOG_INFO, LOG_ERR, ...).
 * @param fmt        Formato printf.
 */
static void log_event(int priority, const char *fmt, ...)
{
    char msg[512];
    va_list args;

    /* Formatear el mensaje en un buffer intermedio */
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* --- syslog --- */
    syslog(priority, "%s", msg);

    /* --- stderr (útil en desarrollo y en la terminal del alumno) --- */
    fprintf(stderr, "[smart_copy] %s\n", msg);
}

/* =========================================================================
 * sc_perror  –  wrapper de perror() con prefijo uniforme
 * =========================================================================*/

void sc_perror(const char *context)
{
    fprintf(stderr, "[smart_copy ERROR] %s: %s\n", context, strerror(errno));
}

/* =========================================================================
 * sys_smart_copy  –  copia binaria con syscalls de bajo nivel
 * =========================================================================
 *
 * Flujo interno (imita lo que haría el kernel):
 *
 *   1. Validar punteros de entrada (no NULL).
 *   2. Verificar permisos con access(2) si SC_FLAG_CHECK_PERMS está activo.
 *   3. Abrir src con open(O_RDONLY)  → fd_src.
 *   4. Obtener tamaño del archivo con fstat(2).
 *   5. Abrir/crear dst con open(O_WRONLY|O_CREAT|O_TRUNC) → fd_dst.
 *   6. Asignar buffer de PAGE_SIZE (o PAGE_SIZE*256 con SC_FLAG_LARGE_BUF).
 *   7. Bucle read→write hasta EOF.  Detectar ENOSPC en cada write(2).
 *   8. fsync(fd_dst) si SC_FLAG_SYNC está activo.
 *   9. Rellenar sc_stats_t si el puntero no es NULL.
 *  10. Cerrar ambos descriptores y liberar el buffer.
 * =========================================================================*/

int sys_smart_copy(const char *src_path,
                   const char *dst_path,
                   uint32_t    flags,
                   sc_stats_t *stats)
{
    int      fd_src  = -1;
    int      fd_dst  = -1;
    char    *buffer  = NULL;
    ssize_t  n_read;
    ssize_t  n_written;
    ssize_t  total_written;
    off_t    bytes_read    = 0;
    off_t    bytes_written = 0;
    uint32_t block_count   = 0;
    uint32_t buf_size;
    int      ret = SC_OK;

    /* ------------------------------------------------------------------
     * 1. Validar punteros de entrada
     * ------------------------------------------------------------------ */
    if (src_path == NULL || dst_path == NULL) {
        errno = EINVAL;
        sc_perror("sys_smart_copy: puntero NULL recibido");
        return SC_ERROR;
    }

    /* ------------------------------------------------------------------
     * 2. Inicializar syslog si el log está activo
     * ------------------------------------------------------------------ */
    if (flags & SC_FLAG_LOG) {
        openlog("smart_copy", LOG_PID | LOG_CONS, LOG_USER);
        log_event(LOG_INFO,
                  "Iniciando copia: '%s' → '%s'  flags=0x%02X",
                  src_path, dst_path, flags);
    }

    /* ------------------------------------------------------------------
     * 3. Verificar permisos antes de abrir descriptores
     *    (equivale a una comprobación en el VFS del kernel)
     * ------------------------------------------------------------------ */
    if (flags & SC_FLAG_CHECK_PERMS) {
        if (access(src_path, R_OK) != 0) {
            /* errno ya tiene ENOENT o EACCES */
            sc_perror("access() origen");
            ret = SC_ERROR;
            goto cleanup;
        }
        /* Verificar si el directorio destino es escribible es complejo;
         * aquí verificamos si el archivo destino existe y es escribible. */
        if (access(dst_path, W_OK) != 0 && errno != ENOENT) {
            sc_perror("access() destino");
            ret = SC_ERROR;
            goto cleanup;
        }
    }

    /* ------------------------------------------------------------------
     * 4. Abrir archivo fuente (solo lectura)
     * ------------------------------------------------------------------ */
    fd_src = open(src_path, O_RDONLY);
    if (fd_src < 0) {
        sc_perror("open() origen");
        ret = SC_ERROR;
        goto cleanup;
    }

    /* ------------------------------------------------------------------
     * 5. Abrir/crear archivo destino
     *    Modo 0644: rw-r--r-- (igual a cp por defecto)
     * ------------------------------------------------------------------ */
    fd_dst = open(dst_path,
                  O_WRONLY | O_CREAT | O_TRUNC,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd_dst < 0) {
        sc_perror("open() destino");
        ret = SC_ERROR;
        goto cleanup;
    }

    /* ------------------------------------------------------------------
     * 6. Elegir tamaño de buffer según flags
     * ------------------------------------------------------------------ */
    buf_size = (flags & SC_FLAG_LARGE_BUF) ? SC_BUFFER_SIZE_LARGE
                                            : SC_BUFFER_SIZE;

    /*
     * malloc() en espacio de usuario.  En un módulo de kernel real se
     * usaría kmalloc(buf_size, GFP_KERNEL) con la zona de memoria correcta.
     */
    buffer = (char *)malloc(buf_size);
    if (buffer == NULL) {
        errno = ENOMEM;
        sc_perror("malloc() buffer");
        ret = SC_ERROR;
        goto cleanup;
    }

    /* ------------------------------------------------------------------
     * 7. Bucle de transferencia: read(2) → write(2)
     *
     *    Cada iteración realiza:
     *      - Una syscall read  →  kernel copia datos de page cache al buffer.
     *      - Una syscall write →  kernel copia buffer a page cache del destino.
     *    Esto implica DOS context switches por bloque (user→kernel→user).
     *    stdio los amortigua con su propio buffer en espacio de usuario,
     *    reduciendo la frecuencia de los context switches para archivos
     *    pequeños (ver análisis en reporte.pdf).
     * ------------------------------------------------------------------ */
    while ((n_read = read(fd_src, buffer, buf_size)) > 0) {

        bytes_read += n_read;
        total_written = 0;

        /*
         * El write(2) puede devolver menos bytes que los solicitados
         * (escritura parcial).  Debemos reintentar hasta escribir todo
         * o detectar un error real (ENOSPC, EIO, etc.).
         */
        while (total_written < n_read) {
            n_written = write(fd_dst,
                              buffer + total_written,
                              (size_t)(n_read - total_written));
            if (n_written < 0) {
                if (errno == ENOSPC) {
                    sc_perror("write() destino: disco lleno");
                } else {
                    sc_perror("write() destino: error de I/O");
                    errno = EIO;
                }
                ret = SC_ERROR;
                goto cleanup;
            }
            total_written  += n_written;
            bytes_written  += n_written;
        }

        block_count++;
    }

    /* read() devolvió 0 (EOF) o -1 (error) */
    if (n_read < 0) {
        sc_perror("read() origen");
        errno = EIO;
        ret = SC_ERROR;
        goto cleanup;
    }

    /* ------------------------------------------------------------------
     * 8. fsync() – garantiza que los datos estén en disco físico
     *    (equivale a filemap_write_and_wait en el kernel)
     * ------------------------------------------------------------------ */
    if (flags & SC_FLAG_SYNC) {
        if (fsync(fd_dst) != 0) {
            sc_perror("fsync() destino");
            ret = SC_ERROR;
            goto cleanup;
        }
    }

    /* ------------------------------------------------------------------
     * 9. Rellenar estadísticas
     * ------------------------------------------------------------------ */
    if (stats != NULL) {
        stats->bytes_read    = bytes_read;
        stats->bytes_written = bytes_written;
        stats->block_count   = block_count;
        stats->buffer_size   = buf_size;
    }

    if (flags & SC_FLAG_LOG) {
        log_event(LOG_INFO,
                  "Copia completada: %lld bytes en %u bloque(s). buf=%u B",
                  (long long)bytes_written, block_count, buf_size);
    }

/* ------------------------------------------------------------------
 * 10. Limpieza garantizada (goto cleanup centraliza todos los cierres)
 * ------------------------------------------------------------------ */
cleanup:
    if (buffer != NULL) {
        free(buffer);
        buffer = NULL;
    }
    if (fd_src >= 0) {
        close(fd_src);
    }
    if (fd_dst >= 0) {
        close(fd_dst);
    }
    if (flags & SC_FLAG_LOG) {
        closelog();
    }

    return ret;
}

/* =========================================================================
 * stdio_copy  –  implementación de referencia con stdio.h
 * =========================================================================
 *
 * Usa FILE* (fopen/fread/fwrite/fclose).  La principal diferencia frente a
 * sys_smart_copy es que stdio mantiene un buffer en ESPACIO DE USUARIO
 * (típicamente 8 KB en glibc).  Esto reduce la frecuencia de syscalls
 * read/write y por lo tanto el número de context switches, lo que lo hace
 * MÁS EFICIENTE para archivos pequeños.  Para archivos grandes, la ventaja
 * se reduce porque ambos métodos terminan saturando el ancho de banda de I/O.
 * =========================================================================*/

int stdio_copy(const char *src_path, const char *dst_path)
{
    FILE   *fp_src   = NULL;
    FILE   *fp_dst   = NULL;
    char    buf[SC_BUFFER_SIZE];   /* Buffer local en stack (4 KiB)  */
    size_t  n_read;
    size_t  n_written;
    int     ret = SC_OK;

    if (src_path == NULL || dst_path == NULL) {
        errno = EINVAL;
        sc_perror("stdio_copy: puntero NULL");
        return SC_ERROR;
    }

    fp_src = fopen(src_path, "rb");
    if (fp_src == NULL) {
        sc_perror("fopen() origen");
        return SC_ERROR;
    }

    fp_dst = fopen(dst_path, "wb");
    if (fp_dst == NULL) {
        sc_perror("fopen() destino");
        fclose(fp_src);
        return SC_ERROR;
    }

    /*
     * fread/fwrite usan el buffer interno de FILE*.
     * Cuando el buffer de FILE* se llena, glibc emite una syscall write()
     * de forma automática.  Para archivos muy pequeños (< 8 KB) es posible
     * que ni siquiera se necesite una syscall, reduciendo el overhead.
     */
    while ((n_read = fread(buf, 1, sizeof(buf), fp_src)) > 0) {
        n_written = fwrite(buf, 1, n_read, fp_dst);
        if (n_written != n_read) {
            sc_perror("fwrite() destino");
            errno = ENOSPC;   /* Causa más probable de escritura incompleta */
            ret = SC_ERROR;
            break;
        }
    }

    /* Verificar si el bucle terminó por error de lectura */
    if (ret == SC_OK && ferror(fp_src)) {
        sc_perror("fread() origen");
        ret = SC_ERROR;
    }

    fclose(fp_src);
    fclose(fp_dst);

    return ret;
}
