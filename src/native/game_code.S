.section __MRCODE,__text,regular,pure_instructions
.p2align 12
.globl _mr_game_code
_mr_game_code:
.incbin "build/minion_code.bin"
.globl _mr_game_code_end
_mr_game_code_end:
