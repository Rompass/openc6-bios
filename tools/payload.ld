/* tools/payload.ld */
OUTPUT_ARCH(riscv)
ENTRY(payload_main)

SECTIONS
{
    . = 0x0;

    /* Секція КОДУ ТА КОНСТАНТ (Тільки читання) */
    .text : ALIGN(4)
    {
        *(.text.entry)          
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.srodata)
        *(.srodata.*)
    }

    .data : ALIGN(4)
    {
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.sdata.*)
    }

    .bss : ALIGN(4)
    {
        *(.bss)
        *(.bss.*)
        *(.sbss)
        *(.sbss.*)
        *(COMMON)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.debug_*)
    }
}
