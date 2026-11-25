/**
 * O que depende do sistema operacional, atras de uma interface so.
 *
 *     host_os_posix.c   Linux (e, no futuro, macOS)
 *     host_os_win32.c   Windows (MinGW-w64)
 *
 * Cada arquivo se anula inteiro no sistema errado, entao os dois podem estar
 * na lista de fontes. Codigo novo que precise de algo do sistema (caminho do
 * executavel, sinais, excecoes, threads do processo) entra aqui, nao espalhado
 * pelos hooks.
 */
#ifndef DW4_HOST_OS_H
#define DW4_HOST_OS_H

#include <stddef.h>
#include <stdint.h>

/* Pasta do executavel, absoluta e sem barra no fim. 0 se nao deu para saber. */
int dw4_os_exe_dir(char *out, size_t n);

/* Caminho absoluto e resolvido de `in` (que tem de existir). 0 se falhou. */
int dw4_os_abspath(const char *in, char *out, size_t n);

/* Cria a pasta se nao existir (so o ultimo nivel). */
void dw4_os_mkdir(const char *path);

/* Define a variavel de ambiente so se o usuario nao definiu. */
void dw4_os_env_default(const char *name, const char *value);

/* Defaults de ambiente que so fazem sentido neste sistema (chamado junto com
 * os do port, antes de qualquer init). */
void dw4_os_env_defaults(void);

/* Tempo de processador do SISTEMA inteiro ate agora, somados todos os nucleos:
 * *busy = segundos ocupados, *total = ocupados + ociosos. A diferenca entre
 * duas leituras da a ocupacao media dos nucleos (busy/total). 0 se falhou. */
int dw4_os_cpu_times(double *busy, double *total);

/* Ocupacao da placa de video, 0..100, ou -1 quando o sistema nao informa.
 * E da placa inteira, nao so deste processo. Pode demorar alguns ms: nao
 * chamar da thread do renderizador. Linux: NVIDIA (NVML) e AMD (sysfs).
 * Windows: NVIDIA (NVML) e, nas outras, os contadores "GPU Engine" do sistema. */
int dw4_os_gpu_percent(void);

/* Falhas de acesso: primeiro os registradores de hardware emulados do toolkit
 * (APU, AC'97, OHCI); o que sobrar vira relatorio de crash no log. No POSIX
 * instala tambem o despejo por SIGUSR1 (ver dw4_dump_all_threads). */
void dw4_os_install_crash_handlers(void);

/* Partes do relatorio de crash comuns aos sistemas (main.c). Rodam na thread
 * que falhou, porque os registradores do guest sao thread-local. */
void dw4_crash_report_regs(void);          /* eax..esp do guest */
void dw4_crash_report_guest_stack(void);   /* retornos na pilha do guest + ICALLs */

#endif /* DW4_HOST_OS_H */
