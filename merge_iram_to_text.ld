/* Merge .iram1* sections into .text for dynamic module builds.
 * ESP-IDF places interrupt-safe code in .iram1, but as a dynamic module
 * loaded from PSRAM, flash cache safety is irrelevant — all code is in RAM.
 * Merging avoids the need for --rename-section objcopy hacks. */
SECTIONS
{
  .text :
  {
    *(.iram1 .iram1.*)
  }
}
INSERT AFTER .text;
