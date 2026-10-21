/*
 * vita-elf-create appends the module's SCE data after the code segment and
 * fails ("segment 1 overlaps") if the data segment starts too soon after
 * it. The data segment begins at the next 64 KB boundary after the code,
 * so whether there was room depended on where the code happened to end.
 * Moving the location counter 8 KB on before that alignment always leaves
 * at least 8 KB, whatever the code size.
 */
SECTIONS
{
  . = . + 0x2000;
}
INSERT AFTER .eh_frame;
