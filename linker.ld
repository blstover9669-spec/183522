/* NovaOS kernel linker script: loaded by a Multiboot loader at 1 MiB, identity mapped. */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .text ALIGN(4K) : {
        KEEP(*(.multiboot))
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    /* Application registry: every REGISTER_APP() drops a pointer here. */
    .apps ALIGN(4) : {
        __apps_start = .;
        KEEP(*(.apps))
        __apps_end = .;
    }

    /* Boot-time init hooks: REGISTER_INIT(stage, fn). */
    .initcalls ALIGN(4) : {
        __initcalls_start = .;
        KEEP(*(.initcalls))
        __initcalls_end = .;
    }

    /* Kernel symbol table for stack traces (tools/gensyms.py). Kept after all code
       and read-only data so generating it does not move any function. */
    .ksyms ALIGN(4) : {
        KEEP(*(.ksyms))
    }

    . = ALIGN(4K);
    __ro_end = .;              /* [__kernel_start, __ro_end) is mapped read-only */

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    .bss ALIGN(4K) (NOLOAD) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
