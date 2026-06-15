/* tools/payload.ld */
OUTPUT_ARCH(riscv)
ENTRY(payload_main)

SECTIONS
{
    /* Базова адреса 0x0. Код буде повністю PIC (Position Independent Code). */
    . = 0x0;

    /* Секція КОДУ ТА КОНСТАНТ (Тільки читання) */
    .text : ALIGN(4)
    {
        *(.text.entry)          /* Головна точка входу payload_main ПЕРШОЮ */
        *(.text)
        *(.text.*)
        *(.rodata)              /* Константи (наприклад, рядки "Hello") */
        *(.rodata.*)
        *(.srodata)
        *(.srodata.*)
    }

    /* Секція ЗМІННИХ (Читання та Запис)
       УВАГА: Якщо Payload працює з Flash (MMU XIP), запис сюди викличе Crash!
       Ці змінні будуть працювати тільки якщо Payload завантажено в RAM. */
    .data : ALIGN(4)
    {
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.sdata.*)
    }

    /* Секція НЕІНІЦІАЛІЗОВАНИХ ЗМІННИХ (Повинні бути нулями) */
    .bss : ALIGN(4)
    {
        *(.bss)
        *(.bss.*)
        *(.sbss)
        *(.sbss.*)
        *(COMMON)
    }

    /* Відсікаємо всі непотрібні ELF-секції для мінімізації розміру */
    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.debug_*)
    }
}
