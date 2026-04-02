/**
 * smart_copy.h
 * ---------------------------------------------------------------------------
 * Interfaz pública del Smart Backup Kernel-Space Utility.
 *
 * Simula la firma de una syscall de copia optimizada que operaría en espacio
 * de kernel.  En Linux real esto viviría en include/linux/syscalls.h y sería
 * invocada mediante un número de syscall registrado en la tabla del kernel.
 *
 * Autores : [Tu nombre]
 * Materia  : Sistemas Operativos  –  Universidad EAFIT
 * ---------------------------------------------------------------------------
 */

#ifndef SMART_COPY_H
#define SMART_COPY_H

#include <sys/types.h>   /* ssize_t, off_t  */
#include <stdint.h>      /* uint32_t         */

/* =========================================================================
 * CONSTANTES
 * =========================================================================*/

/** Tamaño de página en bytes (coincide con PAGE_SIZE del kernel x86-64). */
#define SC_PAGE_SIZE        4096

/** Número máximo de bytes que se transfieren en un único bloque.
 *  Usar múltiplos de PAGE_SIZE reduce el número de context switches. */
#define SC_BUFFER_SIZE      SC_PAGE_SIZE          /* 4 KiB  */
#define SC_BUFFER_SIZE_LARGE (SC_PAGE_SIZE * 256) /* 1 MiB  */

/** Valor de retorno en caso de éxito (al estilo de syscalls POSIX). */
#define SC_OK               0

/** Valor de retorno en caso de error (errno contiene el detalle). */
#define SC_ERROR           -1

/* =========================================================================
 * FLAGS  –  controlan el comportamiento de sys_smart_copy()
 * =========================================================================
 *
 * Se combinan con OR bit a bit:
 *   sys_smart_copy(src, dst, SC_FLAG_VERIFY | SC_FLAG_LOG, NULL)
 */

/** Sin opciones adicionales; copia plana. */
#define SC_FLAG_NONE        0x00u

/** Verifica permisos de lectura en origen y escritura en destino antes de
 *  abrir los descriptores.  Equivale a una llamada a access(2) previa. */
#define SC_FLAG_CHECK_PERMS 0x01u

/** Registra la operación en el log del sistema (stderr en modo userspace).
 *  En kernel real escribiría con printk(KERN_INFO ...). */
#define SC_FLAG_LOG         0x02u

/** Usa el tamaño de buffer ampliado (SC_BUFFER_SIZE_LARGE) para archivos
 *  grandes.  Si no se activa, usa SC_BUFFER_SIZE (4 KiB). */
#define SC_FLAG_LARGE_BUF   0x04u

/** Fuerza fsync() al cerrar el destino para garantizar persistencia en disco.
 *  Relevante en entornos con caché de escritura agresiva. */
#define SC_FLAG_SYNC        0x08u

/* =========================================================================
 * TIPOS AUXILIARES
 * =========================================================================*/

/**
 * sc_stats_t  –  estadísticas de la última copia realizada.
 * Se rellena por referencia al finalizar sys_smart_copy().
 */
typedef struct {
    off_t    bytes_read;      /**< Bytes leídos del origen.            */
    off_t    bytes_written;   /**< Bytes escritos en el destino.       */
    uint32_t block_count;     /**< Número de bloques transferidos.      */
    uint32_t buffer_size;     /**< Tamaño de buffer usado (bytes).      */
} sc_stats_t;

/* =========================================================================
 * PROTOTIPOS PÚBLICOS
 * =========================================================================*/

/**
 * sys_smart_copy  –  copia optimizada simulando una syscall de kernel.
 *
 * @param src_path   Ruta absoluta o relativa del archivo fuente.
 * @param dst_path   Ruta del archivo destino (se crea si no existe).
 * @param flags      Combinación OR de SC_FLAG_*.
 * @param stats      Puntero a sc_stats_t donde se depositan métricas;
 *                   puede ser NULL si no se necesitan estadísticas.
 *
 * @return SC_OK (0) en éxito; SC_ERROR (-1) con errno configurado en error.
 *
 * Errores posibles (errno):
 *   ENOENT       – src_path no existe.
 *   EACCES       – permisos insuficientes.
 *   ENOSPC       – espacio en disco agotado.
 *   ENOMEM       – fallo al asignar el buffer interno.
 *   EIO          – error de I/O durante lectura/escritura.
 */
int sys_smart_copy(const char *src_path,
                   const char *dst_path,
                   uint32_t    flags,
                   sc_stats_t *stats);

/**
 * stdio_copy  –  implementación de referencia usando stdio.h (fread/fwrite).
 * Se usa para comparar tiempos frente a sys_smart_copy.
 *
 * @param src_path  Ruta del archivo fuente.
 * @param dst_path  Ruta del archivo destino.
 * @return SC_OK en éxito; SC_ERROR en error.
 */
int stdio_copy(const char *src_path, const char *dst_path);

/**
 * sc_perror  –  imprime un mensaje de error con contexto y errno.
 * Wrapper sobre perror() con prefijo uniforme.
 *
 * @param context  Nombre de la operación que falló.
 */
void sc_perror(const char *context);

#endif /* SMART_COPY_H */
