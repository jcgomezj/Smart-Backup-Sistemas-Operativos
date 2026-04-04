/**
 * smart_copy.h
 * ---------------------------------------------------------------------------
 * Definiciones, constantes y prototipos del Smart Backup Kernel-Space Utility.
 *
 * Extiende el código base del profesor agregando:
 *   - Constante PAGE_SIZE para el buffer de transferencia.
 *   - Flags de control combinables por OR bit a bit.
 *   - Struct sc_stats_t para métricas de rendimiento.
 *   - Prototipo de stdio_copy para la comparativa de rendimiento.
 *
 * Materia  : Sistemas Operativos – Universidad EAFIT
 * Autores  : Juan José Barón · Estefanía Ramírez · Juan Camilo Gómez Jiménez
 * ---------------------------------------------------------------------------
 */

#ifndef SMART_COPY_H
#define SMART_COPY_H

#include <sys/types.h>
#include <stdint.h>

/* =========================================================================
 * CONSTANTES
 * =========================================================================*/

/** Tamaño de buffer alineado a PAGE_SIZE del kernel x86-64 (4096 bytes).
 *  Usar este valor maximiza el throughput porque coincide con la granularidad
 *  mínima de gestión de memoria del kernel. */
#define BUFFER_SIZE      4096
#define SC_PAGE_SIZE     BUFFER_SIZE

/** Códigos de retorno al estilo POSIX */
#define SC_OK            0
#define SC_ERROR        -1

/* =========================================================================
 * FLAGS de control para copy_file()
 * =========================================================================
 * Se combinan con OR:  copy_file(src, dst, SC_FLAG_LOG | SC_FLAG_SYNC)
 */
#define SC_FLAG_NONE        0x00u  /**< Sin opciones adicionales            */
#define SC_FLAG_CHECK_PERMS 0x01u  /**< Verificar permisos con access(2)   */
#define SC_FLAG_LOG         0x02u  /**< Registrar en syslog/stderr          */
#define SC_FLAG_LARGE_BUF   0x04u  /**< Buffer ampliado de 1 MiB            */
#define SC_FLAG_SYNC        0x08u  /**< fsync() al cerrar el destino        */

/* =========================================================================
 * TIPOS
 * =========================================================================*/

/**
 * sc_stats_t — estadísticas de una operación de copia.
 * Se pasa por referencia a copy_file(); puede ser NULL.
 */
typedef struct {
    off_t    bytes_read;     /**< Bytes leídos del origen      */
    off_t    bytes_written;  /**< Bytes escritos en el destino */
    uint32_t block_count;    /**< Bloques de BUFFER_SIZE usados */
    uint32_t buffer_size;    /**< Tamaño de buffer utilizado   */
} sc_stats_t;

/* =========================================================================
 * PROTOTIPOS
 * =========================================================================*/

/** Copia un archivo usando syscalls POSIX directas (versión extendida). */
void copy_file(const char *src, const char *dest);

/** Copia un directorio recursivamente usando opendir/readdir. */
void copy_directory(const char *src, const char *dest);

/** Copia un archivo usando stdio.h (fread/fwrite) — referencia para benchmark. */
int stdio_copy(const char *src, const char *dest);

/** Imprime ayuda de uso. */
void print_help(const char *prog_name);

/** Silencia stderr durante el benchmark (0=normal, 1=silencioso). */
extern int sc_silent_mode;

#endif /* SMART_COPY_H */
