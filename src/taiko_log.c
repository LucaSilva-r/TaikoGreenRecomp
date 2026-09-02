/* Windows release logging gate.
 *
 * The runtime still contains extensive bring-up diagnostics.  Sending those
 * through Wine's console is expensive, and redirecting the handles to NUL
 * still pays the formatting and stdio-locking cost.  The MinGW link wraps the
 * common formatted/output entry points below so quiet mode returns before any
 * of that work happens.  File I/O is never suppressed: stream-taking wrappers
 * only intercept stdout and stderr.
 */
#ifdef _WIN32

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>

static int s_taiko_log_quiet;

static int taiko_env_enabled(const char* value)
{
    if (!value || !*value) return 0;
    return strcmp(value, "0") != 0 && _stricmp(value, "false") != 0 &&
           _stricmp(value, "off") != 0 && _stricmp(value, "no") != 0;
}

void taiko_log_configure(int standalone)
{
    const char* setting = getenv("TAIKO_QUIET_LOG");
    s_taiko_log_quiet = setting ? taiko_env_enabled(setting) : standalone;
}

/* Run before ordinary project constructors, several of which emit registration
 * diagnostics.  Parsing the OS command line here lets the standalone release
 * start quiet without hiding those messages from an explicit-ELF dev run. */
__attribute__((constructor(101)))
static void taiko_log_configure_early(void)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    taiko_log_configure(!argv || argc < 2);
    if (argv) LocalFree(argv);
}

static int taiko_suppress_stream(FILE* stream)
{
    return s_taiko_log_quiet && (stream == stdout || stream == stderr);
}

int __mingw_vfprintf(FILE* stream, const char* format, va_list args);
int __mingw_vprintf(const char* format, va_list args);

int __wrap___mingw_fprintf(FILE* stream, const char* format, ...)
{
    if (taiko_suppress_stream(stream)) return 0;
    va_list args;
    va_start(args, format);
    const int result = __mingw_vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int __wrap___mingw_printf(const char* format, ...)
{
    if (s_taiko_log_quiet) return 0;
    va_list args;
    va_start(args, format);
    const int result = __mingw_vprintf(format, args);
    va_end(args);
    return result;
}

int __wrap_fprintf(FILE* stream, const char* format, ...)
{
    if (taiko_suppress_stream(stream)) return 0;
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int __wrap_printf(const char* format, ...)
{
    if (s_taiko_log_quiet) return 0;
    va_list args;
    va_start(args, format);
    const int result = vprintf(format, args);
    va_end(args);
    return result;
}

int __real_fputc(int ch, FILE* stream);
int __real_fputs(const char* text, FILE* stream);
size_t __real_fwrite(const void* data, size_t size, size_t count, FILE* stream);
int __real_putchar(int ch);
int __real_puts(const char* text);

int __wrap_fputc(int ch, FILE* stream)
{
    return taiko_suppress_stream(stream) ? (unsigned char)ch
                                         : __real_fputc(ch, stream);
}

int __wrap_fputs(const char* text, FILE* stream)
{
    return taiko_suppress_stream(stream) ? 0 : __real_fputs(text, stream);
}

size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* stream)
{
    return taiko_suppress_stream(stream) ? count
                                         : __real_fwrite(data, size, count, stream);
}

int __wrap_putchar(int ch)
{
    return s_taiko_log_quiet ? (unsigned char)ch : __real_putchar(ch);
}

int __wrap_puts(const char* text)
{
    return s_taiko_log_quiet ? 0 : __real_puts(text);
}

#endif
