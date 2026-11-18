# Imports of work\apk\lib\armeabi\libStormGLOFT.so

DT_NEEDED (search order): liblog.so, libz.so, libm.so, libdl.so, libEGL.so, libGLESv1_CM.so, libGLESv2.so, libandroid.so, libc.so, libstdc++.so
Total imported symbols: 126

Grouping is by API family (old ELF imports carry no library name).
Objects marked [DATA] are variables, not functions: our shim must provide
memory with bionic's exact layout for them.

## libc  (78)
- `__assert2`
- `__errno`
- `__sF` [DATA]
- `__stack_chk_fail`
- `__stack_chk_guard` [DATA]
- `abort`
- `atoi`
- `basename`
- `cacheflush`
- `calloc`
- `clearerr`
- `close`
- `exit`
- `fclose`
- `feof`
- `ferror`
- `fgets`
- `fopen`
- `fprintf`
- `fputc`
- `fputs`
- `fread`
- `free`
- `fscanf`
- `fseek`
- `ftell`
- `fwrite`
- `getc`
- `getenv`
- `getpid`
- `localtime`
- `lrand48`
- `lseek`
- `malloc`
- `memalign`
- `memcmp`
- `memcpy`
- `mmap`
- `mprotect`
- `munmap`
- `open`
- `printf`
- `putchar`
- `puts`
- `qsort`
- `raise`
- `read`
- `readlink`
- `realloc`
- `snprintf`
- `sprintf`
- `srand48`
- `sscanf`
- `strcasecmp`
- `strcat`
- `strchr`
- `strcmp`
- `strcpy`
- `strcspn`
- `strdup`
- `strerror`
- `strftime`
- `strlen`
- `strncat`
- `strncmp`
- `strncpy`
- `strrchr`
- `strspn`
- `strstr`
- `strtod`
- `strtok`
- `strtol`
- `strtoll`
- `strtoul`
- `strtoull`
- `sysconf`
- `time`
- `vsnprintf`

## libm  (12)
- `ceilf`
- `cos`
- `exp`
- `exp2`
- `floor`
- `floorf`
- `ldexp`
- `log`
- `pow`
- `sin`
- `sqrtf`
- `truncf`

## GLES 1.1 (common to ES1/ES2)  (10)
- `glCompressedTexImage2D`
- `glCompressedTexSubImage2D`
- `glGetShaderiv`
- `glGetString`
- `glRenderbufferStorage`
- `glScissor`
- `glShaderSource`
- `glTexImage2D`
- `glTexSubImage2D`
- `glViewport`

## libc (pthreads / scheduling)  (10)
- `pthread_cond_broadcast`
- `pthread_cond_wait`
- `pthread_getspecific`
- `pthread_key_create`
- `pthread_key_delete`
- `pthread_mutex_destroy`
- `pthread_mutex_init`
- `pthread_mutex_lock`
- `pthread_mutex_unlock`
- `pthread_setspecific`

## libc (ARM EABI runtime helpers: soft-float / int division)  (9)
- `__aeabi_memclr`
- `__aeabi_memclr4`
- `__aeabi_memclr8`
- `__aeabi_memcpy`
- `__aeabi_memcpy4`
- `__aeabi_memcpy8`
- `__aeabi_memmove`
- `__aeabi_memset`
- `__aeabi_memset4`

## libdl  (3)
- `dlclose`
- `dlopen`
- `dlsym`

## libc (C++ ABI support)  (2)
- `__cxa_atexit`
- `__cxa_finalize`

## C++ unwinder (libgcc/libstdc++)  (1)
- `__gnu_Unwind_Find_exidx`

## liblog  (1)
- `__android_log_print`
